#ifndef SSU_API_H
#define SSU_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SSU_RELIABILITY_STRIPE = 0,
    SSU_RELIABILITY_EC = 1,
    SSU_RELIABILITY_REPLICA = 2,
} ssu_reliability_t;

typedef enum {
    SSU_SHARE_EXCLUSIVE = 0,
    SSU_SHARE_SHARED = 1,
} ssu_share_type_t;

typedef enum {
    SSU_MAP_DIR_FORWARD = 0,
    SSU_MAP_DIR_REVERSE = 1,
} ssu_map_dir_t;

typedef struct {
    uint64_t size_bytes;
    uint64_t io_bandwidth_bps;
    uint32_t physical_disk_count;
    ssu_reliability_t reliability;
    uint32_t replica_count;
    uint32_t ec_data;
    uint32_t ec_parity;
    ssu_share_type_t share_type;
    uint64_t share_range[2];
    ssu_map_dir_t map_dir;
    const char *tenant;
} ssu_alloc_req_t;

typedef struct {
    char allocate_id[64];
    uint64_t logical_size_bytes;
    uint32_t extent_count;
} ssu_alloc_result_t;

typedef struct {
    char ssu_id[64];
    char host_id[64];
    char ns_id[32];
    uint64_t logical_offset;
    uint64_t length;
} ssu_alloc_extent_t;

typedef struct {
    const char *allocate_id;
    const char *host_id;
    char logical_dev[64];
} ssu_mount_req_t;

typedef enum {
    SSU_QUERY_POOL = 0,
    SSU_QUERY_ALLOCATION = 1,
    SSU_QUERY_LOGDEV = 2,
} ssu_query_type_t;

typedef struct {
    ssu_query_type_t type;
    const char *allocate_id;
    const char *host_id;
    const char *logical_dev;
} ssu_query_req_t;

typedef struct {
    char ssu_id[64];
    uint64_t total_bytes;
    uint64_t used_bytes;
    char state[16];
    char host_id[64];
} ssu_resource_info_t;

typedef struct {
    char allocate_id[64];
    char tenant[64];
    ssu_reliability_t policy;
    ssu_share_type_t share_type;
    char state[16];
    char ssu_id[64];
    char ns_id[32];
    uint64_t logical_offset;
    uint64_t length;
    uint64_t phys_sector;
    uint32_t role_index;
} ssu_allocation_info_t;

typedef struct {
    char logical_dev[64];
    char host_id[64];
    char allocate_id[64];
    uint64_t logical_offset;
    uint64_t length;
    char phys_dev[64];
    char ns_id[32];
    uint64_t phys_sector;
} ssu_logdev_info_t;

#define SSU_API_MAX_PHYSICAL_DISKS 128U

typedef struct {
    uint64_t size_bytes;
    const char *user_id;
    uint32_t physical_disk_count;
    int logical_disk_aggregate;
    ssu_share_type_t allocation_type;
    const char *const *host_ids;
    uint32_t host_count;
} ssu_api_allocate_req_t;

typedef struct {
    char request_id[64];
} ssu_api_allocate_resp_t;

typedef enum {
    SSU_OK = 0,
    SSU_ERR_INVALID = -1,
    SSU_ERR_NO_RESOURCE = -2,
    SSU_ERR_NOT_FOUND = -3,
    SSU_ERR_BUSY = -4,
    SSU_ERR_IO = -5,
    SSU_ERR_KERNEL = -6,
    SSU_ERR_NS_EXISTS = -7,
    SSU_ERR_BUFFER_TOO_SMALL = -8,
    SSU_ERR_UNSUPPORTED = -9,
    SSU_ERR_INTERNAL = -99,
} ssu_err_t;

typedef struct {
    char ssu_id[64];
    char ns_id[32];
    uint64_t logical_offset;
    uint64_t length;
    uint64_t lba;
} ssu_api_physical_lba_t;

typedef struct {
    ssu_err_t status;
    char device_path[64];
    uint32_t physical_disk_count;
    ssu_api_physical_lba_t physical_disks[SSU_API_MAX_PHYSICAL_DISKS];
    char error_message[128];
} ssu_api_allocate_result_info_t;

ssu_err_t ssu_resource_alloc(const ssu_alloc_req_t *req,
                             ssu_alloc_result_t *out,
                             ssu_alloc_extent_t *out_extents,
                             uint32_t *inout_extent_count);

ssu_err_t ssu_resource_mount(const ssu_mount_req_t *req);

ssu_err_t ssu_resource_unmount(const char *logical_dev);

ssu_err_t ssu_resource_release(const char *allocate_id);

ssu_err_t ssu_resource_query(const ssu_query_req_t *req,
                             void *out_array,
                             size_t out_elem_size,
                             uint32_t *inout_count);

ssu_err_t ssu_api_allocate(const ssu_api_allocate_req_t *req,
                           ssu_api_allocate_resp_t *out);

ssu_err_t ssu_api_free(const char *device_path);

ssu_err_t ssu_api_list(ssu_resource_info_t *out,
                       uint32_t *inout_count);

ssu_err_t ssu_api_allocate_result_get(
    const char *request_id,
    ssu_api_allocate_result_info_t *out);

ssu_err_t ssu_api_mount(const char *device_path,
                        const char *host_id);

ssu_err_t ssu_api_unmount(const char *device_path);

#ifdef __cplusplus
}
#endif

#endif
