#ifndef UBSE_SSU_SDK_H
#define UBSE_SSU_SDK_H

#include "ssu_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UBSE_SSU_SDK_DEFAULT_SOCKET "/tmp/ubse-ssu-mgr.fifo"

typedef struct {
    /* Set to sizeof(ubse_ssu_sdk_init_options_t). */
    size_t struct_size;
    /* Optional. NULL or empty uses SSU_MGR_SOCKET, then the default socket. */
    const char *socket_path;
} ubse_ssu_sdk_init_options_t;

typedef struct {
    size_t struct_size;

    ssu_err_t (*init)(const ubse_ssu_sdk_init_options_t *opts);
    void (*fini)(void);

    ssu_err_t (*allocate)(const ssu_api_allocate_req_t *req,
                          ssu_api_allocate_resp_t *out);
    ssu_err_t (*free)(const char *device_path);
    ssu_err_t (*list)(ssu_resource_info_t *out,
                      uint32_t *inout_count);
    ssu_err_t (*allocate_result_get)(
        const char *request_id,
        ssu_api_allocate_result_info_t *out);
    ssu_err_t (*mount)(const char *device_path,
                       const char *host_id);
    ssu_err_t (*unmount)(const char *device_path);
} ubse_ssu_sdk_ops_t;

ssu_err_t ubse_ssu_sdk_init(const ubse_ssu_sdk_init_options_t *opts);
void ubse_ssu_sdk_fini(void);

ssu_err_t ubse_ssu_sdk_allocate(const ssu_api_allocate_req_t *req,
                                ssu_api_allocate_resp_t *out);
ssu_err_t ubse_ssu_sdk_free(const char *device_path);
ssu_err_t ubse_ssu_sdk_list(ssu_resource_info_t *out,
                            uint32_t *inout_count);
ssu_err_t ubse_ssu_sdk_allocate_result_get(
    const char *request_id,
    ssu_api_allocate_result_info_t *out);
ssu_err_t ubse_ssu_sdk_mount(const char *device_path,
                             const char *host_id);
ssu_err_t ubse_ssu_sdk_unmount(const char *device_path);

/* dlopen users only need to dlsym this single entry point. */
const ubse_ssu_sdk_ops_t *ubse_ssu_sdk_entry(void);

#ifdef __cplusplus
}
#endif

#endif
