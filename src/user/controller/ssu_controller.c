#include "ssu_controller.h"

#include <string.h>

#define SSU_CONTROLLER_MAX_RESOURCES 128U

static ssu_resource_info_t pool[SSU_CONTROLLER_MAX_RESOURCES];
static uint32_t pool_count;

ssu_err_t ssu_controller_refresh_pool(const ssu_plugin_ops_t *plugin)
{
    ssu_resource_info_t discovered[SSU_CONTROLLER_MAX_RESOURCES];
    uint32_t count = SSU_CONTROLLER_MAX_RESOURCES;
    ssu_err_t err;

    if (plugin == NULL || plugin->discover == NULL) {
        return SSU_ERR_INVALID;
    }

    memset(discovered, 0, sizeof(discovered));
    err = plugin->discover(discovered, &count);
    if (err != SSU_OK) {
        return err;
    }

    if (count > SSU_CONTROLLER_MAX_RESOURCES) {
        return SSU_ERR_INTERNAL;
    }

    memcpy(pool, discovered, sizeof(pool[0]) * count);
    pool_count = count;
    return SSU_OK;
}

ssu_err_t ssu_controller_query_pool(ssu_resource_info_t *out,
                                    uint32_t *inout_count)
{
    if (inout_count == NULL) {
        return SSU_ERR_INVALID;
    }

    if (out == NULL || *inout_count < pool_count) {
        *inout_count = pool_count;
        return pool_count == 0 ? SSU_OK : SSU_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(out, pool, sizeof(pool[0]) * pool_count);
    *inout_count = pool_count;
    return SSU_OK;
}
