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

static int expect_named_pool_usage(uint64_t expected_used)
{
    ssu_resource_info_t resources[3];
    uint32_t count = 3;

    memset(resources, 0, sizeof(resources));
    if (expect_err("list named pool usage",
                   ssu_api_list(resources, &count), SSU_OK) != 0) {
        return 1;
    }

    if (count != 3 ||
        resources[0].used_bytes != expected_used ||
        resources[1].used_bytes != expected_used ||
        resources[2].used_bytes != 0) {
        fprintf(stderr,
                "unexpected pool usage: count=%u used=%llu/%llu/%llu\n",
                count,
                (unsigned long long)resources[0].used_bytes,
                (unsigned long long)resources[1].used_bytes,
                (unsigned long long)resources[2].used_bytes);
        return 1;
    }

    return 0;
}

int main(void)
{
    const char *hosts[] = {"nodeA"};
    const char *other_hosts[] = {"nodeB"};
    const char *shared_hosts[] = {"nodeA", "nodeB"};
    const char *shared_hosts_reordered[] = {"nodeB", "nodeA"};
    ssu_api_allocate_req_t req = {};
    ssu_api_allocate_resp_t first;
    ssu_api_allocate_resp_t second;
    ssu_api_allocate_result_info_t result;
    ssu_query_req_t query_req = {};
    ssu_allocation_info_t allocations[4];
    ssu_logdev_info_t logdevs[2];
    uint32_t allocation_count = 4;
    uint32_t logdev_count = 2;

    setenv("SSU_MOCK_SSU_COUNT", "3", 1);

    req.size_bytes = 8192;
    req.disk_name = "data-a";
    req.user_id = "user-named";
    req.physical_disk_count = 2;
    req.logical_disk_aggregate = 1;
    req.allocation_type = SSU_SHARE_EXCLUSIVE;
    req.host_ids = hosts;
    req.host_count = 1;

    memset(&first, 0, sizeof(first));
    if (expect_err("first named allocate",
                   ssu_api_allocate(&req, &first), SSU_OK) != 0) {
        return 1;
    }

    if (strcmp(first.request_id, "alloc-0") != 0) {
        fprintf(stderr, "unexpected named request id: %s\n",
                first.request_id);
        return 1;
    }

    memset(&result, 0, sizeof(result));
    if (expect_err("first named allocate result",
                   ssu_api_allocate_result_get(first.request_id, &result),
                   SSU_OK) != 0) {
        return 1;
    }

    if (result.status != SSU_OK ||
        strcmp(result.device_path, "/dev/ssu/data-a") != 0 ||
        result.physical_disk_count != 2 ||
        strcmp(result.physical_disks[0].ssu_id, "mock-ssu0") != 0 ||
        strcmp(result.physical_disks[1].ssu_id, "mock-ssu1") != 0 ||
        result.physical_disks[0].length != 4096 ||
        result.physical_disks[1].length != 4096) {
        fputs("named allocate result returned unexpected layout\n", stderr);
        return 1;
    }

    if (expect_named_pool_usage(4096) != 0) {
        return 1;
    }

    memset(&second, 0, sizeof(second));
    if (expect_err("second named allocate",
                   ssu_api_allocate(&req, &second), SSU_OK) != 0) {
        return 1;
    }

    if (strcmp(second.request_id, first.request_id) != 0) {
        fprintf(stderr, "idempotent allocate returned %s instead of %s\n",
                second.request_id, first.request_id);
        return 1;
    }

    query_req.type = SSU_QUERY_ALLOCATION;
    memset(allocations, 0, sizeof(allocations));
    if (expect_err("query named allocations",
                   ssu_resource_query(&query_req, allocations,
                                      sizeof(allocations[0]),
                                      &allocation_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (allocation_count != 2 ||
        strcmp(allocations[0].allocate_id, "alloc-0") != 0 ||
        strcmp(allocations[1].allocate_id, "alloc-0") != 0 ||
        strcmp(allocations[0].disk_name, "data-a") != 0 ||
        strcmp(allocations[1].disk_name, "data-a") != 0) {
        fprintf(stderr, "idempotent allocate created %u allocation rows\n",
                allocation_count);
        return 1;
    }

    if (expect_named_pool_usage(4096) != 0) {
        return 1;
    }

    if (expect_err("mount named disk",
                   ssu_api_mount(result.device_path, "nodeA"),
                   SSU_OK) != 0) {
        return 1;
    }

    query_req.type = SSU_QUERY_LOGDEV;
    memset(logdevs, 0, sizeof(logdevs));
    if (expect_err("query named logdev",
                   ssu_resource_query(&query_req, logdevs,
                                      sizeof(logdevs[0]), &logdev_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (logdev_count != 2 ||
        strcmp(logdevs[0].logical_dev, "/dev/ssu/data-a") != 0 ||
        strcmp(logdevs[1].logical_dev, "/dev/ssu/data-a") != 0 ||
        strcmp(logdevs[0].allocate_id, "alloc-0") != 0 ||
        strcmp(logdevs[1].allocate_id, "alloc-0") != 0) {
        fputs("named mount did not preserve the logical disk name\n",
              stderr);
        return 1;
    }

    if (expect_err("unmount named disk",
                   ssu_api_unmount(result.device_path), SSU_OK) != 0) {
        return 1;
    }

    req.size_bytes = 12288;
    memset(&second, 0, sizeof(second));
    if (expect_err("conflicting named allocate",
                   ssu_api_allocate(&req, &second),
                   SSU_ERR_NS_EXISTS) != 0) {
        return 1;
    }

    req.size_bytes = 8192;
    req.host_ids = other_hosts;
    if (expect_err("conflicting named host",
                   ssu_api_allocate(&req, &second),
                   SSU_ERR_NS_EXISTS) != 0) {
        return 1;
    }

    req.host_ids = hosts;
    req.disk_name = "bad name";
    if (expect_err("invalid disk name",
                   ssu_api_allocate(&req, &second),
                   SSU_ERR_INVALID) != 0) {
        return 1;
    }

    req.disk_name = "abcdefghijklmnopqrstuvwxyz12";
    if (expect_err("too long disk name",
                   ssu_api_allocate(&req, &second),
                   SSU_ERR_INVALID) != 0) {
        return 1;
    }

    if (expect_err("free named disk",
                   ssu_api_free(result.device_path), SSU_OK) != 0) {
        return 1;
    }

    req.disk_name = "shared-a";
    req.size_bytes = 4096;
    req.physical_disk_count = 1;
    req.allocation_type = SSU_SHARE_SHARED;
    req.host_ids = shared_hosts;
    req.host_count = 2;
    memset(&first, 0, sizeof(first));
    if (expect_err("first shared named allocate",
                   ssu_api_allocate(&req, &first), SSU_OK) != 0) {
        return 1;
    }

    req.host_ids = shared_hosts_reordered;
    memset(&second, 0, sizeof(second));
    if (expect_err("reordered shared named allocate",
                   ssu_api_allocate(&req, &second), SSU_OK) != 0) {
        return 1;
    }

    if (strcmp(first.request_id, "alloc-1") != 0 ||
        strcmp(second.request_id, "alloc-1") != 0) {
        fputs("shared named allocate did not preserve request id\n", stderr);
        return 1;
    }

    memset(&result, 0, sizeof(result));
    if (expect_err("shared named allocate result",
                   ssu_api_allocate_result_get("alloc-1", &result),
                   SSU_OK) != 0) {
        return 1;
    }

    if (strcmp(result.device_path, "/dev/ssu/shared-a") != 0) {
        fprintf(stderr, "unexpected shared named path: %s\n",
                result.device_path);
        return 1;
    }

    if (expect_err("free shared named disk",
                   ssu_api_free(result.device_path), SSU_OK) != 0) {
        return 1;
    }

    return 0;
}
