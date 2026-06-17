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

static int test_reliability_gate(ssu_reliability_t reliability)
{
    ssu_alloc_req_t req = {
        .size_bytes = 4096,
        .reliability = reliability,
        .share_type = SSU_SHARE_EXCLUSIVE,
        .map_dir = SSU_MAP_DIR_FORWARD,
    };
    ssu_alloc_result_t out;

    return expect_err("reliability gate",
                      ssu_resource_alloc(&req, &out, NULL, NULL),
                      SSU_ERR_UNSUPPORTED);
}

static int test_stripe_alloc_success(void)
{
    ssu_alloc_req_t req = {
        .size_bytes = 4096,
        .reliability = SSU_RELIABILITY_STRIPE,
        .share_type = SSU_SHARE_EXCLUSIVE,
        .map_dir = SSU_MAP_DIR_FORWARD,
    };
    ssu_alloc_result_t out;
    ssu_alloc_extent_t extents[1];
    uint32_t extent_count = 1;
    ssu_err_t err;

    setenv("SSU_MOCK_SSU_COUNT", "1", 1);

    err = ssu_resource_alloc(&req, &out, extents, &extent_count);

    if (expect_err("stripe alloc", err, SSU_OK) != 0) {
        return 1;
    }

    if (strcmp(out.allocate_id, "alloc-0") != 0 ||
        out.logical_size_bytes != req.size_bytes ||
        out.extent_count != 1 ||
        extent_count != 1 ||
        strcmp(extents[0].ssu_id, "mock-ssu0") != 0) {
        fprintf(stderr, "stripe alloc returned unexpected MVP-2 result\n");
        return 1;
    }

    return 0;
}

static int test_query_elem_size_gate(void)
{
    ssu_query_req_t req = {
        .type = SSU_QUERY_POOL,
    };
    uint32_t count = 0;

    return expect_err("query elem size",
                      ssu_resource_query(&req, NULL, sizeof(ssu_logdev_info_t),
                                         &count),
                      SSU_ERR_INVALID);
}

int main(void)
{
    if (test_reliability_gate(SSU_RELIABILITY_REPLICA) != 0) {
        return 1;
    }

    if (test_reliability_gate(SSU_RELIABILITY_EC) != 0) {
        return 1;
    }

    if (test_stripe_alloc_success() != 0) {
        return 1;
    }

    if (test_query_elem_size_gate() != 0) {
        return 1;
    }

    return 0;
}
