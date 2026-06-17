#ifndef SSU_CONTROLLER_H
#define SSU_CONTROLLER_H

#include <stdint.h>

#include "ssu_api.h"
#include "ssu_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

ssu_err_t ssu_controller_refresh_pool(const ssu_plugin_ops_t *plugin);

ssu_err_t ssu_controller_query_pool(ssu_resource_info_t *out,
                                    uint32_t *inout_count);

ssu_err_t ssu_controller_alloc(const ssu_plugin_ops_t *plugin,
                               const ssu_alloc_req_t *req,
                               ssu_alloc_result_t *out,
                               ssu_alloc_extent_t *out_extents,
                               uint32_t *inout_extent_count);

ssu_err_t ssu_controller_release(const ssu_plugin_ops_t *plugin,
                                 const char *allocate_id);

ssu_err_t ssu_controller_query_allocations(ssu_allocation_info_t *out,
                                           uint32_t *inout_count);

ssu_err_t ssu_controller_mount(const ssu_plugin_ops_t *plugin,
                               const ssu_mount_req_t *req);

ssu_err_t ssu_controller_unmount(const ssu_plugin_ops_t *plugin,
                                 const char *logical_dev);

ssu_err_t ssu_controller_query_logdevs(ssu_logdev_info_t *out,
                                       uint32_t *inout_count);

#ifdef __cplusplus
}
#endif

#endif
