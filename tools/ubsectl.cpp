#include "ssu_api.h"
#include "ssu_controller.h"
#include "ssu_plugin.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define RESPONSE_SIZE 8192U
#define DEFAULT_MANAGER_SOCKET "/tmp/ubse-ssu-mgr.fifo"

static void usage(void)
{
    puts("usage: ubsectl <alloc|mount|unmount|release|query>");
    puts("       ubsectl alloc --size BYTES --stripe|--replica N [--out FILE]");
    puts("       ubsectl mount --aid AID --host HOST --dev /dev/ssuN");
    puts("       ubsectl unmount --dev /dev/ssuN");
    puts("       ubsectl release --aid AID");
    puts("       ubsectl query --type pool|allocation|logdev");
}

static int is_empty_string(const char *s)
{
    return s == NULL || s[0] == '\0';
}

static const char *manager_socket_path(void)
{
    const char *path = getenv("SSU_MGR_SOCKET");
    struct stat st;

    if (!is_empty_string(path)) {
        return path;
    }

    if (stat(DEFAULT_MANAGER_SOCKET, &st) == 0 && S_ISFIFO(st.st_mode)) {
        return DEFAULT_MANAGER_SOCKET;
    }

    return NULL;
}

static const char *ssu_err_name(ssu_err_t err)
{
    switch (err) {
    case SSU_OK:
        return "SSU_OK";
    case SSU_ERR_INVALID:
        return "SSU_ERR_INVALID";
    case SSU_ERR_NO_RESOURCE:
        return "SSU_ERR_NO_RESOURCE";
    case SSU_ERR_NOT_FOUND:
        return "SSU_ERR_NOT_FOUND";
    case SSU_ERR_BUSY:
        return "SSU_ERR_BUSY";
    case SSU_ERR_IO:
        return "SSU_ERR_IO";
    case SSU_ERR_KERNEL:
        return "SSU_ERR_KERNEL";
    case SSU_ERR_NS_EXISTS:
        return "SSU_ERR_NS_EXISTS";
    case SSU_ERR_BUFFER_TOO_SMALL:
        return "SSU_ERR_BUFFER_TOO_SMALL";
    case SSU_ERR_UNSUPPORTED:
        return "SSU_ERR_UNSUPPORTED";
    case SSU_ERR_INTERNAL:
        return "SSU_ERR_INTERNAL";
    default:
        return "SSU_ERR_UNKNOWN";
    }
}

static void print_operation_error(const char *operation, ssu_err_t err)
{
    fprintf(stderr, "%s failed: %s (%d)\n", operation,
            ssu_err_name(err), err);
}

static int parse_size(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;
    uint64_t multiplier = 1;

    if (is_empty_string(s) || out == NULL) {
        return 0;
    }

    errno = 0;
    value = strtoull(s, &end, 10);
    if (errno != 0 || end == s) {
        return 0;
    }

    if (*end != '\0') {
        if (end[1] != '\0') {
            return 0;
        }

        switch (*end) {
        case 'k':
        case 'K':
            multiplier = 1024ULL;
            break;
        case 'm':
        case 'M':
            multiplier = 1024ULL * 1024ULL;
            break;
        case 'g':
        case 'G':
            multiplier = 1024ULL * 1024ULL * 1024ULL;
            break;
        default:
            return 0;
        }
    }

    if (value > UINT64_MAX / multiplier) {
        return 0;
    }

    *out = (uint64_t)value * multiplier;
    return 1;
}

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
    const char *request_path = manager_socket_path();
    char response_path[128];
    char request[512];
    int request_fd;
    int response_fd;
    size_t used = 0;

    if (request_path == NULL || response == NULL || n == 0) {
        return 0;
    }

    snprintf(response_path, sizeof(response_path),
             "/tmp/ubsectl-response-%ld.fifo", (long)getpid());
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

static int write_first_response_line(const char *body, const char *path)
{
    FILE *fp;
    const char *end;

    if (path == NULL) {
        return 0;
    }

    fp = fopen(path, "w");
    if (fp == NULL) {
        perror(path);
        return 1;
    }

    end = strchr(body, '\n');
    if (end == NULL) {
        fputs(body, fp);
        fputc('\n', fp);
    } else {
        fwrite(body, 1, (size_t)(end - body), fp);
        fputc('\n', fp);
    }

    fclose(fp);
    return 0;
}

static int print_manager_response(const char *response, const char *out_path)
{
    const char *body;

    if (strncmp(response, "OK\n", 3) == 0) {
        body = response + 3;
        if (out_path != NULL && write_first_response_line(body, out_path) != 0) {
            return 1;
        }
        fputs(body, stdout);
        return 0;
    }

    if (strncmp(response, "ERR ", 4) == 0) {
        body = strchr(response, '\n');
        if (body != NULL) {
            fputs(body + 1, stderr);
        } else {
            fputs(response, stderr);
        }
        return 1;
    }

    fputs("manager returned malformed response\n", stderr);
    return 1;
}

static int query_type_from_string(const char *s, ssu_query_type_t *out)
{
    if (s == NULL || out == NULL) {
        return 0;
    }

    if (strcmp(s, "pool") == 0) {
        *out = SSU_QUERY_POOL;
        return 1;
    }

    if (strcmp(s, "allocation") == 0) {
        *out = SSU_QUERY_ALLOCATION;
        return 1;
    }

    if (strcmp(s, "logdev") == 0) {
        *out = SSU_QUERY_LOGDEV;
        return 1;
    }

    return 0;
}

static const char *query_type_name(ssu_query_type_t type)
{
    switch (type) {
    case SSU_QUERY_POOL:
        return "pool";
    case SSU_QUERY_ALLOCATION:
        return "allocation";
    case SSU_QUERY_LOGDEV:
        return "logdev";
    default:
        return "unknown";
    }
}

static int parse_query_args(int argc, char **argv, ssu_query_type_t *type)
{
    int i;

    *type = SSU_QUERY_POOL;
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--type") != 0) {
            return 0;
        }

        if (i + 1 >= argc || !query_type_from_string(argv[i + 1], type)) {
            return 0;
        }

        i++;
    }

    return 1;
}

static int refresh_mock_pool(void)
{
    const ssu_plugin_ops_t *plugin = ssu_plugin_entry();
    ssu_err_t err;

    if (plugin == NULL) {
        fputs("mock plugin unavailable\n", stderr);
        return 1;
    }

    err = ssu_controller_refresh_pool(plugin);
    if (err != SSU_OK) {
        print_operation_error("pool refresh", err);
        return 1;
    }

    return 0;
}

static int print_pool(void)
{
    ssu_query_req_t req = {};
    uint32_t count = 0;
    ssu_resource_info_t *resources;
    uint32_t i;
    ssu_err_t err;

    req.type = SSU_QUERY_POOL;
    err = ssu_resource_query(&req, NULL, sizeof(ssu_resource_info_t),
                             &count);

    if (err != SSU_OK && err != SSU_ERR_BUFFER_TOO_SMALL) {
        print_operation_error("query", err);
        return 1;
    }

    if (count == 0) {
        puts("pool entries: 0");
        return 0;
    }

    resources = (ssu_resource_info_t *)calloc(count, sizeof(*resources));
    if (resources == NULL) {
        fputs("query failed: out of memory\n", stderr);
        return 1;
    }

    err = ssu_resource_query(&req, resources, sizeof(*resources), &count);
    if (err != SSU_OK) {
        free(resources);
        print_operation_error("query", err);
        return 1;
    }

    printf("pool entries: %u\n", count);
    for (i = 0; i < count; i++) {
        printf("%s %s %s %llu/%llu\n",
               resources[i].ssu_id,
               resources[i].host_id,
               resources[i].state,
               (unsigned long long)resources[i].used_bytes,
               (unsigned long long)resources[i].total_bytes);
    }

    free(resources);
    return 0;
}

static int print_allocations(void)
{
    ssu_query_req_t req = {};
    uint32_t count = 0;
    ssu_allocation_info_t *allocations;
    uint32_t i;
    ssu_err_t err;

    req.type = SSU_QUERY_ALLOCATION;
    err = ssu_resource_query(&req, NULL, sizeof(ssu_allocation_info_t),
                             &count);

    if (err != SSU_OK && err != SSU_ERR_BUFFER_TOO_SMALL) {
        print_operation_error("query", err);
        return 1;
    }

    printf("allocation entries: %u\n", count);
    if (count == 0) {
        return 0;
    }

    allocations = (ssu_allocation_info_t *)calloc(count,
                                                  sizeof(*allocations));
    if (allocations == NULL) {
        fputs("query failed: out of memory\n", stderr);
        return 1;
    }

    err = ssu_resource_query(&req, allocations, sizeof(*allocations), &count);
    if (err != SSU_OK) {
        free(allocations);
        print_operation_error("query", err);
        return 1;
    }

    for (i = 0; i < count; i++) {
        printf("%s %s %s %s %llu\n",
               allocations[i].allocate_id,
               allocations[i].state,
               allocations[i].ssu_id,
               allocations[i].ns_id,
               (unsigned long long)allocations[i].length);
    }

    free(allocations);
    return 0;
}

static int print_logdevs(void)
{
    ssu_query_req_t req = {};
    uint32_t count = 0;
    ssu_logdev_info_t *logdevs;
    uint32_t i;
    ssu_err_t err;

    req.type = SSU_QUERY_LOGDEV;
    err = ssu_resource_query(&req, NULL, sizeof(ssu_logdev_info_t), &count);

    if (err != SSU_OK && err != SSU_ERR_BUFFER_TOO_SMALL) {
        print_operation_error("query", err);
        return 1;
    }

    printf("logdev entries: %u\n", count);
    if (count == 0) {
        return 0;
    }

    logdevs = (ssu_logdev_info_t *)calloc(count, sizeof(*logdevs));
    if (logdevs == NULL) {
        fputs("query failed: out of memory\n", stderr);
        return 1;
    }

    err = ssu_resource_query(&req, logdevs, sizeof(*logdevs), &count);
    if (err != SSU_OK) {
        free(logdevs);
        print_operation_error("query", err);
        return 1;
    }

    for (i = 0; i < count; i++) {
        printf("%s %s %s %llu %llu %s %s %llu\n",
               logdevs[i].logical_dev,
               logdevs[i].host_id,
               logdevs[i].allocate_id,
               (unsigned long long)logdevs[i].logical_offset,
               (unsigned long long)logdevs[i].length,
               logdevs[i].phys_dev,
               logdevs[i].ns_id,
               (unsigned long long)logdevs[i].phys_sector);
    }

    free(logdevs);
    return 0;
}

static int cmd_query(int argc, char **argv)
{
    ssu_query_type_t type;
    char command[64];
    char response[RESPONSE_SIZE];

    if (!parse_query_args(argc, argv, &type)) {
        usage();
        return 1;
    }

    if (manager_socket_path() != NULL) {
        snprintf(command, sizeof(command), "QUERY %s", query_type_name(type));
        if (!manager_request(command, response, sizeof(response))) {
            return 1;
        }
        return print_manager_response(response, NULL);
    }

    if (type == SSU_QUERY_POOL && refresh_mock_pool() != 0) {
        return 1;
    }

    if (type == SSU_QUERY_POOL) {
        return print_pool();
    }

    if (type == SSU_QUERY_ALLOCATION) {
        return print_allocations();
    }

    if (type == SSU_QUERY_LOGDEV) {
        return print_logdevs();
    }

    return 1;
}

static int write_alloc_output(const char *allocate_id, const char *out_path)
{
    FILE *fp;

    if (out_path == NULL) {
        printf("%s\n", allocate_id);
        return 0;
    }

    fp = fopen(out_path, "w");
    if (fp == NULL) {
        perror(out_path);
        return 1;
    }

    fprintf(fp, "%s\n", allocate_id);
    fclose(fp);
    return 0;
}

static int cmd_alloc(int argc, char **argv)
{
    ssu_alloc_req_t req = {};
    const char *out_path = NULL;
    ssu_alloc_result_t out;
    ssu_alloc_extent_t extents[1];
    uint32_t extent_count = 1;
    char command[128];
    char response[RESPONSE_SIZE];
    int i;
    ssu_err_t err;

    req.reliability = SSU_RELIABILITY_STRIPE;
    req.share_type = SSU_SHARE_EXCLUSIVE;
    req.map_dir = SSU_MAP_DIR_FORWARD;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0) {
            if (i + 1 >= argc || !parse_size(argv[i + 1], &req.size_bytes)) {
                usage();
                return 1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--stripe") == 0) {
            req.reliability = SSU_RELIABILITY_STRIPE;
            continue;
        }

        if (strcmp(argv[i], "--replica") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            req.reliability = SSU_RELIABILITY_REPLICA;
            req.replica_count = (uint32_t)strtoul(argv[i + 1], NULL, 10);
            i++;
            continue;
        }

        if (strcmp(argv[i], "--share") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            if (strcmp(argv[i + 1], "exclusive") == 0) {
                req.share_type = SSU_SHARE_EXCLUSIVE;
            } else if (strcmp(argv[i + 1], "shared") == 0) {
                req.share_type = SSU_SHARE_SHARED;
            } else {
                usage();
                return 1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--out") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            out_path = argv[++i];
            continue;
        }

        usage();
        return 1;
    }

    if (manager_socket_path() != NULL) {
        snprintf(command, sizeof(command), "ALLOC %llu %d %u %d",
                 (unsigned long long)req.size_bytes,
                 (int)req.reliability,
                 req.replica_count,
                 (int)req.share_type);
        if (!manager_request(command, response, sizeof(response))) {
            return 1;
        }
        return print_manager_response(response, out_path);
    }

    memset(&out, 0, sizeof(out));
    memset(extents, 0, sizeof(extents));
    err = ssu_resource_alloc(&req, &out, extents, &extent_count);
    if (err != SSU_OK) {
        print_operation_error("alloc", err);
        return 1;
    }

    return write_alloc_output(out.allocate_id, out_path);
}

static int cmd_mount(int argc, char **argv)
{
    ssu_mount_req_t req = {};
    char command[256];
    char response[RESPONSE_SIZE];
    int i;
    ssu_err_t err;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--aid") == 0 && i + 1 < argc) {
            req.allocate_id = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            req.host_id = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            snprintf(req.logical_dev, sizeof(req.logical_dev), "%s",
                     argv[++i]);
            continue;
        }
        usage();
        return 1;
    }

    if (is_empty_string(req.allocate_id) || is_empty_string(req.host_id) ||
        is_empty_string(req.logical_dev)) {
        usage();
        return 1;
    }

    if (manager_socket_path() != NULL) {
        snprintf(command, sizeof(command), "MOUNT %s %s %s",
                 req.allocate_id, req.host_id, req.logical_dev);
        if (!manager_request(command, response, sizeof(response))) {
            return 1;
        }
        return print_manager_response(response, NULL);
    }

    err = ssu_resource_mount(&req);
    if (err != SSU_OK) {
        print_operation_error("mount", err);
        return 1;
    }

    printf("mounted %s\n", req.logical_dev);
    return 0;
}

static int cmd_unmount(int argc, char **argv)
{
    const char *logical_dev = NULL;
    char command[128];
    char response[RESPONSE_SIZE];
    int i;
    ssu_err_t err;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            logical_dev = argv[++i];
            continue;
        }
        if (logical_dev == NULL) {
            logical_dev = argv[i];
            continue;
        }
        usage();
        return 1;
    }

    if (is_empty_string(logical_dev)) {
        usage();
        return 1;
    }

    if (manager_socket_path() != NULL) {
        snprintf(command, sizeof(command), "UNMOUNT %s", logical_dev);
        if (!manager_request(command, response, sizeof(response))) {
            return 1;
        }
        return print_manager_response(response, NULL);
    }

    err = ssu_resource_unmount(logical_dev);
    if (err != SSU_OK) {
        print_operation_error("unmount", err);
        return 1;
    }

    printf("unmounted %s\n", logical_dev);
    return 0;
}

static int cmd_release(int argc, char **argv)
{
    const char *allocate_id = NULL;
    char command[128];
    char response[RESPONSE_SIZE];
    int i;
    ssu_err_t err;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--aid") == 0 && i + 1 < argc) {
            allocate_id = argv[++i];
            continue;
        }
        if (allocate_id == NULL) {
            allocate_id = argv[i];
            continue;
        }
        usage();
        return 1;
    }

    if (is_empty_string(allocate_id)) {
        usage();
        return 1;
    }

    if (manager_socket_path() != NULL) {
        snprintf(command, sizeof(command), "RELEASE %s", allocate_id);
        if (!manager_request(command, response, sizeof(response))) {
            return 1;
        }
        return print_manager_response(response, NULL);
    }

    err = ssu_resource_release(allocate_id);
    if (err != SSU_OK) {
        print_operation_error("release", err);
        return 1;
    }

    printf("released %s\n", allocate_id);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "alloc") == 0) {
        return cmd_alloc(argc, argv);
    }

    if (strcmp(argv[1], "query") == 0) {
        return cmd_query(argc, argv);
    }

    if (strcmp(argv[1], "mount") == 0) {
        return cmd_mount(argc, argv);
    }

    if (strcmp(argv[1], "unmount") == 0) {
        return cmd_unmount(argc, argv);
    }

    if (strcmp(argv[1], "release") == 0) {
        return cmd_release(argc, argv);
    }

    usage();
    return 1;
}
