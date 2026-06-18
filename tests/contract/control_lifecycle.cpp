#include "ssu_controller.h"
#include "ssu_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ssu_err_t ssu_controller_alloc(const ssu_plugin_ops_t *plugin,
                                      const ssu_alloc_req_t *req,
                                      ssu_alloc_result_t *out,
                                      ssu_alloc_extent_t *out_extents,
                                      uint32_t *inout_extent_count);
extern ssu_err_t ssu_controller_release(const ssu_plugin_ops_t *plugin,
                                        const char *allocate_id);
extern ssu_err_t ssu_controller_query_allocations(ssu_allocation_info_t *out,
                                                  uint32_t *inout_count);
extern ssu_err_t ssu_controller_mount(const ssu_plugin_ops_t *plugin,
                                      const ssu_mount_req_t *req);
extern ssu_err_t ssu_controller_unmount(const ssu_plugin_ops_t *plugin,
                                        const char *logical_dev);
extern ssu_err_t ssu_controller_query_logdevs(ssu_logdev_info_t *out,
                                              uint32_t *inout_count);

static int expect_err(const char *name, ssu_err_t actual, ssu_err_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
        return 1;
    }

    return 0;
}

int main(void)
{
    const ssu_plugin_ops_t *plugin;
    ssu_alloc_req_t req = {};
    ssu_alloc_result_t result;
    ssu_alloc_extent_t extents[1];
    uint32_t extent_count = 1;
    ssu_allocation_info_t allocations[1];
    uint32_t allocation_count = 1;
    ssu_mount_req_t mount_req = {};
    ssu_logdev_info_t logdevs[1];
    uint32_t logdev_count = 1;

    setenv("SSU_MOCK_SSU_COUNT", "1", 1);

    req.size_bytes = 8192;
    req.reliability = SSU_RELIABILITY_STRIPE;
    req.share_type = SSU_SHARE_EXCLUSIVE;
    req.map_dir = SSU_MAP_DIR_FORWARD;
    req.tenant = "tenant-a";
    mount_req.allocate_id = "alloc-0";
    mount_req.host_id = "local";

    plugin = ssu_plugin_entry();
    if (plugin == NULL) {
        fputs("mock plugin did not return ops\n", stderr);
        return 1;
    }

    if (expect_err("refresh pool", ssu_controller_refresh_pool(plugin),
                   SSU_OK) != 0) {
        return 1;
    }

    memset(&result, 0, sizeof(result));
    memset(extents, 0, sizeof(extents));
    if (expect_err("alloc", ssu_controller_alloc(plugin, &req, &result,
                                                 extents, &extent_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (strcmp(result.allocate_id, "alloc-0") != 0 ||
        result.logical_size_bytes != req.size_bytes ||
        result.extent_count != 1 || extent_count != 1) {
        fputs("alloc returned unexpected result header\n", stderr);
        return 1;
    }

    if (strcmp(extents[0].ssu_id, "mock-ssu0") != 0 ||
        strcmp(extents[0].host_id, "mock-host0") != 0 ||
        strcmp(extents[0].ns_id, "mock-ns0") != 0 ||
        extents[0].logical_offset != 0 ||
        extents[0].length != req.size_bytes) {
        fputs("alloc returned unexpected extent\n", stderr);
        return 1;
    }

    memset(allocations, 0, sizeof(allocations));
    if (expect_err("query allocations",
                   ssu_controller_query_allocations(allocations,
                                                    &allocation_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (allocation_count != 1 ||
        strcmp(allocations[0].allocate_id, "alloc-0") != 0 ||
        strcmp(allocations[0].state, "ACTIVE") != 0 ||
        allocations[0].length != req.size_bytes) {
        fputs("query allocations returned unexpected active allocation\n",
              stderr);
        return 1;
    }

    snprintf(mount_req.logical_dev, sizeof(mount_req.logical_dev),
             "/dev/ssu0");
    if (expect_err("mount", ssu_controller_mount(plugin, &mount_req),
                   SSU_OK) != 0) {
        return 1;
    }

    memset(logdevs, 0, sizeof(logdevs));
    if (expect_err("query logdevs",
                   ssu_controller_query_logdevs(logdevs, &logdev_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (logdev_count != 1 ||
        strcmp(logdevs[0].logical_dev, "/dev/ssu0") != 0 ||
        strcmp(logdevs[0].allocate_id, "alloc-0") != 0 ||
        strcmp(logdevs[0].phys_dev, "/dev/nullb0") != 0 ||
        strcmp(logdevs[0].ns_id, "mock-ns0") != 0 ||
        logdevs[0].length != req.size_bytes) {
        fputs("query logdevs returned unexpected mapping\n", stderr);
        return 1;
    }

    allocation_count = 1;
    memset(allocations, 0, sizeof(allocations));
    if (expect_err("query allocations after mount",
                   ssu_controller_query_allocations(allocations,
                                                    &allocation_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (allocation_count != 1 ||
        strcmp(allocations[0].state, "MOUNTED") != 0) {
        fputs("mount should mark allocation MOUNTED\n", stderr);
        return 1;
    }

    if (expect_err("release mounted allocation",
                   ssu_controller_release(plugin, "alloc-0"),
                   SSU_ERR_BUSY) != 0) {
        return 1;
    }

    if (expect_err("unmount", ssu_controller_unmount(plugin, "/dev/ssu0"),
                   SSU_OK) != 0) {
        return 1;
    }

    logdev_count = 1;
    memset(logdevs, 0, sizeof(logdevs));
    if (expect_err("query logdevs after unmount",
                   ssu_controller_query_logdevs(logdevs, &logdev_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (logdev_count != 0) {
        fputs("unmount should remove logdev mapping\n", stderr);
        return 1;
    }

    allocation_count = 1;
    memset(allocations, 0, sizeof(allocations));
    if (expect_err("query allocations after unmount",
                   ssu_controller_query_allocations(allocations,
                                                    &allocation_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (allocation_count != 1 ||
        strcmp(allocations[0].state, "ACTIVE") != 0) {
        fputs("unmount should keep allocation ACTIVE\n", stderr);
        return 1;
    }

    if (expect_err("release", ssu_controller_release(plugin, "alloc-0"),
                   SSU_OK) != 0) {
        return 1;
    }

    allocation_count = 1;
    memset(allocations, 0, sizeof(allocations));
    if (expect_err("query allocations after release",
                   ssu_controller_query_allocations(allocations,
                                                    &allocation_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (allocation_count != 0) {
        fputs("release should remove allocation from active view\n", stderr);
        return 1;
    }

    if (expect_err("release idempotent",
                   ssu_controller_release(plugin, "alloc-0"), SSU_OK) != 0) {
        return 1;
    }

    return 0;
}
