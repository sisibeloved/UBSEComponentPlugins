#include "ubse_ssu_sdk.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SDK_RESPONSE_SIZE 8192U
#define SDK_COMMAND_SIZE 512U

static int sdk_initialized;
static char sdk_socket_path[256];
static unsigned int sdk_response_counter;

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

static ssu_err_t manager_request(const char *command, char *response, size_t n)
{
    const char *request_path = active_socket_path();
    char response_path[128];
    char request[SDK_COMMAND_SIZE + 160];
    int request_fd;
    int response_fd;
    size_t used = 0;
    int rc;

    if (is_empty_string(command) || response == NULL || n == 0 ||
        is_empty_string(request_path)) {
        return SSU_ERR_INVALID;
    }

    rc = snprintf(response_path, sizeof(response_path),
                  "/tmp/ubse-ssu-sdk-response-%ld-%u.fifo",
                  (long)getpid(), sdk_response_counter++);
    if (rc < 0 || (size_t)rc >= sizeof(response_path)) {
        return SSU_ERR_INVALID;
    }

    unlink(response_path);
    if (mkfifo(response_path, 0600) != 0) {
        return SSU_ERR_IO;
    }

    rc = snprintf(request, sizeof(request), "%s %s\n",
                  response_path, command);
    if (rc < 0 || (size_t)rc >= sizeof(request)) {
        unlink(response_path);
        return SSU_ERR_INVALID;
    }

    request_fd = open(request_path, O_WRONLY | O_NONBLOCK);
    if (request_fd < 0) {
        ssu_err_t err = (errno == ENOENT || errno == ENXIO) ?
                            SSU_ERR_NOT_FOUND : SSU_ERR_IO;
        unlink(response_path);
        return err;
    }

    if (!send_all(request_fd, request, strlen(request))) {
        close(request_fd);
        unlink(response_path);
        return SSU_ERR_IO;
    }
    close(request_fd);

    response_fd = open(response_path, O_RDONLY);
    if (response_fd < 0) {
        unlink(response_path);
        return SSU_ERR_IO;
    }

    while (used + 1 < n) {
        ssize_t got = read(response_fd, response + used, n - used - 1);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(response_fd);
            unlink(response_path);
            return SSU_ERR_IO;
        }
        if (got == 0) {
            break;
        }
        used += (size_t)got;
    }

    response[used] = '\0';
    close(response_fd);
    unlink(response_path);
    return SSU_OK;
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
        return SSU_ERR_IO;
    }

    errno = 0;
    value = strtol(response + 4, &end, 10);
    newline = strchr(response, '\n');
    if (errno != 0 || end == response + 4 || newline == NULL ||
        end != newline) {
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
    if (opts != NULL && opts->struct_size < sizeof(*opts)) {
        return SSU_ERR_INVALID;
    }

    sdk_socket_path[0] = '\0';
    if (opts != NULL && !is_empty_string(opts->socket_path)) {
        if (!token_is_safe(opts->socket_path) ||
            strlen(opts->socket_path) >= sizeof(sdk_socket_path)) {
            return SSU_ERR_INVALID;
        }
        copy_cstr(sdk_socket_path, sizeof(sdk_socket_path),
                  opts->socket_path);
    }

    sdk_initialized = 1;
    return SSU_OK;
}

void ubse_ssu_sdk_fini(void)
{
    sdk_initialized = 0;
    sdk_socket_path[0] = '\0';
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
};

const ubse_ssu_sdk_ops_t *ubse_ssu_sdk_entry(void)
{
    return &sdk_ops;
}
