#include "ssu_controller.h"
#include "ssu_plugin.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SSU_DEFAULT_ROLE
#define SSU_DEFAULT_ROLE "unknown"
#endif

#define RUNTIME_BODY_SIZE 8192U
#define DEFAULT_MANAGER_SOCKET "/tmp/ubse-ssu-mgr.fifo"

typedef struct {
    const char *role;
    const char *config;
    const char *socket_path;
    int once;
} runtime_options_t;

static volatile sig_atomic_t stop_requested;

static const ssu_plugin_ops_t *runtime_plugin(void);

static const char *configured_socket_path(void)
{
    const char *path = getenv("SSU_MGR_SOCKET");

    if (path != NULL && path[0] != '\0') {
        return path;
    }

    return DEFAULT_MANAGER_SOCKET;
}

static void handle_signal(int signum)
{
    (void)signum;
    stop_requested = 1;
}

static const char *program_label(const char *role)
{
    if (strcmp(role, "manager") == 0) {
        return "ssu-mgr";
    }

    if (strcmp(role, "agent") == 0) {
        return "ssu-agent";
    }

    return "ssu-runtime";
}

static int parse_args(int argc, char **argv, runtime_options_t *opts)
{
    int i;

    opts->role = SSU_DEFAULT_ROLE;
    opts->config = NULL;
    opts->socket_path = NULL;
    opts->once = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) {
            opts->once = 1;
            continue;
        }

        if (strcmp(argv[i], "--role") == 0) {
            if (i + 1 >= argc) {
                return 0;
            }
            opts->role = argv[++i];
            continue;
        }

        if (strncmp(argv[i], "--role=", 7) == 0) {
            opts->role = argv[i] + 7;
            continue;
        }

        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                return 0;
            }
            opts->config = argv[++i];
            continue;
        }

        if (strncmp(argv[i], "--config=", 9) == 0) {
            opts->config = argv[i] + 9;
            continue;
        }

        if (strcmp(argv[i], "--socket") == 0) {
            if (i + 1 >= argc) {
                return 0;
            }
            opts->socket_path = argv[++i];
            continue;
        }

        if (strncmp(argv[i], "--socket=", 9) == 0) {
            opts->socket_path = argv[i] + 9;
            continue;
        }

        return 0;
    }

    return opts->role != NULL && opts->role[0] != '\0';
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

static const char *operation_error_hint(const char *plugin_name,
                                        const char *operation,
                                        ssu_err_t err)
{
    if (plugin_name != NULL && strcmp(plugin_name, "lbc_mock") == 0) {
        if (strcmp(operation, "alloc") == 0 &&
            err == SSU_ERR_NOT_FOUND) {
            return "lbc_mock create/attach finished, but no new /dev/nvmeXnY namespace was found. Check LBC_PREFIX, mock/run_mock.sh, ls /dev/nvme*n*, and /tmp/ubse-lbc-mock.log.";
        }

        if (strcmp(operation, "alloc") == 0 &&
            err == SSU_ERR_IO) {
            return "lbc_mock create/attach command failed. Check ssu-mgr stderr and /tmp/ubse-lbc-mock.log for the exact script and exit status.";
        }

        if (err == SSU_ERR_NOT_FOUND) {
            return "lbc_mock did not find the requested SSU, allocation, namespace, or logical device. Check query output and /tmp/ubse-lbc-mock.log.";
        }
    }

    if (err == SSU_ERR_NO_RESOURCE) {
        return "No usable SSU resource is currently available. Run query --type pool and check whether discovery succeeded.";
    }

    if (err == SSU_ERR_BUSY) {
        return "The resource is still in use. Unmount it before releasing or reuse another logical device path.";
    }

    return NULL;
}

static void format_operation_error(char *body, size_t n,
                                   const char *operation, ssu_err_t err)
{
    const ssu_plugin_ops_t *plugin = runtime_plugin();
    const char *plugin_name = plugin != NULL && plugin->name != NULL ?
                                  plugin->name() : "unknown";
    const char *hint = operation_error_hint(plugin_name, operation, err);

    if (hint == NULL) {
        snprintf(body, n, "%s failed: %s (%d)\nplugin: %s\n",
                 operation, ssu_err_name(err), err, plugin_name);
        return;
    }

    snprintf(body, n, "%s failed: %s (%d)\nplugin: %s\nhint: %s\n",
             operation, ssu_err_name(err), err, plugin_name, hint);
}

static const ssu_plugin_ops_t *runtime_plugin(void)
{
    return ssu_plugin_entry();
}

static int refresh_pool(void)
{
    const ssu_plugin_ops_t *plugin = runtime_plugin();
    ssu_err_t err;

    if (plugin == NULL) {
        fputs("mock plugin unavailable\n", stderr);
        return 1;
    }

    err = ssu_controller_refresh_pool(plugin);
    if (err != SSU_OK) {
        fprintf(stderr, "pool refresh failed: %s (%d)\n",
                ssu_err_name(err), err);
        return 1;
    }

    return 0;
}

static void appendf(char *buf, size_t n, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int rc;

    if (*used >= n) {
        return;
    }

    va_start(ap, fmt);
    rc = vsnprintf(buf + *used, n - *used, fmt, ap);
    va_end(ap);

    if (rc < 0) {
        return;
    }

    if ((size_t)rc >= n - *used) {
        *used = n - 1;
        return;
    }

    *used += (size_t)rc;
}

static ssu_err_t build_pool_body(char *body, size_t n)
{
    ssu_resource_info_t resources[128];
    uint32_t count = 128;
    uint32_t i;
    size_t used = 0;
    ssu_err_t err = ssu_controller_query_pool(resources, &count);

    if (err != SSU_OK) {
        return err;
    }

    appendf(body, n, &used, "pool entries: %u\n", count);
    for (i = 0; i < count; i++) {
        appendf(body, n, &used, "%s %s %s %llu/%llu\n",
                resources[i].ssu_id,
                resources[i].host_id,
                resources[i].state,
                (unsigned long long)resources[i].used_bytes,
                (unsigned long long)resources[i].total_bytes);
    }

    return SSU_OK;
}

static ssu_err_t build_allocation_body(char *body, size_t n)
{
    ssu_allocation_info_t allocations[128];
    uint32_t count = 128;
    uint32_t i;
    size_t used = 0;
    ssu_err_t err = ssu_controller_query_allocations(allocations, &count);

    if (err != SSU_OK) {
        return err;
    }

    appendf(body, n, &used, "allocation entries: %u\n", count);
    for (i = 0; i < count; i++) {
        appendf(body, n, &used, "%s %s %s %s %llu\n",
                allocations[i].allocate_id,
                allocations[i].state,
                allocations[i].ssu_id,
                allocations[i].ns_id,
                (unsigned long long)allocations[i].length);
    }

    return SSU_OK;
}

static ssu_err_t build_logdev_body(char *body, size_t n)
{
    ssu_logdev_info_t logdevs[128];
    uint32_t count = 128;
    uint32_t i;
    size_t used = 0;
    ssu_err_t err = ssu_controller_query_logdevs(logdevs, &count);

    if (err != SSU_OK) {
        return err;
    }

    appendf(body, n, &used, "logdev entries: %u\n", count);
    for (i = 0; i < count; i++) {
        appendf(body, n, &used, "%s %s %s %llu %llu %s %s %llu\n",
                logdevs[i].logical_dev,
                logdevs[i].host_id,
                logdevs[i].allocate_id,
                (unsigned long long)logdevs[i].logical_offset,
                (unsigned long long)logdevs[i].length,
                logdevs[i].phys_dev,
                logdevs[i].ns_id,
                (unsigned long long)logdevs[i].phys_sector);
    }

    return SSU_OK;
}

static int print_pool(void)
{
    char body[RUNTIME_BODY_SIZE];
    ssu_err_t err;

    memset(body, 0, sizeof(body));
    err = build_pool_body(body, sizeof(body));
    if (err != SSU_OK) {
        fprintf(stderr, "pool query failed: %s (%d)\n",
                ssu_err_name(err), err);
        return 1;
    }

    fputs(body, stdout);
    return 0;
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

static void send_response(int fd, ssu_err_t err, const char *body)
{
    char header[64];

    if (err == SSU_OK) {
        send_all(fd, "OK\n", 3);
        send_all(fd, body, strlen(body));
        return;
    }

    snprintf(header, sizeof(header), "ERR %d\n", err);
    send_all(fd, header, strlen(header));
    send_all(fd, body, strlen(body));
}

static ssu_err_t handle_alloc(char *save, char *body, size_t n)
{
    char *size_s = strtok_r(NULL, " ", &save);
    char *reliability_s = strtok_r(NULL, " ", &save);
    char *replica_s = strtok_r(NULL, " ", &save);
    char *share_s = strtok_r(NULL, " ", &save);
    ssu_alloc_req_t req;
    ssu_alloc_result_t result;
    ssu_alloc_extent_t extents[1];
    uint32_t extent_count = 1;
    ssu_err_t err;

    if (size_s == NULL || reliability_s == NULL || replica_s == NULL ||
        share_s == NULL) {
        snprintf(body, n, "invalid ALLOC command\n");
        return SSU_ERR_INVALID;
    }

    memset(&req, 0, sizeof(req));
    req.size_bytes = (uint64_t)strtoull(size_s, NULL, 10);
    req.physical_disk_count = 1;
    req.reliability = (ssu_reliability_t)strtol(reliability_s, NULL, 10);
    req.replica_count = (uint32_t)strtoul(replica_s, NULL, 10);
    req.share_type = (ssu_share_type_t)strtol(share_s, NULL, 10);
    req.map_dir = SSU_MAP_DIR_FORWARD;

    memset(&result, 0, sizeof(result));
    memset(extents, 0, sizeof(extents));
    err = ssu_controller_alloc(runtime_plugin(), &req, &result, extents,
                               &extent_count);
    if (err != SSU_OK) {
        format_operation_error(body, n, "alloc", err);
        return err;
    }

    snprintf(body, n, "%s\n", result.allocate_id);
    return SSU_OK;
}

static ssu_err_t handle_allocate(char *save, char *body, size_t n)
{
    char *size_s = strtok_r(NULL, " ", &save);
    char *share_s = strtok_r(NULL, " ", &save);
    char *physical_disks_s = strtok_r(NULL, " ", &save);
    char *aggregate_s = strtok_r(NULL, " ", &save);
    char *user_s = strtok_r(NULL, " ", &save);
    char *hosts_s = strtok_r(NULL, " ", &save);
    const char *hosts[32];
    char *host_save = NULL;
    char *host;
    ssu_api_allocate_req_t req;
    ssu_api_allocate_resp_t result;
    ssu_err_t err;

    if (size_s == NULL || share_s == NULL || physical_disks_s == NULL ||
        aggregate_s == NULL || user_s == NULL || hosts_s == NULL) {
        snprintf(body, n, "invalid ALLOCATE command\n");
        return SSU_ERR_INVALID;
    }

    memset(&req, 0, sizeof(req));
    memset(hosts, 0, sizeof(hosts));
    host = strtok_r(hosts_s, ",", &host_save);
    while (host != NULL) {
        if (req.host_count >= 32U) {
            snprintf(body, n, "too many hosts in ALLOCATE command\n");
            return SSU_ERR_INVALID;
        }
        hosts[req.host_count++] = host;
        host = strtok_r(NULL, ",", &host_save);
    }

    req.size_bytes = (uint64_t)strtoull(size_s, NULL, 10);
    req.allocation_type = (ssu_share_type_t)strtol(share_s, NULL, 10);
    req.physical_disk_count = (uint32_t)strtoul(physical_disks_s, NULL, 10);
    req.logical_disk_aggregate = (int)strtol(aggregate_s, NULL, 10);
    req.user_id = strcmp(user_s, "-") == 0 ? NULL : user_s;
    req.host_ids = hosts;

    memset(&result, 0, sizeof(result));
    err = ssu_api_allocate(&req, &result);
    if (err != SSU_OK) {
        format_operation_error(body, n, "allocate", err);
        return err;
    }

    snprintf(body, n, "%s\n", result.request_id);
    return SSU_OK;
}

static ssu_err_t handle_allocate_result(char *save, char *body, size_t n)
{
    char *request_id = strtok_r(NULL, " ", &save);
    ssu_api_allocate_result_info_t result;
    ssu_err_t err;
    size_t used = 0;
    uint32_t i;

    if (request_id == NULL) {
        snprintf(body, n, "invalid ALLOCATE_RESULT command\n");
        return SSU_ERR_INVALID;
    }

    memset(&result, 0, sizeof(result));
    err = ssu_api_allocate_result_get(request_id, &result);
    if (err != SSU_OK) {
        format_operation_error(body, n, "allocate-result-get", err);
        return err;
    }

    appendf(body, n, &used, "%s\n", result.device_path);
    for (i = 0; i < result.physical_disk_count; i++) {
        appendf(body, n, &used, "physical %u %s %s %llu %llu lba=%llu\n",
                i,
                result.physical_disks[i].ssu_id,
                result.physical_disks[i].ns_id,
                (unsigned long long)result.physical_disks[i].logical_offset,
                (unsigned long long)result.physical_disks[i].length,
                (unsigned long long)result.physical_disks[i].lba);
    }
    return SSU_OK;
}

static ssu_err_t handle_mount(char *save, char *body, size_t n)
{
    char *aid = strtok_r(NULL, " ", &save);
    char *host = strtok_r(NULL, " ", &save);
    char *dev = strtok_r(NULL, " ", &save);
    ssu_mount_req_t req;
    ssu_err_t err;

    if (aid == NULL || host == NULL || dev == NULL) {
        snprintf(body, n, "invalid MOUNT command\n");
        return SSU_ERR_INVALID;
    }

    memset(&req, 0, sizeof(req));
    req.allocate_id = aid;
    req.host_id = host;
    snprintf(req.logical_dev, sizeof(req.logical_dev), "%s", dev);
    err = ssu_controller_mount(runtime_plugin(), &req);
    if (err != SSU_OK) {
        format_operation_error(body, n, "mount", err);
        return err;
    }

    snprintf(body, n, "mounted %s\n", dev);
    return SSU_OK;
}

static ssu_err_t handle_mount_dev(char *save, char *body, size_t n)
{
    char *dev = strtok_r(NULL, " ", &save);
    char *host = strtok_r(NULL, " ", &save);
    ssu_err_t err;

    if (dev == NULL || host == NULL) {
        snprintf(body, n, "invalid MOUNT_DEV command\n");
        return SSU_ERR_INVALID;
    }

    err = ssu_api_mount(dev, host);
    if (err != SSU_OK) {
        format_operation_error(body, n, "mount", err);
        return err;
    }

    snprintf(body, n, "mounted %s\n", dev);
    return SSU_OK;
}

static ssu_err_t handle_unmount(char *save, char *body, size_t n)
{
    char *dev = strtok_r(NULL, " ", &save);
    ssu_err_t err;

    if (dev == NULL) {
        snprintf(body, n, "invalid UNMOUNT command\n");
        return SSU_ERR_INVALID;
    }

    err = ssu_controller_unmount(runtime_plugin(), dev);
    if (err != SSU_OK) {
        format_operation_error(body, n, "unmount", err);
        return err;
    }

    snprintf(body, n, "unmounted %s\n", dev);
    return SSU_OK;
}

static ssu_err_t handle_release(char *save, char *body, size_t n)
{
    char *aid = strtok_r(NULL, " ", &save);
    ssu_err_t err;

    if (aid == NULL) {
        snprintf(body, n, "invalid RELEASE command\n");
        return SSU_ERR_INVALID;
    }

    err = ssu_controller_release(runtime_plugin(), aid);
    if (err != SSU_OK) {
        format_operation_error(body, n, "release", err);
        return err;
    }

    snprintf(body, n, "released %s\n", aid);
    return SSU_OK;
}

static ssu_err_t handle_list(char *body, size_t n)
{
    if (refresh_pool() != 0) {
        snprintf(body, n, "pool refresh failed\n");
        return SSU_ERR_INTERNAL;
    }

    return build_pool_body(body, n);
}

static ssu_err_t handle_free(char *save, char *body, size_t n)
{
    char *dev = strtok_r(NULL, " ", &save);
    ssu_err_t err;

    if (dev == NULL) {
        snprintf(body, n, "invalid FREE command\n");
        return SSU_ERR_INVALID;
    }

    err = ssu_api_free(dev);
    if (err != SSU_OK) {
        format_operation_error(body, n, "free", err);
        return err;
    }

    snprintf(body, n, "freed %s\n", dev);
    return SSU_OK;
}

static ssu_err_t handle_query(char *save, char *body, size_t n)
{
    char *type = strtok_r(NULL, " ", &save);

    if (type == NULL) {
        snprintf(body, n, "invalid QUERY command\n");
        return SSU_ERR_INVALID;
    }

    if (strcmp(type, "pool") == 0) {
        if (refresh_pool() != 0) {
            snprintf(body, n, "pool refresh failed\n");
            return SSU_ERR_INTERNAL;
        }
        return build_pool_body(body, n);
    }

    if (strcmp(type, "allocation") == 0) {
        return build_allocation_body(body, n);
    }

    if (strcmp(type, "logdev") == 0) {
        return build_logdev_body(body, n);
    }

    snprintf(body, n, "unknown query type: %s\n", type);
    return SSU_ERR_INVALID;
}

static void handle_command(int fd, char *request)
{
    char body[RUNTIME_BODY_SIZE];
    char *save = NULL;
    char *cmd;
    ssu_err_t err;

    request[strcspn(request, "\r\n")] = '\0';

    memset(body, 0, sizeof(body));
    cmd = strtok_r(request, " ", &save);
    if (cmd == NULL) {
        send_response(fd, SSU_ERR_INVALID, "empty command\n");
        return;
    }

    if (strcmp(cmd, "ALLOC") == 0) {
        err = handle_alloc(save, body, sizeof(body));
    } else if (strcmp(cmd, "ALLOCATE") == 0) {
        err = handle_allocate(save, body, sizeof(body));
    } else if (strcmp(cmd, "ALLOCATE_RESULT") == 0) {
        err = handle_allocate_result(save, body, sizeof(body));
    } else if (strcmp(cmd, "MOUNT") == 0) {
        err = handle_mount(save, body, sizeof(body));
    } else if (strcmp(cmd, "MOUNT_DEV") == 0) {
        err = handle_mount_dev(save, body, sizeof(body));
    } else if (strcmp(cmd, "UNMOUNT") == 0) {
        err = handle_unmount(save, body, sizeof(body));
    } else if (strcmp(cmd, "RELEASE") == 0) {
        err = handle_release(save, body, sizeof(body));
    } else if (strcmp(cmd, "FREE") == 0) {
        err = handle_free(save, body, sizeof(body));
    } else if (strcmp(cmd, "LIST") == 0) {
        err = handle_list(body, sizeof(body));
    } else if (strcmp(cmd, "QUERY") == 0) {
        err = handle_query(save, body, sizeof(body));
    } else {
        snprintf(body, sizeof(body), "unknown command: %s\n", cmd);
        err = SSU_ERR_INVALID;
    }

    send_response(fd, err, body);
}

static void handle_fifo_line(char *line)
{
    char *save = NULL;
    char *response_path;
    char *command;
    int response_fd;

    response_path = strtok_r(line, " ", &save);
    command = save;
    if (response_path == NULL || command == NULL || command[0] == '\0') {
        return;
    }

    while (*command == ' ') {
        command++;
    }

    response_fd = open(response_path, O_WRONLY);
    if (response_fd < 0) {
        return;
    }

    handle_command(response_fd, command);
    close(response_fd);
}

static int run_fifo_server(const runtime_options_t *opts)
{
    struct pollfd pfd;
    int request_fd;

    if (opts->socket_path == NULL || opts->socket_path[0] == '\0') {
        return 0;
    }

    unlink(opts->socket_path);
    if (mkfifo(opts->socket_path, 0600) != 0) {
        perror("mkfifo");
        return 1;
    }

    request_fd = open(opts->socket_path, O_RDWR | O_NONBLOCK);
    if (request_fd < 0) {
        perror("open manager fifo");
        unlink(opts->socket_path);
        return 1;
    }

    pfd.fd = request_fd;
    pfd.events = POLLIN;
    while (!stop_requested) {
        int rc = poll(&pfd, 1, 500);

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            close(request_fd);
            unlink(opts->socket_path);
            return 1;
        }

        if (rc == 0) {
            if (refresh_pool() != 0) {
                close(request_fd);
                unlink(opts->socket_path);
                return 1;
            }
            continue;
        }

        if ((pfd.revents & POLLIN) != 0) {
            char request[2048];
            char *save = NULL;
            char *line;
            ssize_t got = read(request_fd, request, sizeof(request) - 1);

            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                perror("read manager fifo");
                close(request_fd);
                unlink(opts->socket_path);
                return 1;
            }

            if (got == 0) {
                continue;
            }

            request[got] = '\0';
            line = strtok_r(request, "\n", &save);
            while (line != NULL) {
                handle_fifo_line(line);
                line = strtok_r(NULL, "\n", &save);
            }
        }
    }

    close(request_fd);
    unlink(opts->socket_path);
    return 0;
}

int main(int argc, char **argv)
{
    runtime_options_t opts;

    if (!parse_args(argc, argv, &opts)) {
        fprintf(stderr,
                "usage: %s [--role manager|agent] [--config PATH] [--once] [--socket PATH]\n",
                argv[0]);
        return 1;
    }

    if (opts.config != NULL && access(opts.config, R_OK) != 0) {
        fprintf(stderr, "config not readable: %s\n", opts.config);
        return 1;
    }

    if (refresh_pool() != 0) {
        return 1;
    }

    printf("%s role=%s\n", program_label(opts.role), opts.role);
    if (print_pool() != 0) {
        return 1;
    }

    if (opts.once) {
        return 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (opts.socket_path == NULL && !opts.once &&
        strcmp(opts.role, "manager") == 0) {
        opts.socket_path = configured_socket_path();
    }

    if (opts.socket_path != NULL) {
        printf("manager socket=%s\n", opts.socket_path);
        return run_fifo_server(&opts);
    }

    while (!stop_requested) {
        sleep(2);
        if (!stop_requested && refresh_pool() != 0) {
            return 1;
        }
    }

    return 0;
}
