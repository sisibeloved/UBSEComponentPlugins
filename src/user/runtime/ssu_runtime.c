#include "ssu_controller.h"
#include "ssu_plugin.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef SSU_DEFAULT_ROLE
#define SSU_DEFAULT_ROLE "unknown"
#endif

typedef struct {
    const char *role;
    const char *config;
    int once;
} runtime_options_t;

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signum)
{
    (void)signum;
    stop_requested = 1;
}

static const char *program_label(const char *role)
{
    if (strcmp(role, "manager") == 0) {
        return "ssu-mgr";
    }

    if (strcmp(role, "agent") == 0) {
        return "ssu-agent";
    }

    return "ssu-runtime";
}

static int parse_args(int argc, char **argv, runtime_options_t *opts)
{
    int i;

    opts->role = SSU_DEFAULT_ROLE;
    opts->config = NULL;
    opts->once = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) {
            opts->once = 1;
            continue;
        }

        if (strcmp(argv[i], "--role") == 0) {
            if (i + 1 >= argc) {
                return 0;
            }
            opts->role = argv[++i];
            continue;
        }

        if (strncmp(argv[i], "--role=", 7) == 0) {
            opts->role = argv[i] + 7;
            continue;
        }

        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                return 0;
            }
            opts->config = argv[++i];
            continue;
        }

        if (strncmp(argv[i], "--config=", 9) == 0) {
            opts->config = argv[i] + 9;
            continue;
        }

        return 0;
    }

    return opts->role != NULL && opts->role[0] != '\0';
}

static int refresh_pool(void)
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
    ssu_resource_info_t resources[128];
    uint32_t count = 128;
    uint32_t i;
    ssu_err_t err = ssu_controller_query_pool(resources, &count);

    if (err != SSU_OK) {
        fprintf(stderr, "pool query failed: %d\n", err);
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

    return 0;
}

int main(int argc, char **argv)
{
    runtime_options_t opts;

    if (!parse_args(argc, argv, &opts)) {
        fprintf(stderr,
                "usage: %s [--role manager|agent] [--config PATH] [--once]\n",
                argv[0]);
        return 1;
    }

    if (opts.config != NULL && access(opts.config, R_OK) != 0) {
        fprintf(stderr, "config not readable: %s\n", opts.config);
        return 1;
    }

    if (refresh_pool() != 0) {
        return 1;
    }

    printf("%s role=%s\n", program_label(opts.role), opts.role);
    if (print_pool() != 0) {
        return 1;
    }

    if (opts.once) {
        return 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    while (!stop_requested) {
        sleep(2);
        if (!stop_requested && refresh_pool() != 0) {
            return 1;
        }
    }

    return 0;
}
