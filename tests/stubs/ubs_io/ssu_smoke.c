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

static int find_logdev(const char *logical_dev, ssu_logdev_info_t *out)
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

static void fill_pattern(unsigned char *buf, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        buf[i] = (unsigned char)((i * 31U + 7U) & 0xffU);
    }
}

int main(int argc, char **argv)
{
    const char *logical_dev = NULL;
    uint64_t bytes = 65536;
    ssu_logdev_info_t logdev;
    unsigned char *pattern;
    unsigned char *readback;
    int i;
    int rc = 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            logical_dev = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--bytes") == 0 && i + 1 < argc) {
            bytes = (uint64_t)strtoull(argv[++i], NULL, 10);
            continue;
        }

        if (logical_dev == NULL) {
            logical_dev = argv[i];
            continue;
        }

        fprintf(stderr,
                "usage: ssu_smoke /dev/ssuN [--bytes N]\n");
        return 1;
    }

    if (logical_dev == NULL || bytes == 0 || bytes > (1ULL << 20)) {
        fprintf(stderr, "invalid smoke parameters\n");
        return 1;
    }

    memset(&logdev, 0, sizeof(logdev));
    if (!find_logdev(logical_dev, &logdev)) {
        fprintf(stderr, "logdev not found: %s\n", logical_dev);
        return 1;
    }

    if (bytes > logdev.length) {
        fprintf(stderr, "smoke length exceeds logdev length\n");
        return 1;
    }

    pattern = malloc((size_t)bytes);
    readback = malloc((size_t)bytes);
    if (pattern == NULL || readback == NULL) {
        fputs("out of memory\n", stderr);
        goto out;
    }

    fill_pattern(pattern, (size_t)bytes);
    memset(readback, 0, (size_t)bytes);

    if (ssu_dataplane_write(&logdev, 0, pattern, (size_t)bytes) != SSU_OK) {
        fputs("pattern write failed\n", stderr);
        goto out;
    }

    if (ssu_dataplane_read(&logdev, 0, readback, (size_t)bytes) != SSU_OK) {
        fputs("pattern read failed\n", stderr);
        goto out;
    }

    if (memcmp(pattern, readback, (size_t)bytes) != 0) {
        fputs("pattern mismatch\n", stderr);
        goto out;
    }

    printf("ssu_smoke ok %s %llu bytes via %s\n",
           logical_dev,
           (unsigned long long)bytes,
           logdev.phys_dev);
    rc = 0;

out:
    free(pattern);
    free(readback);
    return rc;
}
