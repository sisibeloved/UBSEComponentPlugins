#include "ssu_api.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

typedef const ssu_api_ops_t *(*ssu_api_entry_fn)(void);

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
        fputs("usage: api_dlopen LIBSSU_API_SO\n", stderr);
        return 1;
    }

    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    ssu_api_entry_fn api_entry =
        load_symbol<ssu_api_entry_fn>(handle, "ssu_api_entry");
    if (api_entry == NULL) {
        dlclose(handle);
        return 1;
    }

    const ssu_api_ops_t *api = api_entry();
    if (api == NULL || api->struct_size < sizeof(*api) ||
        api->init == NULL || api->fini == NULL ||
        api->allocate == NULL || api->free == NULL ||
        api->list == NULL || api->allocate_result_get == NULL ||
        api->mount == NULL || api->unmount == NULL ||
        api->resource_alloc == NULL || api->resource_mount == NULL ||
        api->resource_unmount == NULL || api->resource_release == NULL ||
        api->resource_query == NULL) {
        fputs("ssu_api_entry returned incomplete ops table\n", stderr);
        dlclose(handle);
        return 1;
    }

    if (expect_err("default init", api->init(NULL), SSU_OK) != 0) {
        dlclose(handle);
        return 1;
    }

    ssu_api_init_options_t opts = {};
    opts.struct_size = sizeof(opts);
    if (expect_err("explicit init", api->init(&opts), SSU_OK) != 0) {
        dlclose(handle);
        return 1;
    }

    opts.struct_size = sizeof(opts) - 1U;
    if (expect_err("bad init options", api->init(&opts),
                   SSU_ERR_INVALID) != 0) {
        dlclose(handle);
        return 1;
    }

    ssu_resource_info_t resources[3];
    uint32_t count = 3;
    if (expect_err("list after dlopen init",
                   api->list(resources, &count),
                   SSU_OK) != 0) {
        dlclose(handle);
        return 1;
    }

    api->fini();

    if (dlclose(handle) != 0) {
        fprintf(stderr, "dlclose failed: %s\n", dlerror());
        return 1;
    }

    return 0;
}
