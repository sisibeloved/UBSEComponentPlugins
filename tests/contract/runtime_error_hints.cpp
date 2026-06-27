#include "../../include/ssu_controller.h"

#include <cstring>
#include <iostream>
#include <string>

#define main ssu_runtime_main_for_test
#include "../../src/user/runtime/ssu_runtime.cpp"
#undef main

static const char *test_lbc_mock_name(void)
{
    return "lbc_mock";
}

extern "C" const ssu_plugin_ops_t *ssu_plugin_entry(void)
{
    static const ssu_plugin_ops_t ops = {
        test_lbc_mock_name,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
    };

    return &ops;
}

extern "C" ssu_err_t
ssu_controller_refresh_pool(const ssu_plugin_ops_t *plugin)
{
    (void)plugin;
    return SSU_OK;
}

extern "C" ssu_err_t
ssu_controller_query_pool(ssu_resource_info_t *out, uint32_t *inout_count)
{
    (void)out;
    if (inout_count != nullptr) {
        *inout_count = 0;
    }
    return SSU_OK;
}

extern "C" ssu_err_t
ssu_controller_alloc(const ssu_plugin_ops_t *plugin,
                     const ssu_alloc_req_t *req,
                     ssu_alloc_result_t *out,
                     ssu_alloc_extent_t *out_extents,
                     uint32_t *inout_extent_count)
{
    (void)plugin;
    (void)req;
    (void)out;
    (void)out_extents;
    (void)inout_extent_count;
    return SSU_ERR_INTERNAL;
}

extern "C" ssu_err_t
ssu_controller_release(const ssu_plugin_ops_t *plugin,
                       const char *allocate_id)
{
    (void)plugin;
    (void)allocate_id;
    return SSU_ERR_INTERNAL;
}

extern "C" ssu_err_t
ssu_controller_query_allocations(ssu_allocation_info_t *out,
                                 uint32_t *inout_count)
{
    (void)out;
    if (inout_count != nullptr) {
        *inout_count = 0;
    }
    return SSU_OK;
}

extern "C" ssu_err_t
ssu_controller_mount(const ssu_plugin_ops_t *plugin,
                     const ssu_mount_req_t *req)
{
    (void)plugin;
    (void)req;
    return SSU_ERR_INTERNAL;
}

extern "C" ssu_err_t
ssu_controller_unmount(const ssu_plugin_ops_t *plugin,
                       const char *logical_dev)
{
    (void)plugin;
    (void)logical_dev;
    return SSU_ERR_INTERNAL;
}

extern "C" ssu_err_t
ssu_controller_query_logdevs(ssu_logdev_info_t *out,
                             uint32_t *inout_count)
{
    (void)out;
    if (inout_count != nullptr) {
        *inout_count = 0;
    }
    return SSU_OK;
}

extern "C" ssu_err_t
ssu_resource_alloc(const ssu_alloc_req_t *req,
                   ssu_alloc_result_t *out,
                   ssu_alloc_extent_t *out_extents,
                   uint32_t *inout_extent_count)
{
    (void)req;
    (void)out;
    (void)out_extents;
    (void)inout_extent_count;
    return SSU_ERR_INTERNAL;
}

extern "C" ssu_err_t ssu_resource_mount(const ssu_mount_req_t *req)
{
    (void)req;
    return SSU_ERR_INTERNAL;
}

extern "C" ssu_err_t ssu_resource_unmount(const char *logical_dev)
{
    (void)logical_dev;
    return SSU_ERR_INTERNAL;
}

extern "C" ssu_err_t ssu_resource_release(const char *allocate_id)
{
    (void)allocate_id;
    return SSU_ERR_INTERNAL;
}

extern "C" ssu_err_t
ssu_resource_query(const ssu_query_req_t *req,
                   void *out_array,
                   size_t out_elem_size,
                   uint32_t *inout_count)
{
    (void)req;
    (void)out_array;
    (void)out_elem_size;
    if (inout_count != nullptr) {
        *inout_count = 0;
    }
    return SSU_OK;
}

extern "C" ssu_err_t
ssu_api_allocate(const ssu_api_allocate_req_t *req,
                 ssu_api_allocate_resp_t *out)
{
    (void)req;
    (void)out;
    return SSU_ERR_INTERNAL;
}

extern "C" ssu_err_t ssu_api_free(const char *device_path)
{
    (void)device_path;
    return SSU_ERR_INTERNAL;
}

extern "C" ssu_err_t
ssu_api_list(ssu_resource_info_t *out, uint32_t *inout_count)
{
    (void)out;
    if (inout_count != nullptr) {
        *inout_count = 0;
    }
    return SSU_OK;
}

extern "C" ssu_err_t
ssu_api_allocate_result_get(const char *request_id,
                            ssu_api_allocate_result_info_t *out)
{
    (void)request_id;
    (void)out;
    return SSU_ERR_INTERNAL;
}

extern "C" ssu_err_t
ssu_api_mount(const char *device_path, const char *host_id)
{
    (void)device_path;
    (void)host_id;
    return SSU_ERR_INTERNAL;
}

extern "C" ssu_err_t ssu_api_unmount(const char *device_path)
{
    (void)device_path;
    return SSU_ERR_INTERNAL;
}

static int expect_contains(const std::string &body,
                           const std::string &needle)
{
    if (body.find(needle) != std::string::npos) {
        return 0;
    }

    std::cerr << "missing expected text: " << needle << "\n"
              << "body was:\n" << body;
    return 1;
}

int main(void)
{
    char body[1024];
    std::string text;
    int failed = 0;

    std::memset(body, 0, sizeof(body));
    format_operation_error(body, sizeof(body), "mount", SSU_ERR_KERNEL);
    text = body;

    failed |= expect_contains(text, "mount failed: SSU_ERR_KERNEL (-6)");
    failed |= expect_contains(text, "plugin: lbc_mock");
    failed |= expect_contains(text, "/dev/ssu-ctl");
    failed |= expect_contains(text, "ssu_reqshim.ko");
    failed |= expect_contains(text, "/tmp/ubse-lbc-mock.log");

    return failed == 0 ? 0 : 1;
}
