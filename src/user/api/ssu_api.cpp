#include "ssu_api.h"
#include "ssu_controller.h"
#include "ssu_plugin.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SSU_API_MAX_REQUESTS 128U

typedef struct {
    int active;
    char request_id[64];
    char device_path[64];
} api_request_t;

static api_request_t api_requests[SSU_API_MAX_REQUESTS];
static uint32_t next_api_device_index;
static int api_initialized;

static int is_empty_string(const char *s)
{
    return s == NULL || s[0] == '\0';
}

static size_t query_elem_size(ssu_query_type_t type)
{
    switch (type) {
    case SSU_QUERY_POOL:
        return sizeof(ssu_resource_info_t);
    case SSU_QUERY_ALLOCATION:
        return sizeof(ssu_allocation_info_t);
    case SSU_QUERY_LOGDEV:
        return sizeof(ssu_logdev_info_t);
    default:
        return 0;
    }
}

static const ssu_plugin_ops_t *default_plugin(void)
{
    return ssu_plugin_entry();
}

static ssu_err_t ensure_api_initialized(void)
{
    if (api_initialized) {
        return SSU_OK;
    }

    return ssu_api_init(NULL);
}

static void reset_api_request_cache(void)
{
    memset(api_requests, 0, sizeof(api_requests));
    next_api_device_index = 0;
}

static void copy_cstr(char *dst, size_t n, const char *src)
{
    if (n == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, n, "%s", src);
}

static int compare_allocation_offset(const void *a, const void *b)
{
    const ssu_allocation_info_t *left = (const ssu_allocation_info_t *)a;
    const ssu_allocation_info_t *right = (const ssu_allocation_info_t *)b;

    if (left->logical_offset < right->logical_offset) {
        return -1;
    }

    if (left->logical_offset > right->logical_offset) {
        return 1;
    }

    return 0;
}

static const char *ssu_err_message(ssu_err_t err)
{
    switch (err) {
    case SSU_OK:
        return "";
    case SSU_ERR_INVALID:
        return "invalid argument";
    case SSU_ERR_NO_RESOURCE:
        return "no usable SSU resource";
    case SSU_ERR_NOT_FOUND:
        return "requested SSU object was not found";
    case SSU_ERR_BUSY:
        return "resource is still in use";
    case SSU_ERR_IO:
        return "device I/O operation failed";
    case SSU_ERR_KERNEL:
        return "kernel data path operation failed";
    case SSU_ERR_NS_EXISTS:
        return "namespace already exists";
    case SSU_ERR_BUFFER_TOO_SMALL:
        return "caller buffer is too small";
    case SSU_ERR_UNSUPPORTED:
        return "operation is not supported by this build";
    case SSU_ERR_INTERNAL:
        return "internal error";
    default:
        return "unknown error";
    }
}

static ssu_err_t refresh_default_pool(void)
{
    const ssu_plugin_ops_t *plugin = default_plugin();

    if (plugin == NULL) {
        return SSU_ERR_INTERNAL;
    }

    return ssu_controller_refresh_pool(plugin);
}

static int parse_prefixed_u32(const char *s, const char *prefix,
                              uint32_t *out)
{
    const char *p;
    uint64_t value = 0;
    size_t prefix_len;

    if (s == NULL || prefix == NULL || out == NULL) {
        return 0;
    }

    prefix_len = strlen(prefix);
    if (strncmp(s, prefix, prefix_len) != 0) {
        return 0;
    }

    p = s + prefix_len;
    if (*p == '\0') {
        return 0;
    }

    while (*p != '\0') {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        value = (value * 10U) + (uint64_t)(*p - '0');
        if (value > UINT32_MAX) {
            return 0;
        }
        p++;
    }

    *out = (uint32_t)value;
    return 1;
}

static int parse_logical_dev_index(const char *device_path, uint32_t *out)
{
    const char *base;

    if (is_empty_string(device_path) || out == NULL) {
        return 0;
    }

    base = strrchr(device_path, '/');
    base = base == NULL ? device_path : base + 1;
    return parse_prefixed_u32(base, "ssu", out);
}

static int allocation_disk_name(const char *request_id, char *out, size_t n)
{
    ssu_query_req_t req = {};
    ssu_allocation_info_t allocations[128];
    uint32_t count = 128;
    uint32_t i;
    ssu_err_t err;

    if (is_empty_string(request_id) || out == NULL || n == 0) {
        return 0;
    }

    req.type = SSU_QUERY_ALLOCATION;
    err = ssu_resource_query(&req, allocations, sizeof(allocations[0]),
                             &count);
    if (err != SSU_OK) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        if (strcmp(allocations[i].allocate_id, request_id) == 0 &&
            !is_empty_string(allocations[i].disk_name)) {
            copy_cstr(out, n, allocations[i].disk_name);
            return 1;
        }
    }

    return 0;
}

static int allocation_name_exists(const char *disk_name)
{
    ssu_query_req_t req = {};
    ssu_allocation_info_t allocations[128];
    uint32_t count = 128;
    uint32_t i;
    ssu_err_t err;

    if (is_empty_string(disk_name)) {
        return 0;
    }

    req.type = SSU_QUERY_ALLOCATION;
    err = ssu_resource_query(&req, allocations, sizeof(allocations[0]),
                             &count);
    if (err != SSU_OK) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        if (strcmp(allocations[i].disk_name, disk_name) == 0) {
            return 1;
        }
    }

    return 0;
}

static void default_device_path_for_request(const char *request_id,
                                            const char *disk_name,
                                            char *out, size_t n)
{
    uint32_t index;

    if (!is_empty_string(disk_name)) {
        snprintf(out, n, "/dev/ssu/%s", disk_name);
        return;
    }

    if (parse_prefixed_u32(request_id, "alloc-", &index)) {
        snprintf(out, n, "/dev/ssu/ssu%u", index);
        if (index >= next_api_device_index) {
            next_api_device_index = index + 1U;
        }
        return;
    }

    snprintf(out, n, "/dev/ssu/ssu%u", next_api_device_index);
    next_api_device_index++;
}

static int device_path_to_request_id(const char *device_path,
                                     char *out, size_t n)
{
    uint32_t index;

    if (!parse_logical_dev_index(device_path, &index)) {
        return 0;
    }

    snprintf(out, n, "alloc-%u", index);
    return 1;
}

static api_request_t *find_request_by_id(const char *request_id)
{
    uint32_t i;

    for (i = 0; i < SSU_API_MAX_REQUESTS; i++) {
        if (api_requests[i].active &&
            strcmp(api_requests[i].request_id, request_id) == 0) {
            return &api_requests[i];
        }
    }

    return NULL;
}

static api_request_t *find_request_by_device(const char *device_path)
{
    uint32_t i;

    for (i = 0; i < SSU_API_MAX_REQUESTS; i++) {
        if (api_requests[i].active &&
            strcmp(api_requests[i].device_path, device_path) == 0) {
            return &api_requests[i];
        }
    }

    return NULL;
}

static api_request_t *register_request_path_with_name(
    const char *request_id,
    const char *disk_name)
{
    api_request_t *slot = find_request_by_id(request_id);
    char allocation_name[64];
    uint32_t i;

    memset(allocation_name, 0, sizeof(allocation_name));
    if (is_empty_string(disk_name)) {
        allocation_disk_name(request_id, allocation_name,
                             sizeof(allocation_name));
        disk_name = allocation_name;
    }

    if (slot != NULL) {
        default_device_path_for_request(request_id, disk_name,
                                        slot->device_path,
                                        sizeof(slot->device_path));
        return slot;
    }

    for (i = 0; i < SSU_API_MAX_REQUESTS; i++) {
        if (!api_requests[i].active) {
            memset(&api_requests[i], 0, sizeof(api_requests[i]));
            api_requests[i].active = 1;
            copy_cstr(api_requests[i].request_id,
                      sizeof(api_requests[i].request_id), request_id);
            default_device_path_for_request(request_id, disk_name,
                                            api_requests[i].device_path,
                                            sizeof(api_requests[i].device_path));
            return &api_requests[i];
        }
    }

    return NULL;
}

static api_request_t *register_request_path(const char *request_id)
{
    return register_request_path_with_name(request_id, NULL);
}

static void forget_request_path(const char *request_id)
{
    api_request_t *slot = find_request_by_id(request_id);

    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
}

static int allocation_exists(const char *request_id)
{
    ssu_query_req_t req = {};
    ssu_allocation_info_t allocations[128];
    uint32_t count = 128;
    uint32_t i;
    ssu_err_t err;

    req.type = SSU_QUERY_ALLOCATION;
    err = ssu_resource_query(&req, allocations, sizeof(allocations[0]),
                             &count);
    if (err != SSU_OK) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        if (strcmp(allocations[i].allocate_id, request_id) == 0) {
            return 1;
        }
    }

    return 0;
}

static ssu_err_t fill_allocation_lbas(
    const char *request_id,
    ssu_api_allocate_result_info_t *out)
{
    ssu_query_req_t req = {};
    ssu_allocation_info_t allocations[SSU_API_MAX_PHYSICAL_DISKS];
    ssu_allocation_info_t matches[SSU_API_MAX_PHYSICAL_DISKS];
    uint32_t count = SSU_API_MAX_PHYSICAL_DISKS;
    uint32_t matched = 0;
    uint32_t i;
    ssu_err_t err;

    req.type = SSU_QUERY_ALLOCATION;
    err = ssu_resource_query(&req, allocations, sizeof(allocations[0]),
                             &count);
    if (err != SSU_OK) {
        return err;
    }

    for (i = 0; i < count; i++) {
        if (strcmp(allocations[i].allocate_id, request_id) == 0) {
            if (matched >= SSU_API_MAX_PHYSICAL_DISKS) {
                return SSU_ERR_BUFFER_TOO_SMALL;
            }
            matches[matched++] = allocations[i];
        }
    }

    if (matched == 0) {
        return SSU_ERR_NOT_FOUND;
    }

    qsort(matches, matched, sizeof(matches[0]), compare_allocation_offset);
    out->physical_disk_count = matched;
    for (i = 0; i < matched; i++) {
        copy_cstr(out->physical_disks[i].ssu_id,
                  sizeof(out->physical_disks[i].ssu_id),
                  matches[i].ssu_id);
        copy_cstr(out->physical_disks[i].ns_id,
                  sizeof(out->physical_disks[i].ns_id),
                  matches[i].ns_id);
        out->physical_disks[i].logical_offset = matches[i].logical_offset;
        out->physical_disks[i].length = matches[i].length;
        out->physical_disks[i].lba = matches[i].phys_sector;
    }

    return SSU_OK;
}

static int logdev_request_id(const char *device_path, char *out, size_t n)
{
    ssu_query_req_t req = {};
    ssu_logdev_info_t logdevs[128];
    uint32_t count = 128;
    uint32_t i;
    ssu_err_t err;

    req.type = SSU_QUERY_LOGDEV;
    err = ssu_resource_query(&req, logdevs, sizeof(logdevs[0]), &count);
    if (err != SSU_OK) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        if (strcmp(logdevs[i].logical_dev, device_path) == 0) {
            copy_cstr(out, n, logdevs[i].allocate_id);
            return 1;
        }
    }

    return 0;
}

static int named_allocation_request_id(const char *device_path,
                                       char *out,
                                       size_t n)
{
    ssu_query_req_t req = {};
    ssu_allocation_info_t allocations[128];
    const char *base;
    uint32_t count = 128;
    uint32_t i;
    ssu_err_t err;

    if (is_empty_string(device_path) || out == NULL || n == 0) {
        return 0;
    }

    base = strrchr(device_path, '/');
    base = base == NULL ? device_path : base + 1;
    if (is_empty_string(base)) {
        return 0;
    }

    req.type = SSU_QUERY_ALLOCATION;
    err = ssu_resource_query(&req, allocations, sizeof(allocations[0]),
                             &count);
    if (err != SSU_OK) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        if (!is_empty_string(allocations[i].disk_name) &&
            strcmp(allocations[i].disk_name, base) == 0) {
            register_request_path_with_name(allocations[i].allocate_id,
                                            allocations[i].disk_name);
            copy_cstr(out, n, allocations[i].allocate_id);
            return 1;
        }
    }

    return 0;
}

static ssu_err_t request_id_from_device_path(const char *device_path,
                                             char *out, size_t n)
{
    api_request_t *slot;
    char request_id[64];

    if (is_empty_string(device_path) || out == NULL || n == 0) {
        return SSU_ERR_INVALID;
    }

    if (logdev_request_id(device_path, out, n)) {
        return SSU_OK;
    }

    slot = find_request_by_device(device_path);
    if (slot != NULL && allocation_exists(slot->request_id)) {
        copy_cstr(out, n, slot->request_id);
        return SSU_OK;
    }

    memset(request_id, 0, sizeof(request_id));
    if (device_path_to_request_id(device_path, request_id,
                                  sizeof(request_id)) &&
        allocation_exists(request_id)) {
        register_request_path(request_id);
        copy_cstr(out, n, request_id);
        return SSU_OK;
    }

    if (named_allocation_request_id(device_path, out, n)) {
        return SSU_OK;
    }

    return SSU_ERR_NOT_FOUND;
}

ssu_err_t ssu_resource_alloc(const ssu_alloc_req_t *req,
                             ssu_alloc_result_t *out,
                             ssu_alloc_extent_t *out_extents,
                             uint32_t *inout_extent_count)
{
    ssu_alloc_extent_t *tmp_extents = NULL;
    uint32_t tmp_extent_count = 0;
    ssu_err_t err;

    err = ensure_api_initialized();
    if (err != SSU_OK) {
        return err;
    }

    if (req == NULL || out == NULL) {
        return SSU_ERR_INVALID;
    }

    if (req->reliability != SSU_RELIABILITY_STRIPE) {
        return SSU_ERR_UNSUPPORTED;
    }

    if (out_extents != NULL && inout_extent_count == NULL) {
        return SSU_ERR_INVALID;
    }

    err = refresh_default_pool();
    if (err != SSU_OK) {
        return err;
    }

    if (out_extents == NULL && inout_extent_count == NULL) {
        err = ssu_controller_alloc(default_plugin(), req, out, NULL,
                                   &tmp_extent_count);
        if (err != SSU_ERR_BUFFER_TOO_SMALL) {
            return err;
        }

        tmp_extents = (ssu_alloc_extent_t *)calloc(tmp_extent_count,
                                                   sizeof(*tmp_extents));
        if (tmp_extents == NULL) {
            return SSU_ERR_NO_RESOURCE;
        }

        err = ssu_controller_alloc(default_plugin(), req, out, tmp_extents,
                                   &tmp_extent_count);
        free(tmp_extents);
        return err;
    }

    return ssu_controller_alloc(default_plugin(), req, out, out_extents,
                                inout_extent_count);
}

ssu_err_t ssu_resource_mount(const ssu_mount_req_t *req)
{
    ssu_err_t err = ensure_api_initialized();
    if (err != SSU_OK) {
        return err;
    }

    if (req == NULL || is_empty_string(req->allocate_id) ||
        is_empty_string(req->host_id) || req->logical_dev[0] == '\0') {
        return SSU_ERR_INVALID;
    }

    return ssu_controller_mount(default_plugin(), req);
}

ssu_err_t ssu_resource_unmount(const char *logical_dev)
{
    ssu_err_t err = ensure_api_initialized();
    if (err != SSU_OK) {
        return err;
    }

    if (is_empty_string(logical_dev)) {
        return SSU_ERR_INVALID;
    }

    return ssu_controller_unmount(default_plugin(), logical_dev);
}

ssu_err_t ssu_resource_release(const char *allocate_id)
{
    ssu_err_t err = ensure_api_initialized();
    if (err != SSU_OK) {
        return err;
    }

    if (is_empty_string(allocate_id)) {
        return SSU_ERR_INVALID;
    }

    return ssu_controller_release(default_plugin(), allocate_id);
}

ssu_err_t ssu_resource_query(const ssu_query_req_t *req,
                             void *out_array,
                             size_t out_elem_size,
                             uint32_t *inout_count)
{
    size_t expected_size;
    ssu_err_t err;

    err = ensure_api_initialized();
    if (err != SSU_OK) {
        return err;
    }

    if (req == NULL || inout_count == NULL) {
        return SSU_ERR_INVALID;
    }

    expected_size = query_elem_size(req->type);
    if (expected_size == 0 || out_elem_size != expected_size) {
        return SSU_ERR_INVALID;
    }

    if (req->type == SSU_QUERY_POOL) {
        err = refresh_default_pool();
        if (err != SSU_OK) {
            return err;
        }

        return ssu_controller_query_pool((ssu_resource_info_t *)out_array,
                                         inout_count);
    }

    if (req->type == SSU_QUERY_ALLOCATION) {
        return ssu_controller_query_allocations(
            (ssu_allocation_info_t *)out_array, inout_count);
    }

    if (req->type == SSU_QUERY_LOGDEV) {
        return ssu_controller_query_logdevs((ssu_logdev_info_t *)out_array,
                                            inout_count);
    }

    *inout_count = 0;
    return SSU_OK;
}

ssu_err_t ssu_api_allocate(const ssu_api_allocate_req_t *req,
                           ssu_api_allocate_resp_t *out)
{
    ssu_alloc_req_t resource_req = {};
    ssu_alloc_result_t alloc_result;
    ssu_err_t err;
    int existing_named_allocation = 0;

    err = ensure_api_initialized();
    if (err != SSU_OK) {
        return err;
    }

    if (req == NULL || out == NULL || req->size_bytes == 0 ||
        is_empty_string(req->user_id) || req->host_ids == NULL ||
        req->host_count == 0) {
        return SSU_ERR_INVALID;
    }

    if (req->allocation_type != SSU_SHARE_EXCLUSIVE &&
        req->allocation_type != SSU_SHARE_SHARED) {
        return SSU_ERR_INVALID;
    }

    if (req->allocation_type == SSU_SHARE_EXCLUSIVE &&
        req->host_count != 1) {
        return SSU_ERR_INVALID;
    }

    for (uint32_t i = 0; i < req->host_count; i++) {
        if (is_empty_string(req->host_ids[i])) {
            return SSU_ERR_INVALID;
        }
    }

    if (req->logical_disk_aggregate < 0) {
        return SSU_ERR_UNSUPPORTED;
    }

    resource_req.size_bytes = req->size_bytes;
    resource_req.disk_name = req->disk_name;
    resource_req.physical_disk_count =
        req->physical_disk_count == 0 ? 1 : req->physical_disk_count;
    resource_req.reliability = SSU_RELIABILITY_STRIPE;
    resource_req.share_type = req->allocation_type;
    resource_req.map_dir = SSU_MAP_DIR_FORWARD;
    resource_req.tenant = req->user_id;
    resource_req.host_ids = req->host_ids;
    resource_req.host_count = req->host_count;

    if (!is_empty_string(req->disk_name)) {
        existing_named_allocation = allocation_name_exists(req->disk_name);
    }

    memset(&alloc_result, 0, sizeof(alloc_result));
    err = ssu_resource_alloc(&resource_req, &alloc_result, NULL, NULL);
    if (err != SSU_OK) {
        return err;
    }

    memset(out, 0, sizeof(*out));
    copy_cstr(out->request_id, sizeof(out->request_id),
              alloc_result.allocate_id);
    if (register_request_path_with_name(out->request_id,
                                        req->disk_name) == NULL) {
        if (!existing_named_allocation) {
            ssu_resource_release(out->request_id);
        }
        memset(out, 0, sizeof(*out));
        return SSU_ERR_INTERNAL;
    }

    return SSU_OK;
}

ssu_err_t ssu_api_init(const ssu_api_init_options_t *opts)
{
    if (opts != NULL && opts->struct_size < sizeof(*opts)) {
        return SSU_ERR_INVALID;
    }

    if (default_plugin() == NULL) {
        return SSU_ERR_INTERNAL;
    }

    api_initialized = 1;
    return SSU_OK;
}

void ssu_api_fini(void)
{
    api_initialized = 0;
    reset_api_request_cache();
}

const ssu_api_ops_t *ssu_api_entry(void)
{
    static const ssu_api_ops_t ops = {
        sizeof(ssu_api_ops_t),
        ssu_api_init,
        ssu_api_fini,
        ssu_api_allocate,
        ssu_api_free,
        ssu_api_list,
        ssu_api_allocate_result_get,
        ssu_api_mount,
        ssu_api_unmount,
        ssu_resource_alloc,
        ssu_resource_mount,
        ssu_resource_unmount,
        ssu_resource_release,
        ssu_resource_query,
    };

    return &ops;
}

ssu_err_t ssu_api_free(const char *device_path)
{
    char request_id[64];
    ssu_err_t err;

    err = ensure_api_initialized();
    if (err != SSU_OK) {
        return err;
    }

    memset(request_id, 0, sizeof(request_id));
    err = request_id_from_device_path(device_path, request_id,
                                      sizeof(request_id));
    if (err != SSU_OK) {
        return err;
    }

    err = ssu_resource_release(request_id);
    if (err == SSU_OK) {
        forget_request_path(request_id);
    }

    return err;
}

ssu_err_t ssu_api_list(ssu_resource_info_t *out,
                       uint32_t *inout_count)
{
    ssu_query_req_t req = {};
    ssu_err_t err = ensure_api_initialized();
    if (err != SSU_OK) {
        return err;
    }

    req.type = SSU_QUERY_POOL;
    return ssu_resource_query(&req, out, sizeof(ssu_resource_info_t),
                              inout_count);
}

ssu_err_t ssu_api_allocate_result_get(
    const char *request_id,
    ssu_api_allocate_result_info_t *out)
{
    api_request_t *slot;
    ssu_err_t err = ensure_api_initialized();
    if (err != SSU_OK) {
        return err;
    }

    if (is_empty_string(request_id) || out == NULL) {
        return SSU_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    if (!allocation_exists(request_id)) {
        out->status = SSU_ERR_NOT_FOUND;
        copy_cstr(out->error_message, sizeof(out->error_message),
                  ssu_err_message(SSU_ERR_NOT_FOUND));
        return SSU_ERR_NOT_FOUND;
    }

    slot = register_request_path(request_id);
    if (slot == NULL) {
        out->status = SSU_ERR_INTERNAL;
        copy_cstr(out->error_message, sizeof(out->error_message),
                  ssu_err_message(SSU_ERR_INTERNAL));
        return SSU_ERR_INTERNAL;
    }

    out->status = SSU_OK;
    copy_cstr(out->device_path, sizeof(out->device_path), slot->device_path);
    err = fill_allocation_lbas(request_id, out);
    if (err != SSU_OK) {
        out->status = err;
        copy_cstr(out->error_message, sizeof(out->error_message),
                  ssu_err_message(err));
        return err;
    }

    return SSU_OK;
}

ssu_err_t ssu_api_mount(const char *device_path,
                        const char *host_id)
{
    ssu_mount_req_t req = {};
    char request_id[64];
    ssu_err_t err;

    err = ensure_api_initialized();
    if (err != SSU_OK) {
        return err;
    }

    if (is_empty_string(device_path) || is_empty_string(host_id)) {
        return SSU_ERR_INVALID;
    }

    memset(request_id, 0, sizeof(request_id));
    err = request_id_from_device_path(device_path, request_id,
                                      sizeof(request_id));
    if (err != SSU_OK) {
        return err;
    }

    req.allocate_id = request_id;
    req.host_id = host_id;
    copy_cstr(req.logical_dev, sizeof(req.logical_dev), device_path);
    err = ssu_resource_mount(&req);
    if (err == SSU_OK) {
        register_request_path(request_id);
    }

    return err;
}

ssu_err_t ssu_api_unmount(const char *device_path)
{
    ssu_err_t err = ensure_api_initialized();
    if (err != SSU_OK) {
        return err;
    }

    if (is_empty_string(device_path)) {
        return SSU_ERR_INVALID;
    }

    return ssu_resource_unmount(device_path);
}
