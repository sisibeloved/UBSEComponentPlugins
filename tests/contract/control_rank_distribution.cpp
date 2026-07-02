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
    ssu_alloc_result_t results[4];
    ssu_alloc_extent_t extents[4];
    ssu_allocation_info_t allocations[4];
    const char *hosts[] = {"rank-host"};
    uint32_t allocation_count = 4;

    setenv("SSU_MOCK_SSU_COUNT", "4", 1);

    plugin = ssu_plugin_entry();
    if (plugin == NULL) {
        fputs("mock plugin did not return ops\n", stderr);
        return 1;
    }

    if (expect_err("refresh pool", ssu_controller_refresh_pool(plugin),
                   SSU_OK) != 0) {
        return 1;
    }

    req.size_bytes = 1ULL << 30;
    req.physical_disk_count = 1;
    req.reliability = SSU_RELIABILITY_STRIPE;
    req.share_type = SSU_SHARE_EXCLUSIVE;
    req.map_dir = SSU_MAP_DIR_FORWARD;
    req.tenant = "tenant-rank";
    req.host_ids = hosts;
    req.host_count = 1;

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t extent_count = 1;

        memset(&results[i], 0, sizeof(results[i]));
        memset(&extents[i], 0, sizeof(extents[i]));
        if (expect_err("rank allocate",
                       ssu_controller_alloc(plugin, &req, &results[i],
                                            &extents[i], &extent_count),
                       SSU_OK) != 0) {
            return 1;
        }

        if (extent_count != 1 || results[i].extent_count != 1 ||
            extents[i].length != req.size_bytes) {
            fputs("rank allocation returned unexpected extent shape\n",
                  stderr);
            return 1;
        }

        char expected_ssu[32];
        snprintf(expected_ssu, sizeof(expected_ssu), "mock-ssu%u", i);
        if (strcmp(extents[i].ssu_id, expected_ssu) != 0) {
            fprintf(stderr, "rank allocation %u used %s, expected %s\n",
                    i, extents[i].ssu_id, expected_ssu);
            return 1;
        }
    }

    memset(allocations, 0, sizeof(allocations));
    if (expect_err("query allocations",
                   ssu_controller_query_allocations(allocations,
                                                    &allocation_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (allocation_count != 4) {
        fprintf(stderr, "expected 4 active rank allocations, got %u\n",
                allocation_count);
        return 1;
    }

    for (uint32_t i = 0; i < 4; i++) {
        if (expect_err("release rank",
                       ssu_controller_release(plugin, results[i].allocate_id),
                       SSU_OK) != 0) {
            return 1;
        }
    }

    return 0;
}
