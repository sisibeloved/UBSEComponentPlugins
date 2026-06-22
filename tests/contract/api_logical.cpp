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
    const char *hosts[] = {"nodeA"};
    ssu_api_allocate_req_t req = {};
    ssu_api_allocate_resp_t resp;
    ssu_api_allocate_result_info_t result;
    ssu_query_req_t query_req = {};
    ssu_logdev_info_t logdevs[2];
    ssu_resource_info_t resources[2];
    uint32_t logdev_count = 2;
    uint32_t resource_count = 2;

    setenv("SSU_MOCK_SSU_COUNT", "2", 1);

    req.size_bytes = 8192;
    req.user_id = "user-logical";
    req.physical_disk_count = 0;
    req.logical_disk_aggregate = 0;
    req.allocation_type = SSU_SHARE_EXCLUSIVE;
    req.host_ids = hosts;
    req.host_count = 1;

    memset(resources, 0, sizeof(resources));
    if (expect_err("logical list",
                   ssu_api_list(resources, &resource_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (resource_count != 2 || strcmp(resources[0].ssu_id, "mock-ssu0") != 0 ||
        strcmp(resources[1].ssu_id, "mock-ssu1") != 0) {
        fputs("logical list returned unexpected resources\n", stderr);
        return 1;
    }

    memset(&resp, 0, sizeof(resp));
    if (expect_err("logical allocate",
                   ssu_api_allocate(&req, &resp),
                   SSU_OK) != 0) {
        return 1;
    }

    if (strcmp(resp.request_id, "alloc-0") != 0) {
        fprintf(stderr, "unexpected request id: %s\n", resp.request_id);
        return 1;
    }

    memset(&result, 0, sizeof(result));
    if (expect_err("logical allocate result",
                   ssu_api_allocate_result_get(resp.request_id, &result),
                   SSU_OK) != 0) {
        return 1;
    }

    if (result.status != SSU_OK ||
        strcmp(result.device_path, "/dev/ssu0") != 0 ||
        result.physical_disk_count != 1 ||
        strcmp(result.physical_disks[0].ssu_id, "mock-ssu0") != 0 ||
        strcmp(result.physical_disks[0].ns_id, "mock-ns0") != 0 ||
        result.physical_disks[0].logical_offset != 0 ||
        result.physical_disks[0].length != req.size_bytes ||
        result.physical_disks[0].lba != 0 ||
        result.error_message[0] != '\0') {
        fputs("logical allocate result returned unexpected default disk\n",
              stderr);
        return 1;
    }

    if (expect_err("logical mount",
                   ssu_api_mount(result.device_path, "nodeA"),
                   SSU_OK) != 0) {
        return 1;
    }

    query_req.type = SSU_QUERY_LOGDEV;
    logdev_count = 2;
    memset(logdevs, 0, sizeof(logdevs));
    if (expect_err("logical query logdev",
                   ssu_resource_query(&query_req, logdevs,
                                      sizeof(logdevs[0]), &logdev_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (logdev_count != 1 ||
        strcmp(logdevs[0].logical_dev, "/dev/ssu0") != 0 ||
        strcmp(logdevs[0].host_id, "nodeA") != 0 ||
        strcmp(logdevs[0].allocate_id, "alloc-0") != 0) {
        fputs("logical mount did not create expected mapping\n", stderr);
        return 1;
    }

    if (expect_err("logical free while mounted",
                   ssu_api_free(result.device_path),
                   SSU_ERR_BUSY) != 0) {
        return 1;
    }

    if (expect_err("logical unmount",
                   ssu_api_unmount(result.device_path),
                   SSU_OK) != 0) {
        return 1;
    }

    if (expect_err("logical free",
                   ssu_api_free(result.device_path),
                   SSU_OK) != 0) {
        return 1;
    }

    req.physical_disk_count = 2;
    memset(&resp, 0, sizeof(resp));
    if (expect_err("logical allocate two physical disks",
                   ssu_api_allocate(&req, &resp),
                   SSU_OK) != 0) {
        return 1;
    }

    if (strcmp(resp.request_id, "alloc-1") != 0) {
        fprintf(stderr, "unexpected second request id: %s\n",
                resp.request_id);
        return 1;
    }

    memset(&result, 0, sizeof(result));
    if (expect_err("logical allocate result with lbas",
                   ssu_api_allocate_result_get(resp.request_id, &result),
                   SSU_OK) != 0) {
        return 1;
    }

    if (result.status != SSU_OK ||
        strcmp(result.device_path, "/dev/ssu1") != 0 ||
        result.physical_disk_count != 2 ||
        strcmp(result.physical_disks[0].ssu_id, "mock-ssu0") != 0 ||
        strcmp(result.physical_disks[1].ssu_id, "mock-ssu1") != 0 ||
        result.physical_disks[0].logical_offset != 0 ||
        result.physical_disks[0].length != 4096 ||
        result.physical_disks[0].lba != 0 ||
        result.physical_disks[1].logical_offset != 4096 ||
        result.physical_disks[1].length != 4096 ||
        result.physical_disks[1].lba != 0) {
        fputs("logical allocate result returned unexpected LBA list\n",
              stderr);
        return 1;
    }

    if (expect_err("logical mount two physical disks",
                   ssu_api_mount(result.device_path, "nodeA"),
                   SSU_OK) != 0) {
        return 1;
    }

    logdev_count = 2;
    memset(logdevs, 0, sizeof(logdevs));
    if (expect_err("logical query two logdev entries",
                   ssu_resource_query(&query_req, logdevs,
                                      sizeof(logdevs[0]), &logdev_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (logdev_count != 2 ||
        strcmp(logdevs[0].logical_dev, "/dev/ssu1") != 0 ||
        strcmp(logdevs[1].logical_dev, "/dev/ssu1") != 0 ||
        strcmp(logdevs[0].phys_dev, "/dev/nullb0") != 0 ||
        strcmp(logdevs[1].phys_dev, "/dev/nullb1") != 0) {
        fputs("logical mount did not create two physical mappings\n",
              stderr);
        return 1;
    }

    if (expect_err("logical unmount two physical disks",
                   ssu_api_unmount(result.device_path),
                   SSU_OK) != 0) {
        return 1;
    }

    if (expect_err("logical free two physical disks",
                   ssu_api_free(result.device_path),
                   SSU_OK) != 0) {
        return 1;
    }

    memset(&result, 0, sizeof(result));
    if (expect_err("logical allocate result after free",
                   ssu_api_allocate_result_get(resp.request_id, &result),
                   SSU_ERR_NOT_FOUND) != 0) {
        return 1;
    }

    if (result.status != SSU_ERR_NOT_FOUND ||
        result.error_message[0] == '\0') {
        fputs("logical allocate result should include failure details\n",
              stderr);
        return 1;
    }

    return 0;
}
