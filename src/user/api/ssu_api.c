#include "ssu_api.h"
#include "ssu_controller.h"

#include <stdio.h>
#include <string.h>

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

ssu_err_t ssu_resource_alloc(const ssu_alloc_req_t *req,
                             ssu_alloc_result_t *out,
                             ssu_alloc_extent_t *out_extents,
                             uint32_t *inout_extent_count)
{
    (void)out_extents;

    if (req == NULL || out == NULL) {
        return SSU_ERR_INVALID;
    }

    if (req->reliability != SSU_RELIABILITY_STRIPE) {
        return SSU_ERR_UNSUPPORTED;
    }

    if (out_extents != NULL && inout_extent_count == NULL) {
        return SSU_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->allocate_id, sizeof(out->allocate_id), "stub-alloc-0");
    out->logical_size_bytes = req->size_bytes;
    out->extent_count = 0;

    if (inout_extent_count != NULL) {
        *inout_extent_count = 0;
    }

    return SSU_OK;
}

ssu_err_t ssu_resource_mount(const ssu_mount_req_t *req)
{
    if (req == NULL || is_empty_string(req->allocate_id) ||
        is_empty_string(req->host_id) || req->logical_dev[0] == '\0') {
        return SSU_ERR_INVALID;
    }

    return SSU_OK;
}

ssu_err_t ssu_resource_unmount(const char *logical_dev)
{
    if (is_empty_string(logical_dev)) {
        return SSU_ERR_INVALID;
    }

    return SSU_OK;
}

ssu_err_t ssu_resource_release(const char *allocate_id)
{
    if (is_empty_string(allocate_id)) {
        return SSU_ERR_INVALID;
    }

    return SSU_OK;
}

ssu_err_t ssu_resource_query(const ssu_query_req_t *req,
                             void *out_array,
                             size_t out_elem_size,
                             uint32_t *inout_count)
{
    size_t expected_size;

    if (req == NULL || inout_count == NULL) {
        return SSU_ERR_INVALID;
    }

    expected_size = query_elem_size(req->type);
    if (expected_size == 0 || out_elem_size != expected_size) {
        return SSU_ERR_INVALID;
    }

    if (req->type == SSU_QUERY_POOL) {
        return ssu_controller_query_pool((ssu_resource_info_t *)out_array,
                                         inout_count);
    }

    if (out_array == NULL || *inout_count == 0) {
        *inout_count = 0;
        return SSU_OK;
    }

    *inout_count = 0;
    return SSU_OK;
}
