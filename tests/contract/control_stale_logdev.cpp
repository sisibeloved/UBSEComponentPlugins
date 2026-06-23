#include "ssu_controller.h"
#include "ssu_plugin.h"

#include <stdio.h>
#include <string.h>

static int unmount_calls;
static char unmounted_dev[64];

static int expect_err(const char *name, ssu_err_t actual, ssu_err_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
        return 1;
    }

    return 0;
}

static const char *fake_name(void)
{
    return "stale-logdev-fake";
}

static ssu_err_t fake_unmount(const char *logical_dev)
{
    unmount_calls++;
    snprintf(unmounted_dev, sizeof(unmounted_dev), "%s", logical_dev);
    return SSU_OK;
}

static const ssu_plugin_ops_t fake_plugin = {
    fake_name,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    fake_unmount,
};

int main(void)
{
    if (expect_err("stale unmount",
                   ssu_controller_unmount(&fake_plugin, "/dev/ssu0"),
                   SSU_OK) != 0) {
        return 1;
    }

    if (unmount_calls != 1 || strcmp(unmounted_dev, "/dev/ssu0") != 0) {
        fputs("controller did not delegate stale logdev cleanup to plugin\n",
              stderr);
        return 1;
    }

    return 0;
}
