#include "ssu_api.h"
#include "ssu_dataplane.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RESPONSE_SIZE 8192U

typedef enum {
    SMOKE_IO_VERIFY = 0,
    SMOKE_IO_WRITE_ONLY = 1,
    SMOKE_IO_READ_ONLY = 2,
} smoke_io_mode_t;

typedef struct {
    const char *logical_dev;
    const char *host_id;
    uint64_t alloc_size;
    uint64_t bytes;
    int bytes_set;
    int use_sdk;
    int do_alloc;
    int do_mount;
    int do_io;
    int do_unmount;
    int do_release;
    ssu_reliability_t reliability;
    smoke_io_mode_t io_mode;
} smoke_options_t;

static int send_all(int fd, const char *buf, size_t n)
{
    size_t written = 0;

    while (written < n) {
        ssize_t rc = write(fd, buf + written, n - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        written += (size_t)rc;
    }

    return 1;
}

static int manager_request(const char *command, char *response, size_t n)
{
    const char *request_path = getenv("SSU_MGR_SOCKET");
    char response_path[128];
    char request[512];
    int request_fd;
    int response_fd;
    size_t used = 0;

    if (request_path == NULL || request_path[0] == '\0') {
        fputs("SSU_MGR_SOCKET is required\n", stderr);
        return 0;
    }

    snprintf(response_path, sizeof(response_path),
             "/tmp/ssu-smoke-response-%ld.fifo", (long)getpid());
    unlink(response_path);
    if (mkfifo(response_path, 0600) != 0) {
        perror("mkfifo");
        return 0;
    }

    snprintf(request, sizeof(request), "%s %s\n", response_path, command);
    request_fd = open(request_path, O_WRONLY | O_NONBLOCK);
    if (request_fd < 0) {
        perror("open manager fifo");
        unlink(response_path);
        return 0;
    }

    if (!send_all(request_fd, request, strlen(request))) {
        perror("write");
        close(request_fd);
        unlink(response_path);
        return 0;
    }
    close(request_fd);

    response_fd = open(response_path, O_RDONLY);
    if (response_fd < 0) {
        perror("open response fifo");
        unlink(response_path);
        return 0;
    }

    while (used + 1 < n) {
        ssize_t rc = read(response_fd, response + used, n - used - 1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            close(response_fd);
            unlink(response_path);
            return 0;
        }
        if (rc == 0) {
            break;
        }
        used += (size_t)rc;
    }

    response[used] = '\0';
    close(response_fd);
    unlink(response_path);
    return 1;
}

static int find_logdev_manager(const char *logical_dev, ssu_logdev_info_t *out)
{
    char response[RESPONSE_SIZE];
    char *line;
    char *save = NULL;

    if (!manager_request("QUERY logdev", response, sizeof(response))) {
        return 0;
    }

    if (strncmp(response, "OK\n", 3) != 0) {
        fputs(response, stderr);
        return 0;
    }

    line = strtok_r(response + 3, "\n", &save);
    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        ssu_logdev_info_t candidate;
        unsigned long long logical_offset;
        unsigned long long length;
        unsigned long long phys_sector;

        memset(&candidate, 0, sizeof(candidate));
        if (sscanf(line, "%63s %63s %63s %llu %llu %63s %31s %llu",
                   candidate.logical_dev,
                   candidate.host_id,
                   candidate.allocate_id,
                   &logical_offset,
                   &length,
                   candidate.phys_dev,
                   candidate.ns_id,
                   &phys_sector) != 8) {
            continue;
        }

        if (strcmp(candidate.logical_dev, logical_dev) == 0) {
            candidate.logical_offset = (uint64_t)logical_offset;
            candidate.length = (uint64_t)length;
            candidate.phys_sector = (uint64_t)phys_sector;
            *out = candidate;
            return 1;
        }
    }

    return 0;
}

static int find_logdev_sdk(const char *logical_dev, ssu_logdev_info_t *out)
{
    ssu_query_req_t req = {};
    ssu_logdev_info_t *logdevs;
    uint32_t count = 0;
    uint32_t i;
    ssu_err_t err;

    req.type = SSU_QUERY_LOGDEV;
    req.logical_dev = logical_dev;
    err = ssu_resource_query(&req, NULL, sizeof(ssu_logdev_info_t), &count);
    if (err != SSU_OK && err != SSU_ERR_BUFFER_TOO_SMALL) {
        fprintf(stderr, "query logdev failed: %d\n", err);
        return 0;
    }

    if (count == 0) {
        return 0;
    }

    logdevs = (ssu_logdev_info_t *)calloc(count, sizeof(*logdevs));
    if (logdevs == NULL) {
        fputs("out of memory\n", stderr);
        return 0;
    }

    err = ssu_resource_query(&req, logdevs, sizeof(*logdevs), &count);
    if (err != SSU_OK) {
        fprintf(stderr, "query logdev failed: %d\n", err);
        free(logdevs);
        return 0;
    }

    for (i = 0; i < count; i++) {
        if (strcmp(logdevs[i].logical_dev, logical_dev) == 0) {
            *out = logdevs[i];
            free(logdevs);
            return 1;
        }
    }

    free(logdevs);
    return 0;
}

static void fill_pattern(unsigned char *buf, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        buf[i] = (unsigned char)((i * 31U + 7U) & 0xffU);
    }
}

static int run_pattern_io(const ssu_logdev_info_t *logdev, uint64_t bytes,
                          smoke_io_mode_t mode)
{
    unsigned char *pattern;
    unsigned char *readback = NULL;
    int rc = 1;

    if (bytes == 0 || bytes > (1ULL << 20) || bytes > logdev->length) {
        fprintf(stderr, "invalid smoke io length\n");
        return 1;
    }

    pattern = (unsigned char *)malloc((size_t)bytes);
    if (mode != SMOKE_IO_WRITE_ONLY) {
        readback = (unsigned char *)malloc((size_t)bytes);
    }
    if (pattern == NULL || (mode != SMOKE_IO_WRITE_ONLY &&
                            readback == NULL)) {
        fputs("out of memory\n", stderr);
        goto out;
    }

    fill_pattern(pattern, (size_t)bytes);

    if (mode != SMOKE_IO_READ_ONLY &&
        ssu_dataplane_write(logdev, 0, pattern, (size_t)bytes) != SSU_OK) {
        fputs("pattern write failed\n", stderr);
        goto out;
    }

    if (mode != SMOKE_IO_WRITE_ONLY) {
        memset(readback, 0, (size_t)bytes);
        if (ssu_dataplane_read(logdev, 0, readback,
                               (size_t)bytes) != SSU_OK) {
            fputs("pattern read failed\n", stderr);
            goto out;
        }

        if (memcmp(pattern, readback, (size_t)bytes) != 0) {
            fputs("pattern mismatch\n", stderr);
            goto out;
        }
    }

    if (mode == SMOKE_IO_WRITE_ONLY) {
        printf("ssu_smoke wrote %s %llu bytes via %s\n",
               logdev->logical_dev,
               (unsigned long long)bytes,
               logdev->phys_dev);
    } else if (mode == SMOKE_IO_READ_ONLY) {
        printf("ssu_smoke read ok %s %llu bytes via %s\n",
               logdev->logical_dev,
               (unsigned long long)bytes,
               logdev->phys_dev);
    } else {
        printf("ssu_smoke ok %s %llu bytes via %s\n",
               logdev->logical_dev,
               (unsigned long long)bytes,
               logdev->phys_dev);
    }
    rc = 0;

out:
    free(pattern);
    free(readback);
    return rc;
}

static int alloc_with_extent_retry(const ssu_alloc_req_t *req,
                                   ssu_alloc_result_t *out)
{
    ssu_alloc_extent_t *extents = NULL;
    uint32_t extent_count = 0;
    ssu_err_t err;

    err = ssu_resource_alloc(req, out, NULL, &extent_count);
    if (err != SSU_ERR_BUFFER_TOO_SMALL || extent_count == 0) {
        fprintf(stderr, "alloc sizing failed: %d\n", err);
        return 0;
    }

    extents = (ssu_alloc_extent_t *)calloc(extent_count, sizeof(*extents));
    if (extents == NULL) {
        fputs("out of memory\n", stderr);
        return 0;
    }

    err = ssu_resource_alloc(req, out, extents, &extent_count);
    free(extents);
    if (err != SSU_OK) {
        fprintf(stderr, "alloc failed: %d\n", err);
        return 0;
    }

    return 1;
}

static int run_sdk_flow(const smoke_options_t *opts)
{
    ssu_alloc_result_t alloc_result;
    ssu_mount_req_t mount_req = {};
    ssu_logdev_info_t logdev;
    int allocated = 0;
    int mounted = 0;
    int rc = 1;

    memset(&alloc_result, 0, sizeof(alloc_result));
    if (opts->do_alloc) {
        ssu_alloc_req_t req = {};

        req.size_bytes = opts->alloc_size;
        req.reliability = opts->reliability;
        req.share_type = SSU_SHARE_EXCLUSIVE;
        req.map_dir = SSU_MAP_DIR_FORWARD;
        req.tenant = "ssu-smoke";

        if (req.size_bytes == 0) {
            fputs("--alloc requires --size BYTES\n", stderr);
            return 1;
        }

        if (!alloc_with_extent_retry(&req, &alloc_result)) {
            return 1;
        }
        allocated = 1;
        printf("allocated %s\n", alloc_result.allocate_id);
    }

    mount_req.host_id = opts->host_id;
    if (opts->do_mount) {
        if (!allocated || opts->logical_dev == NULL) {
            fputs("--mount requires --alloc and --dev /dev/ssuN\n", stderr);
            goto cleanup;
        }

        mount_req.allocate_id = alloc_result.allocate_id;
        snprintf(mount_req.logical_dev, sizeof(mount_req.logical_dev), "%s",
                 opts->logical_dev);
        if (ssu_resource_mount(&mount_req) != SSU_OK) {
            fputs("mount failed\n", stderr);
            goto cleanup;
        }
        mounted = 1;
        printf("mounted %s\n", opts->logical_dev);
    }

    if (opts->do_io) {
        memset(&logdev, 0, sizeof(logdev));
        if (!mounted || !find_logdev_sdk(opts->logical_dev, &logdev)) {
            fprintf(stderr, "logdev not found: %s\n", opts->logical_dev);
            goto cleanup;
        }

        if (run_pattern_io(&logdev, opts->bytes, opts->io_mode) != 0) {
            goto cleanup;
        }
    }

    rc = 0;

cleanup:
    if (opts->do_unmount && mounted) {
        ssu_err_t err = ssu_resource_unmount(opts->logical_dev);
        if (err != SSU_OK) {
            fprintf(stderr, "unmount failed: %d\n", err);
            rc = 1;
        } else {
            mounted = 0;
            printf("unmounted %s\n", opts->logical_dev);
        }
    }

    if (opts->do_release && allocated) {
        ssu_err_t err = ssu_resource_release(alloc_result.allocate_id);
        if (err != SSU_OK) {
            fprintf(stderr, "release failed: %d\n", err);
            rc = 1;
        } else {
            allocated = 0;
            printf("released %s\n", alloc_result.allocate_id);
        }
    }

    return rc;
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (s == NULL || out == NULL) {
        return 0;
    }

    errno = 0;
    value = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return 0;
    }

    *out = (uint64_t)value;
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: ssu_smoke /dev/ssuN [--bytes N]\n"
            "       ssu_smoke /dev/ssuN [--bytes N] [--write-only|--read-only]\n"
            "       ssu_smoke --alloc --size BYTES --stripe --mount --dev /dev/ssuN --io --pattern verify --unmount --release\n");
}

int main(int argc, char **argv)
{
    smoke_options_t opts = {};
    ssu_logdev_info_t logdev;
    int i;

    opts.host_id = "local";
    opts.bytes = 65536;
    opts.reliability = SSU_RELIABILITY_STRIPE;
    opts.io_mode = SMOKE_IO_VERIFY;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            opts.logical_dev = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--bytes") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &opts.bytes)) {
                usage();
                return 1;
            }
            opts.bytes_set = 1;
            continue;
        }

        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &opts.alloc_size)) {
                usage();
                return 1;
            }
            opts.use_sdk = 1;
            continue;
        }

        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            opts.host_id = argv[++i];
            opts.use_sdk = 1;
            continue;
        }

        if (strcmp(argv[i], "--alloc") == 0) {
            opts.use_sdk = 1;
            opts.do_alloc = 1;
            continue;
        }

        if (strcmp(argv[i], "--mount") == 0) {
            opts.use_sdk = 1;
            opts.do_mount = 1;
            continue;
        }

        if (strcmp(argv[i], "--io") == 0) {
            opts.use_sdk = 1;
            opts.do_io = 1;
            continue;
        }

        if (strcmp(argv[i], "--unmount") == 0) {
            opts.use_sdk = 1;
            opts.do_unmount = 1;
            continue;
        }

        if (strcmp(argv[i], "--release") == 0) {
            opts.use_sdk = 1;
            opts.do_release = 1;
            continue;
        }

        if (strcmp(argv[i], "--write-only") == 0) {
            opts.io_mode = SMOKE_IO_WRITE_ONLY;
            continue;
        }

        if (strcmp(argv[i], "--read-only") == 0) {
            opts.io_mode = SMOKE_IO_READ_ONLY;
            continue;
        }

        if (strcmp(argv[i], "--stripe") == 0) {
            opts.reliability = SSU_RELIABILITY_STRIPE;
            opts.use_sdk = 1;
            continue;
        }

        if (strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) {
            opts.use_sdk = 1;
            if (strcmp(argv[++i], "verify") != 0) {
                usage();
                return 1;
            }
            continue;
        }

        if (opts.logical_dev == NULL) {
            opts.logical_dev = argv[i];
            continue;
        }

        usage();
        return 1;
    }

    if (!opts.bytes_set && opts.alloc_size > 0 && opts.alloc_size < opts.bytes) {
        opts.bytes = opts.alloc_size;
    }

    if (opts.use_sdk) {
        if (!opts.do_alloc && !opts.do_mount && !opts.do_io &&
            !opts.do_unmount && !opts.do_release) {
            usage();
            return 1;
        }
        return run_sdk_flow(&opts);
    }

    if (opts.logical_dev == NULL || opts.bytes == 0 ||
        opts.bytes > (1ULL << 20)) {
        fprintf(stderr, "invalid smoke parameters\n");
        return 1;
    }

    memset(&logdev, 0, sizeof(logdev));
    if (!find_logdev_manager(opts.logical_dev, &logdev)) {
        fprintf(stderr, "logdev not found: %s\n", opts.logical_dev);
        return 1;
    }

    return run_pattern_io(&logdev, opts.bytes, opts.io_mode);
}
