#include "ssu_plugin.h"

#include <stdio.h>
#include <string.h>

static const char *mock_name(void)
{
    return "mock";
}

static ssu_err_t mock_discover(ssu_resource_info_t *out, uint32_t *inout_count)
{
    (void)out;

    if (inout_count == NULL) {
        return SSU_ERR_INVALID;
    }

    *inout_count = 0;
    return SSU_OK;
}

static ssu_err_t mock_connect(const char *ssu_id, char *out_devname, size_t n)
{
    if (ssu_id == NULL || out_devname == NULL || n == 0) {
        return SSU_ERR_INVALID;
    }

    snprintf(out_devname, n, "/dev/nullb0");
    return SSU_OK;
}

static ssu_err_t mock_health_check(const char *ssu_id, char *out_state, size_t n)
{
    if (ssu_id == NULL || out_state == NULL || n == 0) {
        return SSU_ERR_INVALID;
    }

    snprintf(out_state, n, "ONLINE");
    return SSU_OK;
}

static ssu_err_t mock_create_ns(const ssu_extent_create_req_t *extent_req,
                                char *out_ns_id, size_t n,
                                uint64_t *out_phys_sector)
{
    if (extent_req == NULL || out_ns_id == NULL || n == 0 ||
        out_phys_sector == NULL) {
        return SSU_ERR_INVALID;
    }

    if (extent_req->policy != SSU_RELIABILITY_STRIPE) {
        return SSU_ERR_UNSUPPORTED;
    }

    snprintf(out_ns_id, n, "mock-ns0");
    *out_phys_sector = extent_req->phys_offset_hint;
    return SSU_OK;
}

static ssu_err_t mock_delete_ns(const char *ssu_id, const char *ns_id)
{
    if (ssu_id == NULL || ns_id == NULL) {
        return SSU_ERR_INVALID;
    }

    return SSU_OK;
}

static ssu_err_t mock_mount(const char *allocate_id, const char *host_id,
                            const char *logical_dev)
{
    if (allocate_id == NULL || host_id == NULL || logical_dev == NULL) {
        return SSU_ERR_INVALID;
    }

    return SSU_OK;
}

static ssu_err_t mock_unmount(const char *logical_dev)
{
    if (logical_dev == NULL) {
        return SSU_ERR_INVALID;
    }

    return SSU_OK;
}

static const ssu_plugin_ops_t ops = {
    .name = mock_name,
    .discover = mock_discover,
    .connect = mock_connect,
    .health_check = mock_health_check,
    .create_ns = mock_create_ns,
    .delete_ns = mock_delete_ns,
    .mount = mock_mount,
    .unmount = mock_unmount,
};

const ssu_plugin_ops_t *ssu_plugin_entry(void)
{
    return &ops;
}
