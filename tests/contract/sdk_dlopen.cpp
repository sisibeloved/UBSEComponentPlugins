#include "ubse_ssu_sdk.h"

#include <dlfcn.h>
#include <stdio.h>

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
    if (argc != 2) {
        fputs("usage: sdk_dlopen LIBUBSE_SSU_SDK_SO\n", stderr);
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
    if (sdk == NULL || sdk->struct_size < sizeof(*sdk) ||
        sdk->init == NULL || sdk->fini == NULL ||
        sdk->allocate == NULL || sdk->free == NULL ||
        sdk->list == NULL || sdk->allocate_result_get == NULL ||
        sdk->mount == NULL || sdk->unmount == NULL) {
        fputs("ubse_ssu_sdk_entry returned incomplete ops table\n", stderr);
        dlclose(handle);
        return 1;
    }

    ubse_ssu_sdk_init_options_t opts = {};
    opts.struct_size = sizeof(opts) - 1U;
    if (expect_err("bad init options", sdk->init(&opts),
                   SSU_ERR_INVALID) != 0) {
        dlclose(handle);
        return 1;
    }

    opts.struct_size = sizeof(opts);
    opts.socket_path = "/tmp/ubse-ssu-sdk-dlopen-no-server.fifo";
    if (expect_err("explicit init", sdk->init(&opts), SSU_OK) != 0) {
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
