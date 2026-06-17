#include "ssu_api.h"
#include "ssu_controller.h"
#include "ssu_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    puts("usage: ubsectl <alloc|mount|unmount|release|query>");
    puts("       ubsectl query --type pool");
}

static int cmd_alloc(void)
{
    ssu_alloc_req_t req = {
        .size_bytes = 0,
        .reliability = SSU_RELIABILITY_STRIPE,
        .share_type = SSU_SHARE_EXCLUSIVE,
        .map_dir = SSU_MAP_DIR_FORWARD,
    };
    ssu_alloc_result_t out;
    ssu_err_t err = ssu_resource_alloc(&req, &out, NULL, NULL);

    if (err != SSU_OK) {
        fprintf(stderr, "alloc failed: %d\n", err);
        return 1;
    }

    printf("%s\n", out.allocate_id);
    return 0;
}

static int query_type_from_string(const char *s, ssu_query_type_t *out)
{
    if (s == NULL || out == NULL) {
        return 0;
    }

    if (strcmp(s, "pool") == 0) {
        *out = SSU_QUERY_POOL;
        return 1;
    }

    if (strcmp(s, "allocation") == 0) {
        *out = SSU_QUERY_ALLOCATION;
        return 1;
    }

    if (strcmp(s, "logdev") == 0) {
        *out = SSU_QUERY_LOGDEV;
        return 1;
    }

    return 0;
}

static int parse_query_args(int argc, char **argv, ssu_query_type_t *type)
{
    int i;

    *type = SSU_QUERY_POOL;
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--type") != 0) {
            return 0;
        }

        if (i + 1 >= argc || !query_type_from_string(argv[i + 1], type)) {
            return 0;
        }

        i++;
    }

    return 1;
}

static int refresh_mock_pool(void)
{
    const ssu_plugin_ops_t *plugin = ssu_plugin_entry();
    ssu_err_t err;

    if (plugin == NULL) {
        fputs("mock plugin unavailable\n", stderr);
        return 1;
    }

    err = ssu_controller_refresh_pool(plugin);
    if (err != SSU_OK) {
        fprintf(stderr, "pool refresh failed: %d\n", err);
        return 1;
    }

    return 0;
}

static int print_pool(void)
{
    ssu_query_req_t req = {
        .type = SSU_QUERY_POOL,
    };
    uint32_t count = 0;
    ssu_resource_info_t *resources;
    uint32_t i;
    ssu_err_t err = ssu_resource_query(&req, NULL, sizeof(ssu_resource_info_t),
                                       &count);

    if (err != SSU_OK && err != SSU_ERR_BUFFER_TOO_SMALL) {
        fprintf(stderr, "query failed: %d\n", err);
        return 1;
    }

    if (count == 0) {
        puts("pool entries: 0");
        return 0;
    }

    resources = calloc(count, sizeof(*resources));
    if (resources == NULL) {
        fputs("query failed: out of memory\n", stderr);
        return 1;
    }

    err = ssu_resource_query(&req, resources, sizeof(*resources), &count);
    if (err != SSU_OK) {
        free(resources);
        fprintf(stderr, "query failed: %d\n", err);
        return 1;
    }

    printf("pool entries: %u\n", count);
    for (i = 0; i < count; i++) {
        printf("%s %s %s %llu/%llu\n",
               resources[i].ssu_id,
               resources[i].host_id,
               resources[i].state,
               (unsigned long long)resources[i].used_bytes,
               (unsigned long long)resources[i].total_bytes);
    }

    free(resources);
    return 0;
}

static int cmd_query(int argc, char **argv)
{
    ssu_query_type_t type;

    if (!parse_query_args(argc, argv, &type)) {
        usage();
        return 1;
    }

    if (type != SSU_QUERY_POOL) {
        fputs("only pool query is implemented in MVP-1\n", stderr);
        return 1;
    }

    if (refresh_mock_pool() != 0) {
        return 1;
    }

    return print_pool();
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "alloc") == 0) {
        return cmd_alloc();
    }

    if (strcmp(argv[1], "query") == 0) {
        return cmd_query(argc, argv);
    }

    if (strcmp(argv[1], "mount") == 0 ||
        strcmp(argv[1], "unmount") == 0 ||
        strcmp(argv[1], "release") == 0) {
        puts("command skeleton only");
        return 0;
    }

    usage();
    return 1;
}
