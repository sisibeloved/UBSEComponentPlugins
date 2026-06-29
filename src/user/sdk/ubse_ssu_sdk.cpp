#include "ubse_ssu_sdk.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SDK_RESPONSE_SIZE 8192U
#define SDK_COMMAND_SIZE 512U
#define SDK_LAST_ERROR_SIZE 512U
#define SDK_SETTLE_AFTER_DATA_MS 20

static int sdk_initialized;
static char sdk_socket_path[256];
static char sdk_log_path[256];
static uint32_t sdk_response_timeout_ms;
static unsigned int sdk_response_counter;
static char sdk_last_error[SDK_LAST_ERROR_SIZE];

static int is_empty_string(const char *s)
{
    return s == NULL || s[0] == '\0';
}

static void copy_cstr(char *dst, size_t n, const char *src)
{
    if (dst == NULL || n == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, n, "%s", src);
}

static int token_is_safe(const char *s)
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

static int init_option_has_field(const ubse_ssu_sdk_init_options_t *opts,
                                 size_t offset,
                                 size_t size)
{
    return opts != NULL && opts->struct_size >= offset + size;
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

static const char *active_socket_path(void)
{
    const char *path;

    if (!is_empty_string(sdk_socket_path)) {
        return sdk_socket_path;
    }

    path = getenv("SSU_MGR_SOCKET");
    if (!is_empty_string(path)) {
        return path;
    }

    return UBSE_SSU_SDK_DEFAULT_SOCKET;
}

static const char *active_log_path(void)
{
    const char *path;

    if (!is_empty_string(sdk_log_path)) {
        return sdk_log_path;
    }

    path = getenv("UBSE_SSU_SDK_LOG");
    if (!is_empty_string(path)) {
        return path;
    }

    return UBSE_SSU_SDK_DEFAULT_LOG;
}

static uint32_t active_response_timeout_ms(void)
{
    const char *value;
    char *end = NULL;
    unsigned long parsed;

    if (sdk_response_timeout_ms != 0) {
        return sdk_response_timeout_ms;
    }

    value = getenv("UBSE_SSU_SDK_RESPONSE_TIMEOUT_MS");
    if (is_empty_string(value)) {
        return UBSE_SSU_SDK_DEFAULT_RESPONSE_TIMEOUT_MS;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed == 0 || parsed > 600000UL) {
        return UBSE_SSU_SDK_DEFAULT_RESPONSE_TIMEOUT_MS;
    }

    return (uint32_t)parsed;
}

static long long monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void append_sdk_log(const char *message)
{
    const char *path = active_log_path();
    FILE *fp;
    time_t now;
    struct tm tm_buf;
    char ts[64];

    if (is_empty_string(path) || message == NULL) {
        return;
    }

    fp = fopen(path, "a");
    if (fp == NULL) {
        return;
    }

    now = time(NULL);
    if (localtime_r(&now, &tm_buf) != NULL) {
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tm_buf);
    } else {
        snprintf(ts, sizeof(ts), "time-unavailable");
    }

    fprintf(fp, "%s pid=%ld %s\n", ts, (long)getpid(), message);
    fclose(fp);
}

static void sdk_logf(const char *fmt, ...)
{
    char message[SDK_LAST_ERROR_SIZE];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    append_sdk_log(message);
}

static void clear_last_error(void)
{
    sdk_last_error[0] = '\0';
}

static void set_last_errorf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(sdk_last_error, sizeof(sdk_last_error), fmt, ap);
    va_end(ap);

    append_sdk_log(sdk_last_error);
}

static ssu_err_t ensure_initialized(void)
{
    if (sdk_initialized) {
        return SSU_OK;
    }

    return ubse_ssu_sdk_init(NULL);
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

static ssu_err_t read_response_fifo(const char *request_path,
                                    const char *response_path,
                                    char *response,
                                    size_t n)
{
    const uint32_t timeout_ms = active_response_timeout_ms();
    const long long deadline = monotonic_ms() + (long long)timeout_ms;
    int response_fd;
    size_t used = 0;
    int saw_data = 0;

    response_fd = open(response_path, O_RDWR | O_NONBLOCK);
    if (response_fd < 0) {
        set_last_errorf("open response fifo failed response_fifo=%s errno=%d (%s)",
                        response_path, errno, strerror(errno));
        return SSU_ERR_IO;
    }

    while (used + 1 < n) {
        struct pollfd pfd;
        int wait_ms;
        int rc;

        if (saw_data) {
            wait_ms = SDK_SETTLE_AFTER_DATA_MS;
        } else {
            long long remaining = deadline - monotonic_ms();
            if (remaining <= 0) {
                close(response_fd);
                set_last_errorf("timeout waiting for manager response request_fifo=%s response_fifo=%s timeout_ms=%u hint=make sure ssu_mgr is running and host/container share /tmp",
                                request_path, response_path,
                                (unsigned int)timeout_ms);
                return SSU_ERR_IO;
            }
            wait_ms = remaining > INT32_MAX ? INT32_MAX : (int)remaining;
        }

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = response_fd;
        pfd.events = POLLIN;
        rc = poll(&pfd, 1, wait_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(response_fd);
            set_last_errorf("poll response fifo failed response_fifo=%s errno=%d (%s)",
                            response_path, errno, strerror(errno));
            return SSU_ERR_IO;
        }

        if (rc == 0) {
            if (saw_data) {
                break;
            }
            close(response_fd);
            set_last_errorf("timeout waiting for manager response request_fifo=%s response_fifo=%s timeout_ms=%u hint=make sure ssu_mgr is running and host/container share /tmp",
                            request_path, response_path,
                            (unsigned int)timeout_ms);
            return SSU_ERR_IO;
        }

        if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
            close(response_fd);
            set_last_errorf("response fifo poll failed response_fifo=%s revents=0x%x",
                            response_path, (unsigned int)pfd.revents);
            return SSU_ERR_IO;
        }

        if ((pfd.revents & POLLIN) == 0) {
            if (saw_data && (pfd.revents & POLLHUP) != 0) {
                break;
            }
            continue;
        }

        for (;;) {
            ssize_t got = read(response_fd, response + used, n - used - 1);
            if (got > 0) {
                used += (size_t)got;
                saw_data = 1;
                if (used + 1 >= n) {
                    break;
                }
                continue;
            }
            if (got == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            close(response_fd);
            set_last_errorf("read response fifo failed response_fifo=%s errno=%d (%s)",
                            response_path, errno, strerror(errno));
            return SSU_ERR_IO;
        }
    }

    response[used] = '\0';
    close(response_fd);
    if (used == 0) {
        set_last_errorf("empty manager response request_fifo=%s response_fifo=%s",
                        request_path, response_path);
        return SSU_ERR_IO;
    }

    return SSU_OK;
}

static ssu_err_t manager_request(const char *command, char *response, size_t n)
{
    const char *request_path = active_socket_path();
    char response_path[128];
    char request[SDK_COMMAND_SIZE + 160];
    int request_fd;
    int rc;
    ssu_err_t err;

    if (is_empty_string(command) || response == NULL || n == 0 ||
        is_empty_string(request_path)) {
        set_last_errorf("invalid manager request command=%s request_fifo=%s",
                        command == NULL ? "(null)" : command,
                        request_path == NULL ? "(null)" : request_path);
        return SSU_ERR_INVALID;
    }

    rc = snprintf(response_path, sizeof(response_path),
                  "/tmp/ubse-ssu-sdk-response-%ld-%u.fifo",
                  (long)getpid(), sdk_response_counter++);
    if (rc < 0 || (size_t)rc >= sizeof(response_path)) {
        set_last_errorf("response fifo path too long");
        return SSU_ERR_INVALID;
    }

    unlink(response_path);
    if (mkfifo(response_path, 0600) != 0) {
        set_last_errorf("mkfifo response fifo failed response_fifo=%s errno=%d (%s)",
                        response_path, errno, strerror(errno));
        return SSU_ERR_IO;
    }

    rc = snprintf(request, sizeof(request), "%s %s\n",
                  response_path, command);
    if (rc < 0 || (size_t)rc >= sizeof(request)) {
        unlink(response_path);
        set_last_errorf("manager request too long command=%s", command);
        return SSU_ERR_INVALID;
    }

    request_fd = open(request_path, O_WRONLY | O_NONBLOCK);
    if (request_fd < 0) {
        ssu_err_t err = (errno == ENOENT || errno == ENXIO) ?
                            SSU_ERR_NOT_FOUND : SSU_ERR_IO;
        set_last_errorf("open manager fifo failed request_fifo=%s errno=%d (%s) hint=make sure ssu_mgr is running and host/container share /tmp",
                        request_path, errno, strerror(errno));
        unlink(response_path);
        return err;
    }

    if (!send_all(request_fd, request, strlen(request))) {
        int saved_errno = errno;
        close(request_fd);
        unlink(response_path);
        set_last_errorf("write manager fifo failed request_fifo=%s errno=%d (%s)",
                        request_path, saved_errno, strerror(saved_errno));
        return SSU_ERR_IO;
    }
    close(request_fd);

    err = read_response_fifo(request_path, response_path, response, n);
    unlink(response_path);
    return err;
}

static ssu_err_t response_status(const char *response, const char **body)
{
    const char *newline;
    long value;
    char *end = NULL;

    if (response == NULL || body == NULL) {
        return SSU_ERR_INVALID;
    }

    if (strncmp(response, "OK\n", 3) == 0) {
        *body = response + 3;
        return SSU_OK;
    }

    if (strncmp(response, "ERR ", 4) != 0) {
        set_last_errorf("invalid manager response: %.80s", response);
        return SSU_ERR_IO;
    }

    errno = 0;
    value = strtol(response + 4, &end, 10);
    newline = strchr(response, '\n');
    if (errno != 0 || end == response + 4 || newline == NULL ||
        end != newline) {
        set_last_errorf("invalid manager error response: %.80s", response);
        return SSU_ERR_IO;
    }

    *body = newline + 1;
    return (ssu_err_t)value;
}

static void copy_first_line(char *dst, size_t n, const char *body)
{
    const char *end;
    size_t len;

    if (dst == NULL || n == 0) {
        return;
    }

    if (body == NULL) {
        dst[0] = '\0';
        return;
    }

    end = strchr(body, '\n');
    len = end == NULL ? strlen(body) : (size_t)(end - body);
    if (len >= n) {
        len = n - 1;
    }

    memcpy(dst, body, len);
    dst[len] = '\0';
}

static ssu_err_t request_ok_body(const char *command,
                                 char *body,
                                 size_t body_size)
{
    char response[SDK_RESPONSE_SIZE];
    const char *response_body = NULL;
    ssu_err_t err;

    memset(response, 0, sizeof(response));
    err = manager_request(command, response, sizeof(response));
    if (err != SSU_OK) {
        return err;
    }

    err = response_status(response, &response_body);
    if (body != NULL && body_size > 0) {
        copy_cstr(body, body_size, response_body);
    }
    if (err == SSU_OK) {
        clear_last_error();
    } else if (response_body != NULL) {
        set_last_errorf("manager returned %s (%d) command=%s detail=%s",
                        ssu_err_name(err), err, command,
                        response_body);
    }
    return err;
}

static int build_host_list(char *buf, size_t n,
                           const char *const *hosts, uint32_t host_count)
{
    const char *default_host = "local";
    size_t used = 0;
    uint32_t i;

    if (buf == NULL || n == 0) {
        return 0;
    }

    buf[0] = '\0';
    if (host_count == 0) {
        return snprintf(buf, n, "%s", default_host) > 0;
    }

    if (hosts == NULL) {
        return 0;
    }

    for (i = 0; i < host_count; i++) {
        int rc;

        if (!token_is_safe(hosts[i]) || strchr(hosts[i], ',') != NULL) {
            return 0;
        }

        rc = snprintf(buf + used, n - used, "%s%s",
                      i == 0 ? "" : ",", hosts[i]);
        if (rc < 0 || (size_t)rc >= n - used) {
            return 0;
        }
        used += (size_t)rc;
    }

    return 1;
}

ssu_err_t ubse_ssu_sdk_init(const ubse_ssu_sdk_init_options_t *opts)
{
    const size_t required_size =
        offsetof(ubse_ssu_sdk_init_options_t, log_path);

    clear_last_error();
    if (opts != NULL && opts->struct_size < required_size) {
        set_last_errorf("invalid init options struct_size=%zu required_min=%zu",
                        opts->struct_size, required_size);
        return SSU_ERR_INVALID;
    }

    sdk_socket_path[0] = '\0';
    sdk_log_path[0] = '\0';
    sdk_response_timeout_ms = 0;
    if (opts != NULL && !is_empty_string(opts->socket_path)) {
        if (!token_is_safe(opts->socket_path) ||
            strlen(opts->socket_path) >= sizeof(sdk_socket_path)) {
            set_last_errorf("invalid socket path");
            return SSU_ERR_INVALID;
        }
        copy_cstr(sdk_socket_path, sizeof(sdk_socket_path),
                  opts->socket_path);
    }

    if (init_option_has_field(opts,
                              offsetof(ubse_ssu_sdk_init_options_t, log_path),
                              sizeof(opts->log_path)) &&
        !is_empty_string(opts->log_path)) {
        if (strlen(opts->log_path) >= sizeof(sdk_log_path)) {
            set_last_errorf("invalid log path: too long");
            return SSU_ERR_INVALID;
        }
        copy_cstr(sdk_log_path, sizeof(sdk_log_path), opts->log_path);
    }

    if (init_option_has_field(
            opts,
            offsetof(ubse_ssu_sdk_init_options_t, response_timeout_ms),
            sizeof(opts->response_timeout_ms))) {
        sdk_response_timeout_ms = opts->response_timeout_ms;
    }

    sdk_initialized = 1;
    sdk_logf("sdk init socket=%s log=%s response_timeout_ms=%u",
             active_socket_path(), active_log_path(),
             (unsigned int)active_response_timeout_ms());
    return SSU_OK;
}

void ubse_ssu_sdk_fini(void)
{
    sdk_initialized = 0;
    sdk_socket_path[0] = '\0';
    sdk_log_path[0] = '\0';
    sdk_response_timeout_ms = 0;
}

ssu_err_t ubse_ssu_sdk_allocate(const ssu_api_allocate_req_t *req,
                                ssu_api_allocate_resp_t *out)
{
    char host_list[256];
    char command[SDK_COMMAND_SIZE];
    char body[SDK_RESPONSE_SIZE];
    const char *user;
    const char *disk_name;
    ssu_err_t err;
    int rc;

    err = ensure_initialized();
    if (err != SSU_OK) {
        return err;
    }

    if (req == NULL || out == NULL || req->size_bytes == 0) {
        return SSU_ERR_INVALID;
    }

    if (!build_host_list(host_list, sizeof(host_list), req->host_ids,
                         req->host_count)) {
        return SSU_ERR_INVALID;
    }

    user = is_empty_string(req->user_id) ? "-" : req->user_id;
    disk_name = is_empty_string(req->disk_name) ? "-" : req->disk_name;
    if (!token_is_safe(user) || !token_is_safe(disk_name)) {
        return SSU_ERR_INVALID;
    }

    rc = snprintf(command, sizeof(command), "ALLOCATE %llu %d %u %d %s %s %s",
                  (unsigned long long)req->size_bytes,
                  (int)req->allocation_type,
                  req->physical_disk_count,
                  req->logical_disk_aggregate,
                  user,
                  host_list,
                  disk_name);
    if (rc < 0 || (size_t)rc >= sizeof(command)) {
        return SSU_ERR_INVALID;
    }

    memset(body, 0, sizeof(body));
    err = request_ok_body(command, body, sizeof(body));
    if (err != SSU_OK) {
        return err;
    }

    memset(out, 0, sizeof(*out));
    copy_first_line(out->request_id, sizeof(out->request_id), body);
    return is_empty_string(out->request_id) ? SSU_ERR_IO : SSU_OK;
}

ssu_err_t ubse_ssu_sdk_free(const char *device_path)
{
    char command[SDK_COMMAND_SIZE];
    int rc;
    ssu_err_t err = ensure_initialized();

    if (err != SSU_OK) {
        return err;
    }

    if (!token_is_safe(device_path)) {
        return SSU_ERR_INVALID;
    }

    rc = snprintf(command, sizeof(command), "FREE %s", device_path);
    if (rc < 0 || (size_t)rc >= sizeof(command)) {
        return SSU_ERR_INVALID;
    }

    return request_ok_body(command, NULL, 0);
}

ssu_err_t ubse_ssu_sdk_list(ssu_resource_info_t *out,
                            uint32_t *inout_count)
{
    char body[SDK_RESPONSE_SIZE];
    char *save = NULL;
    char *line;
    uint32_t reported = 0;
    uint32_t capacity;
    uint32_t parsed = 0;
    ssu_err_t err = ensure_initialized();

    if (err != SSU_OK) {
        return err;
    }

    if (inout_count == NULL) {
        return SSU_ERR_INVALID;
    }

    memset(body, 0, sizeof(body));
    err = request_ok_body("LIST", body, sizeof(body));
    if (err != SSU_OK) {
        return err;
    }

    line = strtok_r(body, "\n", &save);
    if (line == NULL ||
        sscanf(line, "pool entries: %u", &reported) != 1) {
        return SSU_ERR_IO;
    }

    capacity = *inout_count;
    *inout_count = reported;
    if (reported == 0) {
        return SSU_OK;
    }

    if (out == NULL || capacity < reported) {
        return SSU_ERR_BUFFER_TOO_SMALL;
    }

    while ((line = strtok_r(NULL, "\n", &save)) != NULL &&
           parsed < reported) {
        unsigned long long used = 0;
        unsigned long long total = 0;
        ssu_resource_info_t item;

        memset(&item, 0, sizeof(item));
        if (sscanf(line, "%63s %63s %15s %llu/%llu",
                   item.ssu_id,
                   item.host_id,
                   item.state,
                   &used,
                   &total) != 5) {
            return SSU_ERR_IO;
        }

        item.used_bytes = (uint64_t)used;
        item.total_bytes = (uint64_t)total;
        out[parsed++] = item;
    }

    return parsed == reported ? SSU_OK : SSU_ERR_IO;
}

ssu_err_t ubse_ssu_sdk_allocate_result_get(
    const char *request_id,
    ssu_api_allocate_result_info_t *out)
{
    char command[SDK_COMMAND_SIZE];
    char body[SDK_RESPONSE_SIZE];
    char *save = NULL;
    char *line;
    ssu_err_t err;
    int rc;

    err = ensure_initialized();
    if (err != SSU_OK) {
        return err;
    }

    if (!token_is_safe(request_id) || out == NULL) {
        return SSU_ERR_INVALID;
    }

    rc = snprintf(command, sizeof(command), "ALLOCATE_RESULT %s",
                  request_id);
    if (rc < 0 || (size_t)rc >= sizeof(command)) {
        return SSU_ERR_INVALID;
    }

    memset(body, 0, sizeof(body));
    memset(out, 0, sizeof(*out));
    err = request_ok_body(command, body, sizeof(body));
    if (err != SSU_OK) {
        out->status = err;
        copy_cstr(out->error_message, sizeof(out->error_message), body);
        return err;
    }

    out->status = SSU_OK;
    line = strtok_r(body, "\n", &save);
    if (line == NULL) {
        out->status = SSU_ERR_IO;
        return SSU_ERR_IO;
    }
    copy_cstr(out->device_path, sizeof(out->device_path), line);

    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        unsigned int index = 0;
        unsigned long long logical_offset = 0;
        unsigned long long length = 0;
        unsigned long long lba = 0;
        ssu_api_physical_lba_t item;

        memset(&item, 0, sizeof(item));
        if (sscanf(line, "physical %u %63s %31s %llu %llu lba=%llu",
                   &index,
                   item.ssu_id,
                   item.ns_id,
                   &logical_offset,
                   &length,
                   &lba) != 6) {
            out->status = SSU_ERR_IO;
            return SSU_ERR_IO;
        }

        if (index >= SSU_API_MAX_PHYSICAL_DISKS) {
            out->status = SSU_ERR_BUFFER_TOO_SMALL;
            return SSU_ERR_BUFFER_TOO_SMALL;
        }

        item.logical_offset = (uint64_t)logical_offset;
        item.length = (uint64_t)length;
        item.lba = (uint64_t)lba;
        out->physical_disks[index] = item;
        if (index + 1U > out->physical_disk_count) {
            out->physical_disk_count = index + 1U;
        }
    }

    return SSU_OK;
}

ssu_err_t ubse_ssu_sdk_mount(const char *device_path,
                             const char *host_id)
{
    char command[SDK_COMMAND_SIZE];
    int rc;
    ssu_err_t err = ensure_initialized();

    if (err != SSU_OK) {
        return err;
    }

    if (!token_is_safe(device_path) || !token_is_safe(host_id)) {
        return SSU_ERR_INVALID;
    }

    rc = snprintf(command, sizeof(command), "MOUNT_DEV %s %s",
                  device_path, host_id);
    if (rc < 0 || (size_t)rc >= sizeof(command)) {
        return SSU_ERR_INVALID;
    }

    return request_ok_body(command, NULL, 0);
}

ssu_err_t ubse_ssu_sdk_unmount(const char *device_path)
{
    char command[SDK_COMMAND_SIZE];
    int rc;
    ssu_err_t err = ensure_initialized();

    if (err != SSU_OK) {
        return err;
    }

    if (!token_is_safe(device_path)) {
        return SSU_ERR_INVALID;
    }

    rc = snprintf(command, sizeof(command), "UNMOUNT %s", device_path);
    if (rc < 0 || (size_t)rc >= sizeof(command)) {
        return SSU_ERR_INVALID;
    }

    return request_ok_body(command, NULL, 0);
}

const char *ubse_ssu_sdk_last_error(void)
{
    return sdk_last_error;
}

static const ubse_ssu_sdk_ops_t sdk_ops = {
    sizeof(ubse_ssu_sdk_ops_t),
    ubse_ssu_sdk_init,
    ubse_ssu_sdk_fini,
    ubse_ssu_sdk_allocate,
    ubse_ssu_sdk_free,
    ubse_ssu_sdk_list,
    ubse_ssu_sdk_allocate_result_get,
    ubse_ssu_sdk_mount,
    ubse_ssu_sdk_unmount,
    ubse_ssu_sdk_last_error,
};

const ubse_ssu_sdk_ops_t *ubse_ssu_sdk_entry(void)
{
    return &sdk_ops;
}
