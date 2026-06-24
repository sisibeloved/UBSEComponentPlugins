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

static int expect_physical(const ssu_api_allocate_result_info_t *result,
                           uint32_t index,
                           const char *ssu_id,
                           uint64_t logical_offset,
                           uint64_t length,
                           uint64_t lba)
{
    if (index >= result->physical_disk_count) {
        fprintf(stderr, "missing physical disk %u\n", index);
        return 1;
    }

    if (strcmp(result->physical_disks[index].ssu_id, ssu_id) != 0 ||
        result->physical_disks[index].logical_offset != logical_offset ||
        result->physical_disks[index].length != length ||
        result->physical_disks[index].lba != lba) {
        fprintf(stderr,
                "physical %u mismatch: got %s off=%llu len=%llu lba=%llu\n",
                index,
                result->physical_disks[index].ssu_id,
                (unsigned long long)result->physical_disks[index].logical_offset,
                (unsigned long long)result->physical_disks[index].length,
                (unsigned long long)result->physical_disks[index].lba);
        return 1;
    }

    return 0;
}

int main(void)
{
    const char *hosts[] = {"nodeA"};
    ssu_api_allocate_req_t req = {};
    ssu_api_allocate_resp_t seed_resp;
    ssu_api_allocate_resp_t two_resp;
    ssu_api_allocate_resp_t three_resp;
    ssu_api_allocate_result_info_t seed_result;
    ssu_api_allocate_result_info_t two_result;
    ssu_api_allocate_result_info_t three_result;
    int failed = 0;

    setenv("SSU_MOCK_SSU_COUNT", "3", 1);

    req.size_bytes = 8192;
    req.user_id = "user-lba";
    req.physical_disk_count = 1;
    req.logical_disk_aggregate = 1;
    req.allocation_type = SSU_SHARE_EXCLUSIVE;
    req.host_ids = hosts;
    req.host_count = 1;

    memset(&seed_resp, 0, sizeof(seed_resp));
    failed |= expect_err("seed allocate", ssu_api_allocate(&req, &seed_resp),
                         SSU_OK);
    failed |= strcmp(seed_resp.request_id, "alloc-0") != 0;

    memset(&seed_result, 0, sizeof(seed_result));
    failed |= expect_err("seed result",
                         ssu_api_allocate_result_get(seed_resp.request_id,
                                                     &seed_result),
                         SSU_OK);
    failed |= strcmp(seed_result.device_path, "/dev/ssu/ssu0") != 0;
    failed |= seed_result.physical_disk_count != 1;
    failed |= expect_physical(&seed_result, 0, "mock-ssu0", 0, 8192, 0);

    req.size_bytes = 8192;
    req.physical_disk_count = 2;
    memset(&two_resp, 0, sizeof(two_resp));
    failed |= expect_err("two-disk allocate",
                         ssu_api_allocate(&req, &two_resp),
                         SSU_OK);
    failed |= strcmp(two_resp.request_id, "alloc-1") != 0;

    memset(&two_result, 0, sizeof(two_result));
    failed |= expect_err("two-disk result",
                         ssu_api_allocate_result_get(two_resp.request_id,
                                                     &two_result),
                         SSU_OK);
    failed |= strcmp(two_result.device_path, "/dev/ssu/ssu1") != 0;
    failed |= two_result.physical_disk_count != 2;
    failed |= expect_physical(&two_result, 0, "mock-ssu0", 0, 4096, 16);
    failed |= expect_physical(&two_result, 1, "mock-ssu1", 4096, 4096, 0);

    req.size_bytes = 12288;
    req.physical_disk_count = 3;
    memset(&three_resp, 0, sizeof(three_resp));
    failed |= expect_err("three-disk allocate",
                         ssu_api_allocate(&req, &three_resp),
                         SSU_OK);
    failed |= strcmp(three_resp.request_id, "alloc-2") != 0;

    memset(&three_result, 0, sizeof(three_result));
    failed |= expect_err("three-disk result",
                         ssu_api_allocate_result_get(three_resp.request_id,
                                                     &three_result),
                         SSU_OK);
    failed |= strcmp(three_result.device_path, "/dev/ssu/ssu2") != 0;
    failed |= three_result.physical_disk_count != 3;
    failed |= expect_physical(&three_result, 0, "mock-ssu0", 0, 4096, 24);
    failed |= expect_physical(&three_result, 1, "mock-ssu1", 4096, 4096, 8);
    failed |= expect_physical(&three_result, 2, "mock-ssu2", 8192, 4096, 0);

    failed |= expect_err("free three", ssu_api_free(three_result.device_path),
                         SSU_OK);
    failed |= expect_err("free two", ssu_api_free(two_result.device_path),
                         SSU_OK);
    failed |= expect_err("free seed", ssu_api_free(seed_result.device_path),
                         SSU_OK);

    return failed == 0 ? 0 : 1;
}
