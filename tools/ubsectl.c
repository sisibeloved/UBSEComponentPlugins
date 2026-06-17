#include "ssu_api.h"

#include <stdio.h>
#include <string.h>

static void usage(void)
{
    puts("usage: ubsectl <alloc|mount|unmount|release|query>");
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

static int cmd_query(void)
{
    ssu_query_req_t req = {
        .type = SSU_QUERY_POOL,
    };
    uint32_t count = 0;
    ssu_err_t err = ssu_resource_query(&req, NULL, sizeof(ssu_resource_info_t),
                                       &count);

    if (err != SSU_OK) {
        fprintf(stderr, "query failed: %d\n", err);
        return 1;
    }

    printf("pool entries: %u\n", count);
    return 0;
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
        return cmd_query();
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
