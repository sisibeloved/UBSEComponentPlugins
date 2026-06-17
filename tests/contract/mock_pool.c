#include "ssu_controller.h"
#include "ssu_plugin.h"

#include <stdint.h>
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
    ssu_resource_info_t resources[2];
    uint32_t count = 2;

    setenv("SSU_MOCK_SSU_COUNT", "2", 1);

    plugin = ssu_plugin_entry();
    if (plugin == NULL) {
        fputs("mock plugin did not return ops\n", stderr);
        return 1;
    }

    if (expect_err("refresh pool",
                   ssu_controller_refresh_pool(plugin),
                   SSU_OK) != 0) {
        return 1;
    }

    memset(resources, 0, sizeof(resources));
    if (expect_err("query pool",
                   ssu_controller_query_pool(resources, &count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (count != 2) {
        fprintf(stderr, "expected 2 resources, got %u\n", count);
        return 1;
    }

    if (strcmp(resources[0].ssu_id, "mock-ssu0") != 0 ||
        strcmp(resources[1].ssu_id, "mock-ssu1") != 0) {
        fputs("unexpected mock resource ids\n", stderr);
        return 1;
    }

    if (strcmp(resources[0].state, "ONLINE") != 0 ||
        strcmp(resources[1].state, "ONLINE") != 0) {
        fputs("mock resources should start ONLINE\n", stderr);
        return 1;
    }

    char devname[64];
    memset(devname, 0, sizeof(devname));
    if (expect_err("connect second mock ssu",
                   plugin->connect("mock-ssu1", devname, sizeof(devname)),
                   SSU_OK) != 0) {
        return 1;
    }

    if (strcmp(devname, "/dev/nullb1") != 0) {
        fprintf(stderr, "expected /dev/nullb1, got %s\n", devname);
        return 1;
    }

    setenv("SSU_MOCK_SSU_COUNT", "1", 1);
    if (expect_err("refresh pool after device loss",
                   ssu_controller_refresh_pool(plugin),
                   SSU_OK) != 0) {
        return 1;
    }

    memset(resources, 0, sizeof(resources));
    count = 2;
    if (expect_err("query pool after device loss",
                   ssu_controller_query_pool(resources, &count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (count != 2) {
        fprintf(stderr, "expected 2 managed resources after loss, got %u\n",
                count);
        return 1;
    }

    if (strcmp(resources[0].state, "ONLINE") != 0 ||
        strcmp(resources[1].state, "OFFLINE") != 0) {
        fprintf(stderr, "expected ONLINE/OFFLINE after loss, got %s/%s\n",
                resources[0].state, resources[1].state);
        return 1;
    }

    memset(resources, 0, sizeof(resources));
    count = 2;
    ssu_query_req_t req = {
        .type = SSU_QUERY_POOL,
    };
    if (expect_err("api query pool",
                   ssu_resource_query(&req, resources,
                                      sizeof(ssu_resource_info_t), &count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (count != 2 || strcmp(resources[0].ssu_id, "mock-ssu0") != 0 ||
        strcmp(resources[1].ssu_id, "mock-ssu1") != 0) {
        fputs("api query did not return controller pool view\n", stderr);
        return 1;
    }

    return 0;
}
