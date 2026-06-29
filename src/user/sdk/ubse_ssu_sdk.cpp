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
#define SDK_LOG_MESSAGE_SIZE 1024U
#define SDK_SETTLE_AFTER_DATA_MS 20

static int sdk_initialized;
static char sdk_socket_path[256];
static char sdk_log_path[256];
static uint32_t sdk_response_timeout_ms;
static unsigned int sdk_response_counter;
static unsigned long long sdk_audit_counter;
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
    char message[SDK_LOG_MESSAGE_SIZE];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    append_sdk_log(message);
}

static unsigned long long next_audit_id(void)
{
    sdk_audit_counter++;
    return sdk_audit_counter;
}

static void sdk_audit_api_beginf(unsigned long long call_id,
                                 const char *api,
                                 const char *fmt, ...)
{
    char detail[SDK_LOG_MESSAGE_SIZE];
    va_list ap;

    detail[0] = '\0';
    if (fmt != NULL) {
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);
    }

    sdk_logf("audit api=%s event=begin call_id=%llu %s",
             api, call_id, detail);
}

static void sdk_audit_api_endf(unsigned long long call_id,
                               const char *api,
                               long long start_ms,
                               ssu_err_t err,
                               const char *fmt, ...)
{
    char detail[SDK_LOG_MESSAGE_SIZE];
    long long elapsed_ms;
    va_list ap;

    detail[0] = '\0';
    if (fmt != NULL) {
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);
    }

    elapsed_ms = monotonic_ms() - start_ms;
    if (elapsed_ms < 0) {
        elapsed_ms = 0;
    }

    sdk_logf("audit api=%s event=end result=%s(%d) call_id=%llu elapsed_ms=%lld %s",
             api, ssu_err_name(err), err, call_id, elapsed_ms, detail);
}

static void sdk_audit_manager_endf(unsigned long long manager_id,
                                   const char *op,
                                   const char *event,
                                   long long start_ms,
                                   ssu_err_t err,
                                   const char *fmt, ...)
{
    char detail[SDK_LOG_MESSAGE_SIZE];
    long long elapsed_ms;
    va_list ap;

    detail[0] = '\0';
    if (fmt != NULL) {
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);
    }

    elapsed_ms = monotonic_ms() - start_ms;
    if (elapsed_ms < 0) {
        elapsed_ms = 0;
    }

    sdk_logf("audit manager event=%s op=%s result=%s(%d) manager_id=%llu elapsed_ms=%lld %s",
             event, op, ssu_err_name(err), err, manager_id, elapsed_ms,
             detail);
}

static void command_op(const char *command, char *out, size_t n)
{
    size_t len;
    const char *space;

    if (out == NULL || n == 0) {
        return;
    }

    out[0] = '\0';
    if (is_empty_string(command)) {
        copy_cstr(out, n, "(invalid)");
        return;
    }

    space = strchr(command, ' ');
    len = space == NULL ? strlen(command) : (size_t)(space - command);
    if (len >= n) {
        len = n - 1;
    }

    memcpy(out, command, len);
    out[len] = '\0';
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
    char op[64];
    const unsigned long long manager_id = next_audit_id();
    const long long start_ms = monotonic_ms();
    int request_fd;
    int rc;
    ssu_err_t err;

    command_op(command, op, sizeof(op));
    sdk_logf("audit manager event=begin op=%s manager_id=%llu request_fifo=%s command=\"%s\"",
             op, manager_id, request_path == NULL ? "(null)" : request_path,
             command == NULL ? "(null)" : command);

    if (is_empty_string(command) || response == NULL || n == 0 ||
        is_empty_string(request_path)) {
        set_last_errorf("invalid manager request command=%s request_fifo=%s",
                        command == NULL ? "(null)" : command,
                        request_path == NULL ? "(null)" : request_path);
        sdk_audit_manager_endf(manager_id, op, "response", start_ms,
                               SSU_ERR_INVALID, "bytes=0");
        return SSU_ERR_INVALID;
    }

    rc = snprintf(response_path, sizeof(response_path),
                  "/tmp/ubse-ssu-sdk-response-%ld-%u.fifo",
                  (long)getpid(), sdk_response_counter++);
    if (rc < 0 || (size_t)rc >= sizeof(response_path)) {
        set_last_errorf("response fifo path too long");
        sdk_audit_manager_endf(manager_id, op, "response", start_ms,
                               SSU_ERR_INVALID, "bytes=0");
        return SSU_ERR_INVALID;
    }

    unlink(response_path);
    if (mkfifo(response_path, 0600) != 0) {
        set_last_errorf("mkfifo response fifo failed response_fifo=%s errno=%d (%s)",
                        response_path, errno, strerror(errno));
        sdk_audit_manager_endf(manager_id, op, "response", start_ms,
                               SSU_ERR_IO, "response_fifo=%s bytes=0",
                               response_path);
        return SSU_ERR_IO;
    }

    sdk_logf("audit manager event=send op=%s manager_id=%llu request_fifo=%s response_fifo=%s command=\"%s\"",
             op, manager_id, request_path, response_path, command);

    rc = snprintf(request, sizeof(request), "%s %s\n",
                  response_path, command);
    if (rc < 0 || (size_t)rc >= sizeof(request)) {
        unlink(response_path);
        set_last_errorf("manager request too long command=%s", command);
        sdk_audit_manager_endf(manager_id, op, "response", start_ms,
                               SSU_ERR_INVALID, "response_fifo=%s bytes=0",
                               response_path);
        return SSU_ERR_INVALID;
    }

    request_fd = open(request_path, O_WRONLY | O_NONBLOCK);
    if (request_fd < 0) {
        ssu_err_t err = (errno == ENOENT || errno == ENXIO) ?
                            SSU_ERR_NOT_FOUND : SSU_ERR_IO;
        set_last_errorf("open manager fifo failed request_fifo=%s errno=%d (%s) hint=make sure ssu_mgr is running and host/container share /tmp",
                        request_path, errno, strerror(errno));
        unlink(response_path);
        sdk_audit_manager_endf(manager_id, op, "response", start_ms, err,
                               "request_fifo=%s response_fifo=%s bytes=0",
                               request_path, response_path);
        return err;
    }

    if (!send_all(request_fd, request, strlen(request))) {
        int saved_errno = errno;
        close(request_fd);
        unlink(response_path);
        set_last_errorf("write manager fifo failed request_fifo=%s errno=%d (%s)",
                        request_path, saved_errno, strerror(saved_errno));
        sdk_audit_manager_endf(manager_id, op, "response", start_ms,
                               SSU_ERR_IO,
                               "request_fifo=%s response_fifo=%s bytes=0",
                               request_path, response_path);
        return SSU_ERR_IO;
    }
    close(request_fd);

    err = read_response_fifo(request_path, response_path, response, n);
    sdk_audit_manager_endf(manager_id, op, "response", start_ms, err,
                           "request_fifo=%s response_fifo=%s bytes=%zu",
                           request_path, response_path,
                           err == SSU_OK ? strlen(response) : 0U);
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
    char op[64];
    const char *response_body = NULL;
    const unsigned long long manager_id = next_audit_id();
    const long long start_ms = monotonic_ms();
    ssu_err_t err;

    command_op(command, op, sizeof(op));
    memset(response, 0, sizeof(response));
    err = manager_request(command, response, sizeof(response));
    if (err != SSU_OK) {
        sdk_audit_manager_endf(manager_id, op, "status", start_ms, err,
                               "transport=failed");
        return err;
    }

    err = response_status(response, &response_body);
    if (body != NULL && body_size > 0) {
        copy_cstr(body, body_size, response_body);
    }
    if (err == SSU_OK) {
        char first_line[160];

        copy_first_line(first_line, sizeof(first_line), response_body);
        sdk_audit_manager_endf(manager_id, op, "status", start_ms, err,
                               "detail=%s", first_line);
        clear_last_error();
    } else if (response_body != NULL) {
        char first_line[160];

        copy_first_line(first_line, sizeof(first_line), response_body);
        sdk_audit_manager_endf(manager_id, op, "status", start_ms, err,
                               "detail=%s", first_line);
        set_last_errorf("manager returned %s (%d) command=%s detail=%s",
                        ssu_err_name(err), err, command,
                        response_body);
    } else {
        sdk_audit_manager_endf(manager_id, op, "status", start_ms, err,
                               "detail=(none)");
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
    const unsigned long long call_id = next_audit_id();
    const long long start_ms = monotonic_ms();

    clear_last_error();
    sdk_audit_api_beginf(call_id, "init", "opts=%s",
                         opts == NULL ? "default" : "custom");
    if (opts != NULL && opts->struct_size < required_size) {
        set_last_errorf("invalid init options struct_size=%zu required_min=%zu",
                        opts->struct_size, required_size);
        sdk_audit_api_endf(call_id, "init", start_ms, SSU_ERR_INVALID,
                           "struct_size=%zu required_min=%zu",
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
            sdk_audit_api_endf(call_id, "init", start_ms,
                               SSU_ERR_INVALID, "reason=invalid_socket_path");
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
            sdk_audit_api_endf(call_id, "init", start_ms,
                               SSU_ERR_INVALID, "reason=invalid_log_path");
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
    sdk_audit_api_endf(call_id, "init", start_ms, SSU_OK,
                       "socket=%s log=%s response_timeout_ms=%u",
                       active_socket_path(), active_log_path(),
                       (unsigned int)active_response_timeout_ms());
    return SSU_OK;
}

void ubse_ssu_sdk_fini(void)
{
    const unsigned long long call_id = next_audit_id();
    const long long start_ms = monotonic_ms();

    sdk_audit_api_beginf(call_id, "fini", "initialized=%d",
                         sdk_initialized);
    sdk_audit_api_endf(call_id, "fini", start_ms, SSU_OK,
                       "socket=%s log=%s",
                       active_socket_path(), active_log_path());
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
    const unsigned long long call_id = next_audit_id();
    const long long start_ms = monotonic_ms();
    ssu_err_t err;
    int rc;

    sdk_audit_api_beginf(
        call_id, "allocate",
        "req=%p out=%p size=%llu user=%s physical_disks=%u aggregate=%d share=%d host_count=%u disk_name=%s",
        (const void *)req, (void *)out,
        req == NULL ? 0ULL : (unsigned long long)req->size_bytes,
        req == NULL || is_empty_string(req->user_id) ? "-" : req->user_id,
        req == NULL ? 0U : req->physical_disk_count,
        req == NULL ? 0 : req->logical_disk_aggregate,
        req == NULL ? 0 : (int)req->allocation_type,
        req == NULL ? 0U : req->host_count,
        req == NULL || is_empty_string(req->disk_name) ? "-" :
                                                         req->disk_name);

    err = ensure_initialized();
    if (err != SSU_OK) {
        sdk_audit_api_endf(call_id, "allocate", start_ms, err,
                           "stage=init");
        return err;
    }

    if (req == NULL || out == NULL || req->size_bytes == 0) {
        set_last_errorf("allocate invalid args req=%p out=%p size=%llu",
                        (const void *)req, (void *)out,
                        req == NULL ? 0ULL :
                                      (unsigned long long)req->size_bytes);
        sdk_audit_api_endf(call_id, "allocate", start_ms,
                           SSU_ERR_INVALID, "stage=validate");
        return SSU_ERR_INVALID;
    }

    if (!build_host_list(host_list, sizeof(host_list), req->host_ids,
                         req->host_count)) {
        set_last_errorf("allocate invalid host list host_count=%u",
                        req->host_count);
        sdk_audit_api_endf(call_id, "allocate", start_ms,
                           SSU_ERR_INVALID, "stage=host_list");
        return SSU_ERR_INVALID;
    }

    user = is_empty_string(req->user_id) ? "-" : req->user_id;
    disk_name = is_empty_string(req->disk_name) ? "-" : req->disk_name;
    if (!token_is_safe(user) || !token_is_safe(disk_name)) {
        set_last_errorf("allocate invalid token user=%s disk_name=%s",
                        user, disk_name);
        sdk_audit_api_endf(call_id, "allocate", start_ms,
                           SSU_ERR_INVALID, "stage=token");
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
        set_last_errorf("allocate command too long");
        sdk_audit_api_endf(call_id, "allocate", start_ms,
                           SSU_ERR_INVALID, "stage=build_command");
        return SSU_ERR_INVALID;
    }

    memset(body, 0, sizeof(body));
    err = request_ok_body(command, body, sizeof(body));
    if (err != SSU_OK) {
        sdk_audit_api_endf(call_id, "allocate", start_ms, err,
                           "stage=manager");
        return err;
    }

    memset(out, 0, sizeof(*out));
    copy_first_line(out->request_id, sizeof(out->request_id), body);
    if (is_empty_string(out->request_id)) {
        set_last_errorf("allocate response missing request_id");
        sdk_audit_api_endf(call_id, "allocate", start_ms, SSU_ERR_IO,
                           "stage=parse_response");
        return SSU_ERR_IO;
    }

    sdk_audit_api_endf(call_id, "allocate", start_ms, SSU_OK,
                       "request_id=%s", out->request_id);
    return SSU_OK;
}

ssu_err_t ubse_ssu_sdk_free(const char *device_path)
{
    char command[SDK_COMMAND_SIZE];
    const unsigned long long call_id = next_audit_id();
    const long long start_ms = monotonic_ms();
    int rc;
    ssu_err_t err;

    sdk_audit_api_beginf(call_id, "free", "device_path=%s",
                         device_path == NULL ? "(null)" : device_path);
    err = ensure_initialized();
    if (err != SSU_OK) {
        sdk_audit_api_endf(call_id, "free", start_ms, err, "stage=init");
        return err;
    }

    if (!token_is_safe(device_path)) {
        set_last_errorf("free invalid device_path=%s",
                        device_path == NULL ? "(null)" : device_path);
        sdk_audit_api_endf(call_id, "free", start_ms, SSU_ERR_INVALID,
                           "stage=validate");
        return SSU_ERR_INVALID;
    }

    rc = snprintf(command, sizeof(command), "FREE %s", device_path);
    if (rc < 0 || (size_t)rc >= sizeof(command)) {
        set_last_errorf("free command too long device_path=%s", device_path);
        sdk_audit_api_endf(call_id, "free", start_ms, SSU_ERR_INVALID,
                           "stage=build_command");
        return SSU_ERR_INVALID;
    }

    err = request_ok_body(command, NULL, 0);
    sdk_audit_api_endf(call_id, "free", start_ms, err,
                       "device_path=%s", device_path);
    return err;
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
    const unsigned long long call_id = next_audit_id();
    const long long start_ms = monotonic_ms();
    ssu_err_t err;

    sdk_audit_api_beginf(call_id, "list", "out=%p capacity=%u",
                         (void *)out,
                         inout_count == NULL ? 0U : *inout_count);
    err = ensure_initialized();
    if (err != SSU_OK) {
        sdk_audit_api_endf(call_id, "list", start_ms, err, "stage=init");
        return err;
    }

    if (inout_count == NULL) {
        set_last_errorf("list invalid args inout_count=NULL");
        sdk_audit_api_endf(call_id, "list", start_ms, SSU_ERR_INVALID,
                           "stage=validate");
        return SSU_ERR_INVALID;
    }

    memset(body, 0, sizeof(body));
    err = request_ok_body("LIST", body, sizeof(body));
    if (err != SSU_OK) {
        sdk_audit_api_endf(call_id, "list", start_ms, err,
                           "stage=manager");
        return err;
    }

    line = strtok_r(body, "\n", &save);
    if (line == NULL ||
        sscanf(line, "pool entries: %u", &reported) != 1) {
        set_last_errorf("list invalid manager body first_line=%s",
                        line == NULL ? "(null)" : line);
        sdk_audit_api_endf(call_id, "list", start_ms, SSU_ERR_IO,
                           "stage=parse_header");
        return SSU_ERR_IO;
    }

    capacity = *inout_count;
    *inout_count = reported;
    if (reported == 0) {
        sdk_audit_api_endf(call_id, "list", start_ms, SSU_OK,
                           "reported=0 written=0");
        return SSU_OK;
    }

    if (out == NULL || capacity < reported) {
        sdk_audit_api_endf(call_id, "list", start_ms,
                           SSU_ERR_BUFFER_TOO_SMALL,
                           "reported=%u capacity=%u", reported, capacity);
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
            set_last_errorf("list invalid resource row: %s", line);
            sdk_audit_api_endf(call_id, "list", start_ms, SSU_ERR_IO,
                               "stage=parse_row parsed=%u reported=%u",
                               parsed, reported);
            return SSU_ERR_IO;
        }

        item.used_bytes = (uint64_t)used;
        item.total_bytes = (uint64_t)total;
        out[parsed++] = item;
    }

    err = parsed == reported ? SSU_OK : SSU_ERR_IO;
    if (err != SSU_OK) {
        set_last_errorf("list incomplete resource rows parsed=%u reported=%u",
                        parsed, reported);
    }
    sdk_audit_api_endf(call_id, "list", start_ms, err,
                       "reported=%u written=%u", reported, parsed);
    return err;
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
    const unsigned long long call_id = next_audit_id();
    const long long start_ms = monotonic_ms();

    sdk_audit_api_beginf(call_id, "allocate_result_get",
                         "request_id=%s out=%p",
                         request_id == NULL ? "(null)" : request_id,
                         (void *)out);
    err = ensure_initialized();
    if (err != SSU_OK) {
        sdk_audit_api_endf(call_id, "allocate_result_get", start_ms, err,
                           "stage=init");
        return err;
    }

    if (!token_is_safe(request_id) || out == NULL) {
        set_last_errorf("allocate_result_get invalid args request_id=%s out=%p",
                        request_id == NULL ? "(null)" : request_id,
                        (void *)out);
        sdk_audit_api_endf(call_id, "allocate_result_get", start_ms,
                           SSU_ERR_INVALID, "stage=validate");
        return SSU_ERR_INVALID;
    }

    rc = snprintf(command, sizeof(command), "ALLOCATE_RESULT %s",
                  request_id);
    if (rc < 0 || (size_t)rc >= sizeof(command)) {
        set_last_errorf("allocate_result_get command too long request_id=%s",
                        request_id);
        sdk_audit_api_endf(call_id, "allocate_result_get", start_ms,
                           SSU_ERR_INVALID, "stage=build_command");
        return SSU_ERR_INVALID;
    }

    memset(body, 0, sizeof(body));
    memset(out, 0, sizeof(*out));
    err = request_ok_body(command, body, sizeof(body));
    if (err != SSU_OK) {
        out->status = err;
        copy_cstr(out->error_message, sizeof(out->error_message), body);
        sdk_audit_api_endf(call_id, "allocate_result_get", start_ms, err,
                           "stage=manager request_id=%s", request_id);
        return err;
    }

    out->status = SSU_OK;
    line = strtok_r(body, "\n", &save);
    if (line == NULL) {
        out->status = SSU_ERR_IO;
        set_last_errorf("allocate_result_get response missing device_path request_id=%s",
                        request_id);
        sdk_audit_api_endf(call_id, "allocate_result_get", start_ms,
                           SSU_ERR_IO, "stage=parse_device");
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
            set_last_errorf("allocate_result_get invalid physical row request_id=%s row=%s",
                            request_id, line);
            sdk_audit_api_endf(call_id, "allocate_result_get", start_ms,
                               SSU_ERR_IO,
                               "stage=parse_physical device_path=%s",
                               out->device_path);
            return SSU_ERR_IO;
        }

        if (index >= SSU_API_MAX_PHYSICAL_DISKS) {
            out->status = SSU_ERR_BUFFER_TOO_SMALL;
            sdk_audit_api_endf(call_id, "allocate_result_get", start_ms,
                               SSU_ERR_BUFFER_TOO_SMALL,
                               "stage=physical_limit index=%u", index);
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

    sdk_audit_api_endf(call_id, "allocate_result_get", start_ms, SSU_OK,
                       "request_id=%s device_path=%s physical_disks=%u",
                       request_id, out->device_path,
                       out->physical_disk_count);
    return SSU_OK;
}

ssu_err_t ubse_ssu_sdk_mount(const char *device_path,
                             const char *host_id)
{
    char command[SDK_COMMAND_SIZE];
    const unsigned long long call_id = next_audit_id();
    const long long start_ms = monotonic_ms();
    int rc;
    ssu_err_t err;

    sdk_audit_api_beginf(call_id, "mount", "device_path=%s host_id=%s",
                         device_path == NULL ? "(null)" : device_path,
                         host_id == NULL ? "(null)" : host_id);
    err = ensure_initialized();
    if (err != SSU_OK) {
        sdk_audit_api_endf(call_id, "mount", start_ms, err, "stage=init");
        return err;
    }

    if (!token_is_safe(device_path) || !token_is_safe(host_id)) {
        set_last_errorf("mount invalid args device_path=%s host_id=%s",
                        device_path == NULL ? "(null)" : device_path,
                        host_id == NULL ? "(null)" : host_id);
        sdk_audit_api_endf(call_id, "mount", start_ms, SSU_ERR_INVALID,
                           "stage=validate");
        return SSU_ERR_INVALID;
    }

    rc = snprintf(command, sizeof(command), "MOUNT_DEV %s %s",
                  device_path, host_id);
    if (rc < 0 || (size_t)rc >= sizeof(command)) {
        set_last_errorf("mount command too long device_path=%s host_id=%s",
                        device_path, host_id);
        sdk_audit_api_endf(call_id, "mount", start_ms, SSU_ERR_INVALID,
                           "stage=build_command");
        return SSU_ERR_INVALID;
    }

    err = request_ok_body(command, NULL, 0);
    sdk_audit_api_endf(call_id, "mount", start_ms, err,
                       "device_path=%s host_id=%s", device_path, host_id);
    return err;
}

ssu_err_t ubse_ssu_sdk_unmount(const char *device_path)
{
    char command[SDK_COMMAND_SIZE];
    const unsigned long long call_id = next_audit_id();
    const long long start_ms = monotonic_ms();
    int rc;
    ssu_err_t err;

    sdk_audit_api_beginf(call_id, "unmount", "device_path=%s",
                         device_path == NULL ? "(null)" : device_path);
    err = ensure_initialized();
    if (err != SSU_OK) {
        sdk_audit_api_endf(call_id, "unmount", start_ms, err, "stage=init");
        return err;
    }

    if (!token_is_safe(device_path)) {
        set_last_errorf("unmount invalid device_path=%s",
                        device_path == NULL ? "(null)" : device_path);
        sdk_audit_api_endf(call_id, "unmount", start_ms, SSU_ERR_INVALID,
                           "stage=validate");
        return SSU_ERR_INVALID;
    }

    rc = snprintf(command, sizeof(command), "UNMOUNT %s", device_path);
    if (rc < 0 || (size_t)rc >= sizeof(command)) {
        set_last_errorf("unmount command too long device_path=%s",
                        device_path);
        sdk_audit_api_endf(call_id, "unmount", start_ms,
                           SSU_ERR_INVALID, "stage=build_command");
        return SSU_ERR_INVALID;
    }

    err = request_ok_body(command, NULL, 0);
    sdk_audit_api_endf(call_id, "unmount", start_ms, err,
                       "device_path=%s", device_path);
    return err;
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
