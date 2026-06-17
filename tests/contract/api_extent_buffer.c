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
    ssu_alloc_req_t req = {
        .size_bytes = 4096,
        .reliability = SSU_RELIABILITY_STRIPE,
        .share_type = SSU_SHARE_EXCLUSIVE,
        .map_dir = SSU_MAP_DIR_FORWARD,
        .tenant = "tenant-buffer",
    };
    ssu_query_req_t query_req = {
        .type = SSU_QUERY_ALLOCATION,
    };
    ssu_alloc_result_t result;
    ssu_alloc_extent_t *extents;
    uint32_t extent_count = 0;
    uint32_t allocation_count = 0;

    setenv("SSU_MOCK_SSU_COUNT", "1", 1);

    memset(&result, 0, sizeof(result));
    if (expect_err("alloc extent sizing",
                   ssu_resource_alloc(&req, &result, NULL, &extent_count),
                   SSU_ERR_BUFFER_TOO_SMALL) != 0) {
        return 1;
    }

    if (extent_count != 1) {
        fprintf(stderr, "expected one extent, got %u\n", extent_count);
        return 1;
    }

    if (expect_err("query allocation after sizing",
                   ssu_resource_query(&query_req, NULL,
                                      sizeof(ssu_allocation_info_t),
                                      &allocation_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (allocation_count != 0) {
        fputs("sizing call must not create an allocation\n", stderr);
        return 1;
    }

    extents = calloc(extent_count, sizeof(*extents));
    if (extents == NULL) {
        fputs("out of memory\n", stderr);
        return 1;
    }

    if (expect_err("alloc extent retry",
                   ssu_resource_alloc(&req, &result, extents,
                                      &extent_count),
                   SSU_OK) != 0) {
        free(extents);
        return 1;
    }

    if (strcmp(result.allocate_id, "alloc-0") != 0 ||
        result.extent_count != 1 ||
        extent_count != 1 ||
        strcmp(extents[0].ns_id, "mock-ns0") != 0 ||
        extents[0].length != req.size_bytes) {
        fputs("alloc retry returned unexpected extent\n", stderr);
        free(extents);
        return 1;
    }

    free(extents);
    if (expect_err("release allocation",
                   ssu_resource_release(result.allocate_id),
                   SSU_OK) != 0) {
        return 1;
    }

    return 0;
}
