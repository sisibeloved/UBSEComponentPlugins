#include "ssu_controller.h"
#include "ssu_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    ssu_alloc_extent_t extents[2];
    ssu_allocation_info_t allocations[2];
    ssu_mount_req_t mount_req = {};
    ssu_logdev_info_t logdevs[2];
    uint32_t extent_count = 2;
    uint32_t allocation_count = 2;
    uint32_t logdev_count = 2;

    setenv("SSU_MOCK_SSU_COUNT", "2", 1);

    plugin = ssu_plugin_entry();
    if (plugin == NULL) {
        fputs("mock plugin did not return ops\n", stderr);
        return 1;
    }

    req.size_bytes = 65536;
    req.reliability = SSU_RELIABILITY_STRIPE;
    req.share_type = SSU_SHARE_EXCLUSIVE;
    req.map_dir = SSU_MAP_DIR_FORWARD;
    req.tenant = "tenant-multiphys";

    if (expect_err("refresh pool", ssu_controller_refresh_pool(plugin),
                   SSU_OK) != 0) {
        return 1;
    }

    memset(&result, 0, sizeof(result));
    memset(extents, 0, sizeof(extents));
    if (expect_err("alloc two extents",
                   ssu_controller_alloc(plugin, &req, &result, extents,
                                        &extent_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (strcmp(result.allocate_id, "alloc-0") != 0 ||
        result.extent_count != 2 || extent_count != 2 ||
        result.logical_size_bytes != req.size_bytes) {
        fputs("multi-physical alloc returned unexpected header\n", stderr);
        return 1;
    }

    if (strcmp(extents[0].ssu_id, "mock-ssu0") != 0 ||
        strcmp(extents[1].ssu_id, "mock-ssu1") != 0 ||
        extents[0].logical_offset != 0 ||
        extents[0].length != 32768 ||
        extents[1].logical_offset != 32768 ||
        extents[1].length != 32768) {
        fputs("multi-physical alloc returned unexpected extents\n", stderr);
        return 1;
    }

    memset(allocations, 0, sizeof(allocations));
    if (expect_err("query allocation rows",
                   ssu_controller_query_allocations(allocations,
                                                    &allocation_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (allocation_count != 2 ||
        strcmp(allocations[0].allocate_id, "alloc-0") != 0 ||
        strcmp(allocations[1].allocate_id, "alloc-0") != 0 ||
        strcmp(allocations[0].ssu_id, "mock-ssu0") != 0 ||
        strcmp(allocations[1].ssu_id, "mock-ssu1") != 0 ||
        allocations[1].logical_offset != 32768) {
        fputs("allocation query did not expose both physical extents\n",
              stderr);
        return 1;
    }

    mount_req.allocate_id = result.allocate_id;
    mount_req.host_id = "local";
    snprintf(mount_req.logical_dev, sizeof(mount_req.logical_dev),
             "/dev/ssu0");

    if (expect_err("mount two extents",
                   ssu_controller_mount(plugin, &mount_req),
                   SSU_OK) != 0) {
        return 1;
    }

    memset(logdevs, 0, sizeof(logdevs));
    if (expect_err("query logdev rows",
                   ssu_controller_query_logdevs(logdevs, &logdev_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (logdev_count != 2 ||
        strcmp(logdevs[0].logical_dev, "/dev/ssu0") != 0 ||
        strcmp(logdevs[1].logical_dev, "/dev/ssu0") != 0 ||
        strcmp(logdevs[0].phys_dev, "/dev/nullb0") != 0 ||
        strcmp(logdevs[1].phys_dev, "/dev/nullb1") != 0 ||
        logdevs[0].logical_offset != 0 ||
        logdevs[1].logical_offset != 32768) {
        fputs("logdev query did not expose both physical mappings\n",
              stderr);
        return 1;
    }

    if (expect_err("release mounted allocation",
                   ssu_controller_release(plugin, result.allocate_id),
                   SSU_ERR_BUSY) != 0) {
        return 1;
    }

    if (expect_err("unmount", ssu_controller_unmount(plugin, "/dev/ssu0"),
                   SSU_OK) != 0) {
        return 1;
    }

    if (expect_err("release", ssu_controller_release(plugin,
                                                     result.allocate_id),
                   SSU_OK) != 0) {
        return 1;
    }

    allocation_count = 2;
    memset(allocations, 0, sizeof(allocations));
    if (expect_err("query allocations after release",
                   ssu_controller_query_allocations(allocations,
                                                    &allocation_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (allocation_count != 0) {
        fputs("release should remove all allocation extents\n", stderr);
        return 1;
    }

    return 0;
}
