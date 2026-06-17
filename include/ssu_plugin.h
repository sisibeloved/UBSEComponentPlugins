#ifndef SSU_PLUGIN_H
#define SSU_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#include "ssu_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *allocate_id;
    const char *ssu_id;
    uint64_t logical_offset;
    uint64_t length;
    uint64_t phys_offset_hint;
    ssu_reliability_t policy;
    uint32_t role_index;
    const char *tenant;
} ssu_extent_create_req_t;

typedef struct ssu_plugin_ops {
    const char *(*name)(void);
    ssu_err_t (*discover)(ssu_resource_info_t *out, uint32_t *inout_count);
    ssu_err_t (*connect)(const char *ssu_id, char *out_devname, size_t n);
    ssu_err_t (*health_check)(const char *ssu_id, char *out_state, size_t n);
    ssu_err_t (*create_ns)(const ssu_extent_create_req_t *extent_req,
                           char *out_ns_id, size_t n,
                           uint64_t *out_phys_sector);
    ssu_err_t (*delete_ns)(const char *ssu_id, const char *ns_id);
    ssu_err_t (*mount)(const char *allocate_id, const char *host_id,
                       const char *logical_dev);
    ssu_err_t (*unmount)(const char *logical_dev);
} ssu_plugin_ops_t;

const ssu_plugin_ops_t *ssu_plugin_entry(void);

#ifdef __cplusplus
}
#endif

#endif
