#include "ubse_ssu_sdk.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef const ubse_ssu_sdk_ops_t *(*sdk_entry_fn)(void);

static int expect_ok(const char *name, ssu_err_t err)
{
    if (err != SSU_OK) {
        fprintf(stderr, "%s failed: %d\n", name, err);
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
        fprintf(stderr, "dlsym failed for %s: %s\n", name, err);
        return NULL;
    }

    return reinterpret_cast<Fn>(sym);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fputs("usage: userguide_sdk_flow LIBUBSE_SSU_SDK_SO\n", stderr);
        return 1;
    }

    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    sdk_entry_fn entry =
        load_symbol<sdk_entry_fn>(handle, "ubse_ssu_sdk_entry");
    if (entry == NULL) {
        dlclose(handle);
        return 1;
    }

    const ubse_ssu_sdk_ops_t *sdk = entry();
    if (sdk == NULL || sdk->struct_size < sizeof(*sdk)) {
        fputs("invalid sdk table\n", stderr);
        dlclose(handle);
        return 1;
    }

    if (expect_ok("sdk init", sdk->init(NULL)) != 0) {
        dlclose(handle);
        return 1;
    }

    ssu_resource_info_t resources[3];
    uint32_t resource_count = 3;
    memset(resources, 0, sizeof(resources));
    if (expect_ok("sdk list", sdk->list(resources, &resource_count)) != 0) {
        sdk->fini();
        dlclose(handle);
        return 1;
    }
    if (resource_count != 3 ||
        strcmp(resources[0].ssu_id, "lbc-mock-ssu0") != 0 ||
        strcmp(resources[1].ssu_id, "lbc-mock-ssu1") != 0 ||
        strcmp(resources[2].ssu_id, "lbc-mock-ssu2") != 0) {
        fprintf(stderr, "unexpected list result: count=%u first=%s\n",
                resource_count, resources[0].ssu_id);
        sdk->fini();
        dlclose(handle);
        return 1;
    }

    const char *hosts[] = {"local"};
    ssu_api_allocate_req_t req = {};
    req.size_bytes = 512ULL * 1024ULL * 1024ULL;
    req.disk_name = "/dev/ssu/sdk-demo-disk";
    req.user_id = "user-demo";
    req.physical_disk_count = 0;
    req.logical_disk_aggregate = 1;
    req.allocation_type = SSU_SHARE_EXCLUSIVE;
    req.host_ids = hosts;
    req.host_count = 1;

    ssu_api_allocate_resp_t resp = {};
    if (expect_ok("sdk allocate", sdk->allocate(&req, &resp)) != 0) {
        sdk->fini();
        dlclose(handle);
        return 1;
    }
    if (strcmp(resp.request_id, "alloc-0") != 0) {
        fprintf(stderr, "unexpected request id: %s\n", resp.request_id);
        sdk->fini();
        dlclose(handle);
        return 1;
    }

    ssu_api_allocate_result_info_t result = {};
    if (expect_ok("sdk allocate-result-get",
                  sdk->allocate_result_get(resp.request_id, &result)) != 0) {
        fprintf(stderr, "detail: %s\n", result.error_message);
        sdk->fini();
        dlclose(handle);
        return 1;
    }
    if (strcmp(result.device_path, "/dev/ssu/sdk-demo-disk") != 0 ||
        result.physical_disk_count != 1 ||
        strcmp(result.physical_disks[0].ssu_id, "lbc-mock-ssu0") != 0 ||
        strcmp(result.physical_disks[0].ns_id, "1") != 0 ||
        result.physical_disks[0].logical_offset != 0 ||
        result.physical_disks[0].length != req.size_bytes ||
        result.physical_disks[0].lba != 0) {
        fprintf(stderr, "unexpected allocate result: dev=%s count=%u\n",
                result.device_path, result.physical_disk_count);
        sdk->fini();
        dlclose(handle);
        return 1;
    }

    if (expect_ok("sdk mount", sdk->mount(result.device_path, "local")) != 0 ||
        expect_ok("sdk unmount", sdk->unmount(result.device_path)) != 0 ||
        expect_ok("sdk free", sdk->free(result.device_path)) != 0) {
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
