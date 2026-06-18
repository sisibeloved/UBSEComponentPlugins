#include "ssu_api.h"

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
    ssu_alloc_req_t alloc_req = {};
    ssu_alloc_result_t alloc_result;
    ssu_alloc_extent_t extents[1];
    uint32_t extent_count = 1;
    ssu_mount_req_t mount_req = {};
    ssu_query_req_t query_req = {};
    ssu_logdev_info_t logdevs[1];
    uint32_t logdev_count = 1;

    setenv("SSU_MOCK_SSU_COUNT", "1", 1);

    alloc_req.size_bytes = 8192;
    alloc_req.reliability = SSU_RELIABILITY_STRIPE;
    alloc_req.share_type = SSU_SHARE_EXCLUSIVE;
    alloc_req.map_dir = SSU_MAP_DIR_FORWARD;
    alloc_req.tenant = "tenant-api";
    mount_req.host_id = "local";
    query_req.type = SSU_QUERY_LOGDEV;

    memset(&alloc_result, 0, sizeof(alloc_result));
    memset(extents, 0, sizeof(extents));
    if (expect_err("api alloc",
                   ssu_resource_alloc(&alloc_req, &alloc_result, extents,
                                      &extent_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (strcmp(alloc_result.allocate_id, "alloc-0") != 0 ||
        extent_count != 1 ||
        strcmp(extents[0].ns_id, "mock-ns0") != 0) {
        fputs("api alloc returned unexpected allocation\n", stderr);
        return 1;
    }

    mount_req.allocate_id = alloc_result.allocate_id;
    snprintf(mount_req.logical_dev, sizeof(mount_req.logical_dev),
             "/dev/ssu0");
    if (expect_err("api mount", ssu_resource_mount(&mount_req),
                   SSU_OK) != 0) {
        return 1;
    }

    memset(logdevs, 0, sizeof(logdevs));
    if (expect_err("api query logdev",
                   ssu_resource_query(&query_req, logdevs,
                                      sizeof(logdevs[0]), &logdev_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (logdev_count != 1 ||
        strcmp(logdevs[0].logical_dev, "/dev/ssu0") != 0 ||
        strcmp(logdevs[0].allocate_id, "alloc-0") != 0 ||
        strcmp(logdevs[0].phys_dev, "/dev/nullb0") != 0) {
        fputs("api query logdev returned unexpected mapping\n", stderr);
        return 1;
    }

    if (expect_err("api release while mounted",
                   ssu_resource_release(alloc_result.allocate_id),
                   SSU_ERR_BUSY) != 0) {
        return 1;
    }

    if (expect_err("api unmount", ssu_resource_unmount("/dev/ssu0"),
                   SSU_OK) != 0) {
        return 1;
    }

    if (expect_err("api release",
                   ssu_resource_release(alloc_result.allocate_id),
                   SSU_OK) != 0) {
        return 1;
    }

    logdev_count = 1;
    memset(logdevs, 0, sizeof(logdevs));
    if (expect_err("api query logdev after release",
                   ssu_resource_query(&query_req, logdevs,
                                      sizeof(logdevs[0]), &logdev_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (logdev_count != 0) {
        fputs("release should leave no mounted logdev\n", stderr);
        return 1;
    }

    return 0;
}
