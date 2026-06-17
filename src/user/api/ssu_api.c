#include "ssu_api.h"
#include "ssu_controller.h"
#include "ssu_plugin.h"

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

static const ssu_plugin_ops_t *default_plugin(void)
{
    return ssu_plugin_entry();
}

static ssu_err_t refresh_default_pool(void)
{
    const ssu_plugin_ops_t *plugin = default_plugin();

    if (plugin == NULL) {
        return SSU_ERR_INTERNAL;
    }

    return ssu_controller_refresh_pool(plugin);
}

ssu_err_t ssu_resource_alloc(const ssu_alloc_req_t *req,
                             ssu_alloc_result_t *out,
                             ssu_alloc_extent_t *out_extents,
                             uint32_t *inout_extent_count)
{
    ssu_alloc_extent_t tmp_extents[1];
    uint32_t tmp_extent_count = 1;
    ssu_err_t err;

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
        out_extents = tmp_extents;
        inout_extent_count = &tmp_extent_count;
    }

    return ssu_controller_alloc(default_plugin(), req, out, out_extents,
                                inout_extent_count);
}

ssu_err_t ssu_resource_mount(const ssu_mount_req_t *req)
{
    if (req == NULL || is_empty_string(req->allocate_id) ||
        is_empty_string(req->host_id) || req->logical_dev[0] == '\0') {
        return SSU_ERR_INVALID;
    }

    return ssu_controller_mount(default_plugin(), req);
}

ssu_err_t ssu_resource_unmount(const char *logical_dev)
{
    if (is_empty_string(logical_dev)) {
        return SSU_ERR_INVALID;
    }

    return ssu_controller_unmount(default_plugin(), logical_dev);
}

ssu_err_t ssu_resource_release(const char *allocate_id)
{
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

    if (req == NULL || inout_count == NULL) {
        return SSU_ERR_INVALID;
    }

    expected_size = query_elem_size(req->type);
    if (expected_size == 0 || out_elem_size != expected_size) {
        return SSU_ERR_INVALID;
    }

    if (req->type == SSU_QUERY_POOL) {
        ssu_err_t err = refresh_default_pool();
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
