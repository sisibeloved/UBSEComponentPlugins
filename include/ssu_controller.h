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

#ifdef __cplusplus
}
#endif

#endif
