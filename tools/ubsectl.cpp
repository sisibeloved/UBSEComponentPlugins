#include "ubse_ssu_sdk.h"

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
#define MAX_CLI_HOSTS 32U

static void usage(void)
{
    puts("usage: ubsectl <allocate|free|list|allocate-result-get|mount|unmount>");
    puts("       ubsectl allocate --size BYTES [--name NAME] [--user USER] [--physical-disks N] [--aggregate|--no-aggregate] [--share exclusive|shared] [--host HOST] [--out FILE]");
    puts("       ubsectl allocate-result-get --request-id ID [--out FILE]");
    puts("       ubsectl free --dev /dev/ssu/ssuN|/dev/ssu/NAME");
    puts("       ubsectl list");
    puts("");
    puts("compat:");
    puts("       ubsectl <alloc|mount|unmount|release|query>");
    puts("       ubsectl alloc --size BYTES --stripe|--replica N [--out FILE]");
    puts("       ubsectl mount --aid AID --host HOST --dev /dev/ssu/ssuN|/dev/ssu/NAME");
    puts("       ubsectl mount --dev /dev/ssu/ssuN|/dev/ssu/NAME --host HOST");
    puts("       ubsectl unmount --dev /dev/ssu/ssuN|/dev/ssu/NAME");
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

    if (!is_empty_string(path)) {
        return path;
    }

    return DEFAULT_MANAGER_SOCKET;
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

static int parse_share_type(const char *s, ssu_share_type_t *out)
{
    if (s == NULL || out == NULL) {
        return 0;
    }

    if (strcmp(s, "exclusive") == 0) {
        *out = SSU_SHARE_EXCLUSIVE;
        return 1;
    }

    if (strcmp(s, "shared") == 0) {
        *out = SSU_SHARE_SHARED;
        return 1;
    }

    return 0;
}

static int is_manager_token_safe(const char *s)
{
    const char *p;

    if (is_empty_string(s)) {
        return 0;
    }

    for (p = s; *p != '\0'; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            return 0;
        }
    }

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

static void print_pool_row(uint32_t index, const char *ssu_id,
                           const char *host_id, const char *state,
                           unsigned long long used,
                           unsigned long long total)
{
    unsigned long long free_bytes = 0;

    if (total >= used) {
        free_bytes = total - used;
    }

    printf("pool[%u]: ssu_id=%s host_id=%s state=%s used_bytes=%llu total_bytes=%llu free_bytes=%llu\n",
           index, ssu_id, host_id, state, used, total, free_bytes);
}

static void print_query_body(ssu_query_type_t type, const char *body)
{
    char copy[RESPONSE_SIZE];
    char *save = NULL;
    char *line;
    uint32_t index = 0;

    snprintf(copy, sizeof(copy), "%s", body == NULL ? "" : body);

    line = strtok_r(copy, "\n", &save);
    if (line == NULL) {
        return;
    }

    printf("%s\n", line);
    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        if (line[0] == '\0') {
            continue;
        }

        if (type == SSU_QUERY_POOL) {
            char ssu_id[64];
            char host_id[64];
            char state[16];
            unsigned long long used = 0;
            unsigned long long total = 0;

            if (sscanf(line, "%63s %63s %15s %llu/%llu",
                       ssu_id, host_id, state, &used, &total) == 5) {
                print_pool_row(index++, ssu_id, host_id, state, used, total);
                continue;
            }
        } else if (type == SSU_QUERY_ALLOCATION) {
            char allocate_id[64];
            char state[16];
            char ssu_id[64];
            char ns_id[32];
            unsigned long long length = 0;

            if (sscanf(line, "%63s %15s %63s %31s %llu",
                       allocate_id, state, ssu_id, ns_id, &length) == 5) {
                printf("allocation[%u]: allocate_id=%s state=%s ssu_id=%s ns_id=%s length_bytes=%llu\n",
                       index++, allocate_id, state, ssu_id, ns_id, length);
                continue;
            }
        } else if (type == SSU_QUERY_LOGDEV) {
            char logical_dev[64];
            char host_id[64];
            char allocate_id[64];
            char phys_dev[64];
            char ns_id[32];
            unsigned long long logical_offset = 0;
            unsigned long long length = 0;
            unsigned long long phys_sector = 0;
            unsigned long long phys_offset = 0;

            if (sscanf(line, "%63s %63s %63s %llu %llu %63s %31s %llu",
                       logical_dev, host_id, allocate_id, &logical_offset,
                       &length, phys_dev, ns_id, &phys_sector) == 8) {
                phys_offset = phys_sector << 9;
                printf("logdev[%u]: logical_dev=%s host_id=%s allocate_id=%s logical_offset_bytes=%llu length_bytes=%llu physical_dev=%s ns_id=%s physical_lba_512b=%llu physical_offset_bytes=%llu\n",
                       index++, logical_dev, host_id, allocate_id,
                       logical_offset, length, phys_dev, ns_id, phys_sector,
                       phys_offset);
                continue;
            }
        }

        printf("%s\n", line);
        index++;
    }
}

static int print_query_response(ssu_query_type_t type, const char *response)
{
    const char *body;

    if (strncmp(response, "OK\n", 3) == 0) {
        body = response + 3;
        print_query_body(type, body);
        return 0;
    }

    return print_manager_response(response, NULL);
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

static int cmd_query(int argc, char **argv)
{
    ssu_query_type_t type;
    char command[64];
    char response[RESPONSE_SIZE];

    if (!parse_query_args(argc, argv, &type)) {
        usage();
        return 1;
    }

    snprintf(command, sizeof(command), "QUERY %s", query_type_name(type));
    if (!manager_request(command, response, sizeof(response))) {
        return 1;
    }
    return print_query_response(type, response);
}

static int cmd_list(int argc, char **argv)
{
    uint32_t count = 0;
    uint32_t capacity;
    ssu_resource_info_t *resources;
    ssu_err_t err;
    uint32_t i;

    (void)argv;

    if (argc != 2) {
        usage();
        return 1;
    }

    err = ubse_ssu_sdk_list(NULL, &count);
    if (err != SSU_OK && err != SSU_ERR_BUFFER_TOO_SMALL) {
        print_operation_error("list", err);
        return 1;
    }

    if (count == 0) {
        puts("pool entries: 0");
        return 0;
    }

    resources = (ssu_resource_info_t *)calloc(count, sizeof(*resources));
    if (resources == NULL) {
        fputs("list failed: out of memory\n", stderr);
        return 1;
    }

    capacity = count;
    err = ubse_ssu_sdk_list(resources, &capacity);
    if (err != SSU_OK) {
        free(resources);
        print_operation_error("list", err);
        return 1;
    }

    printf("pool entries: %u\n", capacity);
    for (i = 0; i < capacity; i++) {
        print_pool_row(i,
                       resources[i].ssu_id,
                       resources[i].host_id,
                       resources[i].state,
                       (unsigned long long)resources[i].used_bytes,
                       (unsigned long long)resources[i].total_bytes);
    }

    free(resources);
    return 0;
}

static int write_alloc_output(const char *allocate_id, const char *out_path)
{
    FILE *fp;

    printf("%s\n", allocate_id);
    if (out_path == NULL) {
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

static int print_allocate_result(const ssu_api_allocate_result_info_t *result,
                                 const char *out_path)
{
    uint32_t i;

    if (write_alloc_output(result->device_path, out_path) != 0) {
        return 1;
    }

    printf("physical_disks=%u\n", result->physical_disk_count);

    for (i = 0; i < result->physical_disk_count; i++) {
        unsigned long long physical_lba =
            (unsigned long long)result->physical_disks[i].lba;
        unsigned long long physical_offset =
            physical_lba << 9;

        printf("physical[%u]: ssu_id=%s ns_id=%s logical_offset_bytes=%llu length_bytes=%llu physical_lba_512b=%llu physical_offset_bytes=%llu\n",
               i,
               result->physical_disks[i].ssu_id,
               result->physical_disks[i].ns_id,
               (unsigned long long)result->physical_disks[i].logical_offset,
               (unsigned long long)result->physical_disks[i].length,
               physical_lba,
               physical_offset);
    }

    return 0;
}

static int cmd_allocate(int argc, char **argv)
{
    ssu_api_allocate_req_t req = {};
    ssu_api_allocate_resp_t out;
    const char *hosts[MAX_CLI_HOSTS];
    uint32_t host_count = 0;
    const char *out_path = NULL;
    const char *user = "default";
    const char *disk_name = NULL;
    int i;
    ssu_err_t err;

    req.physical_disk_count = 0;
    req.logical_disk_aggregate = 1;
    req.allocation_type = SSU_SHARE_EXCLUSIVE;

    memset(hosts, 0, sizeof(hosts));
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0) {
            if (i + 1 >= argc || !parse_size(argv[i + 1], &req.size_bytes)) {
                usage();
                return 1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--user") == 0 ||
            strcmp(argv[i], "--tenant") == 0) {
            if (i + 1 >= argc || is_empty_string(argv[i + 1])) {
                usage();
                return 1;
            }
            user = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--name") == 0 ||
            strcmp(argv[i], "--disk-name") == 0) {
            if (i + 1 >= argc || !is_manager_token_safe(argv[i + 1])) {
                usage();
                return 1;
            }
            disk_name = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--physical-disks") == 0 ||
            strcmp(argv[i], "--phys-disks") == 0 ||
            strcmp(argv[i], "--shards") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            req.physical_disk_count =
                (uint32_t)strtoul(argv[i + 1], NULL, 10);
            i++;
            continue;
        }

        if (strcmp(argv[i], "--aggregate") == 0) {
            req.logical_disk_aggregate = 1;
            continue;
        }

        if (strcmp(argv[i], "--no-aggregate") == 0) {
            req.logical_disk_aggregate = -1;
            continue;
        }

        if (strcmp(argv[i], "--share") == 0) {
            if (i + 1 >= argc ||
                !parse_share_type(argv[i + 1], &req.allocation_type)) {
                usage();
                return 1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--host") == 0) {
            if (i + 1 >= argc || host_count >= MAX_CLI_HOSTS ||
                is_empty_string(argv[i + 1])) {
                usage();
                return 1;
            }
            hosts[host_count++] = argv[++i];
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

    if (host_count == 0) {
        hosts[host_count++] = "local";
    }

    req.user_id = user;
    req.disk_name = disk_name;
    req.host_ids = hosts;
    req.host_count = host_count;

    memset(&out, 0, sizeof(out));
    err = ubse_ssu_sdk_allocate(&req, &out);
    if (err != SSU_OK) {
        print_operation_error("allocate", err);
        return 1;
    }

    return write_alloc_output(out.request_id, out_path);
}

static int cmd_allocate_result_get(int argc, char **argv)
{
    const char *request_id = NULL;
    const char *out_path = NULL;
    ssu_api_allocate_result_info_t result;
    int i;
    ssu_err_t err;

    for (i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "--request-id") == 0 ||
             strcmp(argv[i], "--rid") == 0) && i + 1 < argc) {
            request_id = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
            continue;
        }

        if (request_id == NULL) {
            request_id = argv[i];
            continue;
        }

        usage();
        return 1;
    }

    if (is_empty_string(request_id)) {
        usage();
        return 1;
    }

    memset(&result, 0, sizeof(result));
    err = ubse_ssu_sdk_allocate_result_get(request_id, &result);
    if (err != SSU_OK) {
        print_operation_error("allocate-result-get", err);
        if (!is_empty_string(result.error_message)) {
            fprintf(stderr, "detail: %s\n", result.error_message);
        }
        return 1;
    }

    return print_allocate_result(&result, out_path);
}

static int cmd_alloc(int argc, char **argv)
{
    uint64_t size_bytes = 0;
    ssu_reliability_t reliability = SSU_RELIABILITY_STRIPE;
    uint32_t replica_count = 0;
    ssu_share_type_t share_type = SSU_SHARE_EXCLUSIVE;
    const char *out_path = NULL;
    char command[128];
    char response[RESPONSE_SIZE];
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0) {
            if (i + 1 >= argc || !parse_size(argv[i + 1], &size_bytes)) {
                usage();
                return 1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--stripe") == 0) {
            reliability = SSU_RELIABILITY_STRIPE;
            continue;
        }

        if (strcmp(argv[i], "--replica") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            reliability = SSU_RELIABILITY_REPLICA;
            replica_count = (uint32_t)strtoul(argv[i + 1], NULL, 10);
            i++;
            continue;
        }

        if (strcmp(argv[i], "--share") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            if (!parse_share_type(argv[i + 1], &share_type)) {
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

    snprintf(command, sizeof(command), "ALLOC %llu %d %u %d",
             (unsigned long long)size_bytes,
             (int)reliability,
             replica_count,
             (int)share_type);
    if (!manager_request(command, response, sizeof(response))) {
        return 1;
    }
    return print_manager_response(response, out_path);
}

static int cmd_mount(int argc, char **argv)
{
    const char *allocate_id = NULL;
    const char *host_id = NULL;
    char logical_dev[64] = {};
    char command[256];
    char response[RESPONSE_SIZE];
    int i;
    ssu_err_t err;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--aid") == 0 && i + 1 < argc) {
            allocate_id = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host_id = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            snprintf(logical_dev, sizeof(logical_dev), "%s", argv[++i]);
            continue;
        }
        usage();
        return 1;
    }

    if (is_empty_string(host_id) || is_empty_string(logical_dev)) {
        usage();
        return 1;
    }

    if (!is_empty_string(allocate_id)) {
        if (!is_manager_token_safe(allocate_id) ||
            !is_manager_token_safe(host_id) ||
            !is_manager_token_safe(logical_dev)) {
            usage();
            return 1;
        }
        snprintf(command, sizeof(command), "MOUNT %s %s %s",
                 allocate_id, host_id, logical_dev);
        if (!manager_request(command, response, sizeof(response))) {
            return 1;
        }
        return print_manager_response(response, NULL);
    }

    err = ubse_ssu_sdk_mount(logical_dev, host_id);
    if (err != SSU_OK) {
        print_operation_error("mount", err);
        return 1;
    }

    printf("mounted %s\n", logical_dev);
    return 0;
}

static int cmd_unmount(int argc, char **argv)
{
    const char *logical_dev = NULL;
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

    err = ubse_ssu_sdk_unmount(logical_dev);
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

    if (!is_manager_token_safe(allocate_id)) {
        usage();
        return 1;
    }

    snprintf(command, sizeof(command), "RELEASE %s", allocate_id);
    if (!manager_request(command, response, sizeof(response))) {
        return 1;
    }
    return print_manager_response(response, NULL);
}

static int cmd_free(int argc, char **argv)
{
    const char *logical_dev = NULL;
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

    err = ubse_ssu_sdk_free(logical_dev);
    if (err != SSU_OK) {
        print_operation_error("free", err);
        return 1;
    }

    printf("freed %s\n", logical_dev);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "allocate") == 0) {
        return cmd_allocate(argc, argv);
    }

    if (strcmp(argv[1], "allocate-result-get") == 0) {
        return cmd_allocate_result_get(argc, argv);
    }

    if (strcmp(argv[1], "list") == 0) {
        return cmd_list(argc, argv);
    }

    if (strcmp(argv[1], "free") == 0) {
        return cmd_free(argc, argv);
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
