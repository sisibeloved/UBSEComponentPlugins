#include "ubse_ssu_sdk.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef const ubse_ssu_sdk_ops_t *(*sdk_entry_fn)(void);

static int expect_err(const char *name, ssu_err_t actual, ssu_err_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
        return 1;
    }

    return 0;
}

template <typename Fn>
static Fn load_symbol(void *handle, const char *name)
{
    dlerror();
    void *sym = dlsym(handle, name);
    const char *err = dlerror();

    if (err != NULL) {
        fprintf(stderr, "missing symbol %s: %s\n", name, err);
        return NULL;
    }

    return reinterpret_cast<Fn>(sym);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fputs("usage: sdk_flow LIBUBSE_SSU_SDK_SO MANAGER_SOCKET\n", stderr);
        return 1;
    }

    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    sdk_entry_fn sdk_entry =
        load_symbol<sdk_entry_fn>(handle, "ubse_ssu_sdk_entry");
    if (sdk_entry == NULL) {
        dlclose(handle);
        return 1;
    }

    const ubse_ssu_sdk_ops_t *sdk = sdk_entry();
    if (sdk == NULL) {
        fputs("sdk entry returned NULL\n", stderr);
        dlclose(handle);
        return 1;
    }

    ubse_ssu_sdk_init_options_t opts = {};
    opts.struct_size = sizeof(opts);
    opts.socket_path = argv[2];
    if (expect_err("sdk init", sdk->init(&opts), SSU_OK) != 0) {
        dlclose(handle);
        return 1;
    }

    const char *hosts[] = {"local"};
    ssu_api_allocate_req_t req = {};
    req.size_bytes = 8192;
    req.disk_name = "sdk-disk";
    req.user_id = "user-sdk";
    req.physical_disk_count = 2;
    req.logical_disk_aggregate = 1;
    req.allocation_type = SSU_SHARE_EXCLUSIVE;
    req.host_ids = hosts;
    req.host_count = 1;

    ssu_api_allocate_resp_t alloc = {};
    if (expect_err("sdk allocate", sdk->allocate(&req, &alloc),
                   SSU_OK) != 0) {
        sdk->fini();
        dlclose(handle);
        return 1;
    }
    if (strcmp(alloc.request_id, "alloc-0") != 0) {
        fprintf(stderr, "unexpected request id: %s\n", alloc.request_id);
        sdk->fini();
        dlclose(handle);
        return 1;
    }

    ssu_api_allocate_result_info_t result = {};
    if (expect_err("sdk allocate-result-get",
                   sdk->allocate_result_get(alloc.request_id, &result),
                   SSU_OK) != 0) {
        sdk->fini();
        dlclose(handle);
        return 1;
    }
    if (strcmp(result.device_path, "/dev/ssu/sdk-disk") != 0 ||
        result.physical_disk_count != 2) {
        fprintf(stderr, "unexpected result path/count: %s %u\n",
                result.device_path, result.physical_disk_count);
        sdk->fini();
        dlclose(handle);
        return 1;
    }

    ssu_resource_info_t resources[3];
    uint32_t resource_count = 3;
    memset(resources, 0, sizeof(resources));
    if (expect_err("sdk list", sdk->list(resources, &resource_count),
                   SSU_OK) != 0) {
        sdk->fini();
        dlclose(handle);
        return 1;
    }
    if (resource_count != 3 ||
        strcmp(resources[0].ssu_id, "lbc-mock-ssu0") != 0 ||
        resources[0].used_bytes != 4096 ||
        resources[1].used_bytes != 4096) {
        fprintf(stderr, "unexpected list result: count=%u first=%s\n",
                resource_count, resources[0].ssu_id);
        sdk->fini();
        dlclose(handle);
        return 1;
    }

    if (expect_err("sdk mount", sdk->mount(result.device_path, "local"),
                   SSU_OK) != 0 ||
        expect_err("sdk unmount", sdk->unmount(result.device_path),
                   SSU_OK) != 0 ||
        expect_err("sdk free", sdk->free(result.device_path),
                   SSU_OK) != 0) {
        sdk->fini();
        dlclose(handle);
        return 1;
    }

    sdk->fini();
    if (dlclose(handle) != 0) {
        fprintf(stderr, "dlclose failed: %s\n", dlerror());
        return 1;
    }

    return 0;
}
