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

static int query_logdev_count(uint32_t expected)
{
    ssu_logdev_info_t logdevs[4];
    uint32_t count = 4;

    memset(logdevs, 0, sizeof(logdevs));
    if (expect_err("query logdevs",
                   ssu_controller_query_logdevs(logdevs, &count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (count != expected) {
        fprintf(stderr, "expected %u logdevs, got %u\n", expected, count);
        return 1;
    }

    return 0;
}

static int query_allocation_state(const char *expected)
{
    ssu_allocation_info_t allocations[1];
    uint32_t count = 1;

    memset(allocations, 0, sizeof(allocations));
    if (expect_err("query allocations",
                   ssu_controller_query_allocations(allocations, &count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (count != 1 || strcmp(allocations[0].state, expected) != 0) {
        fprintf(stderr, "expected allocation state %s\n", expected);
        return 1;
    }

    return 0;
}

int main(void)
{
    const ssu_plugin_ops_t *plugin;
    ssu_alloc_req_t req = {
        .size_bytes = 8192,
        .reliability = SSU_RELIABILITY_STRIPE,
        .share_type = SSU_SHARE_SHARED,
        .map_dir = SSU_MAP_DIR_FORWARD,
        .tenant = "tenant-shared",
    };
    ssu_alloc_result_t result;
    ssu_alloc_extent_t extents[1];
    uint32_t extent_count = 1;
    ssu_mount_req_t mount_a = {
        .allocate_id = NULL,
        .host_id = "nodeA",
    };
    ssu_mount_req_t mount_b = {
        .allocate_id = NULL,
        .host_id = "nodeB",
    };

    setenv("SSU_MOCK_SSU_COUNT", "1", 1);

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
    if (expect_err("shared alloc",
                   ssu_controller_alloc(plugin, &req, &result, extents,
                                        &extent_count),
                   SSU_OK) != 0) {
        return 1;
    }

    mount_a.allocate_id = result.allocate_id;
    snprintf(mount_a.logical_dev, sizeof(mount_a.logical_dev), "/dev/ssu0");
    if (expect_err("mount nodeA",
                   ssu_controller_mount(plugin, &mount_a),
                   SSU_OK) != 0) {
        return 1;
    }

    mount_b.allocate_id = result.allocate_id;
    snprintf(mount_b.logical_dev, sizeof(mount_b.logical_dev), "/dev/ssu1");
    if (expect_err("mount nodeB shared allocation",
                   ssu_controller_mount(plugin, &mount_b),
                   SSU_OK) != 0) {
        return 1;
    }

    if (query_logdev_count(2) != 0 ||
        query_allocation_state("MOUNTED") != 0) {
        return 1;
    }

    if (expect_err("release while shared mounted",
                   ssu_controller_release(plugin, result.allocate_id),
                   SSU_ERR_BUSY) != 0) {
        return 1;
    }

    if (expect_err("unmount nodeB",
                   ssu_controller_unmount(plugin, "/dev/ssu1"),
                   SSU_OK) != 0) {
        return 1;
    }

    if (query_logdev_count(1) != 0 ||
        query_allocation_state("MOUNTED") != 0) {
        return 1;
    }

    if (expect_err("unmount nodeA",
                   ssu_controller_unmount(plugin, "/dev/ssu0"),
                   SSU_OK) != 0) {
        return 1;
    }

    if (query_logdev_count(0) != 0 ||
        query_allocation_state("ACTIVE") != 0) {
        return 1;
    }

    if (expect_err("release shared allocation",
                   ssu_controller_release(plugin, result.allocate_id),
                   SSU_OK) != 0) {
        return 1;
    }

    return 0;
}
