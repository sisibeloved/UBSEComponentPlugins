#include "../../src/kernel/reqshim/reqshim_uapi.h"
#include "../../src/user/plugin/reqshim_iface.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define MAX_CALLS 16

typedef struct {
    unsigned long request;
    struct ssu_logdev_req logdev;
    struct ssu_map_entry map;
} ioctl_call_t;

typedef struct {
    int open_errno;
    int open_count;
    int close_count;
    char opened_path[128];
    ioctl_call_t calls[MAX_CALLS];
    unsigned int call_count;
    char log[1024];
} fake_reqshim_t;

static fake_reqshim_t fake;

static int expect_err(const char *name, ssu_err_t actual, ssu_err_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
        return 1;
    }

    return 0;
}

static int fake_open(const char *path, int flags)
{
    (void)flags;
    fake.open_count++;
    snprintf(fake.opened_path, sizeof(fake.opened_path), "%s", path);

    if (fake.open_errno != 0) {
        errno = fake.open_errno;
        return -1;
    }

    return 7;
}

static int fake_ioctl(int fd, unsigned long request, void *arg)
{
    ioctl_call_t *call;

    if (fd != 7 || fake.call_count >= MAX_CALLS) {
        errno = EINVAL;
        return -1;
    }

    call = &fake.calls[fake.call_count++];
    memset(call, 0, sizeof(*call));
    call->request = request;

    if (request == SSU_IOC_GET_VERSION) {
        *(uint32_t *)arg = SSU_REQSHIM_UAPI_VERSION;
        return 0;
    }

    if (request == SSU_IOC_LOGDEV_CREATE ||
        request == SSU_IOC_LOGDEV_DESTROY) {
        call->logdev = *(struct ssu_logdev_req *)arg;
        return 0;
    }

    if (request == SSU_IOC_MAP_ADD || request == SSU_IOC_MAP_DEL) {
        call->map = *(struct ssu_map_entry *)arg;
        return 0;
    }

    errno = ENOTTY;
    return -1;
}

static int fake_close(int fd)
{
    if (fd != 7) {
        errno = EINVAL;
        return -1;
    }

    fake.close_count++;
    return 0;
}

static void fake_log(void *ctx, const char *message)
{
    (void)ctx;
    snprintf(fake.log + strlen(fake.log),
             sizeof(fake.log) - strlen(fake.log), "%s\n", message);
}

static void reset_fake(void)
{
    memset(&fake, 0, sizeof(fake));
}

static int expect_request(unsigned int index, unsigned long request)
{
    if (fake.call_count <= index || fake.calls[index].request != request) {
        fprintf(stderr, "call %u expected request %lu, got %lu count=%u\n",
                index, request,
                fake.call_count > index ? fake.calls[index].request : 0,
                fake.call_count);
        return 1;
    }

    return 0;
}

static int run_mount_builds_expected_ioctls(void)
{
    const ssu_reqshim_sys_ops_t ops = {
        fake_open,
        fake_ioctl,
        fake_close,
    };
    ssu_reqshim_map_spec_t maps[2] = {};
    int failed = 0;

    snprintf(maps[0].phys_dev, sizeof(maps[0].phys_dev), "/dev/nvme1n1");
    snprintf(maps[0].ns_id, sizeof(maps[0].ns_id), "1");
    maps[0].logical_offset = 0;
    maps[0].length = 4096;
    maps[0].phys_sector = 8;

    snprintf(maps[1].phys_dev, sizeof(maps[1].phys_dev), "/dev/nvme2n1");
    snprintf(maps[1].ns_id, sizeof(maps[1].ns_id), "2");
    maps[1].logical_offset = 4096;
    maps[1].length = 4096;
    maps[1].phys_sector = 16;

    reset_fake();
    failed |= expect_err("mount",
                         ssu_reqshim_mount_logdev("/dev/ssu3", maps, 2, 0,
                                                  fake_log, NULL, &ops,
                                                  "/tmp/ssu-ctl"),
                         SSU_OK);
    failed |= strcmp(fake.opened_path, "/tmp/ssu-ctl") != 0;
    failed |= fake.open_count != 1 || fake.close_count != 1;
    failed |= fake.call_count != 4;
    failed |= expect_request(0, SSU_IOC_GET_VERSION);
    failed |= expect_request(1, SSU_IOC_LOGDEV_CREATE);
    failed |= expect_request(2, SSU_IOC_MAP_ADD);
    failed |= expect_request(3, SSU_IOC_MAP_ADD);

    if (fake.calls[1].logdev.minor != 3 ||
        fake.calls[1].logdev.capacity_sectors != 16 ||
        fake.calls[1].logdev.logical_block_size != 512 ||
        fake.calls[2].map.logical_minor != 3 ||
        fake.calls[2].map.logical_sector != 0 ||
        fake.calls[2].map.length_sectors != 8 ||
        strcmp(fake.calls[2].map.phys_dev, "/dev/nvme1n1") != 0 ||
        fake.calls[2].map.nsid != 1 ||
        fake.calls[2].map.phys_sector != 8 ||
        fake.calls[3].map.logical_sector != 8 ||
        fake.calls[3].map.nsid != 2 ||
        fake.calls[3].map.phys_sector != 16) {
        fputs("mount built unexpected ReqShim ioctl payloads\n", stderr);
        failed = 1;
    }

    return failed;
}

static int run_unmount_builds_expected_ioctls(void)
{
    const ssu_reqshim_sys_ops_t ops = {
        fake_open,
        fake_ioctl,
        fake_close,
    };
    ssu_reqshim_map_spec_t maps[1] = {};
    int failed = 0;

    snprintf(maps[0].phys_dev, sizeof(maps[0].phys_dev), "/dev/nvme1n1");
    snprintf(maps[0].ns_id, sizeof(maps[0].ns_id), "1");
    maps[0].logical_offset = 0;
    maps[0].length = 4096;

    reset_fake();
    failed |= expect_err("unmount",
                         ssu_reqshim_unmount_logdev("ssu3", maps, 1, 0,
                                                    fake_log, NULL, &ops,
                                                    "/tmp/ssu-ctl"),
                         SSU_OK);
    failed |= fake.open_count != 1 || fake.close_count != 1;
    failed |= fake.call_count != 3;
    failed |= expect_request(0, SSU_IOC_GET_VERSION);
    failed |= expect_request(1, SSU_IOC_MAP_DEL);
    failed |= expect_request(2, SSU_IOC_LOGDEV_DESTROY);

    if (fake.calls[1].map.logical_minor != 3 ||
        fake.calls[1].map.logical_sector != 0 ||
        fake.calls[2].logdev.minor != 3) {
        fputs("unmount built unexpected ReqShim ioctl payloads\n", stderr);
        failed = 1;
    }

    return failed;
}

static int run_missing_ctl_policy(void)
{
    const ssu_reqshim_sys_ops_t ops = {
        fake_open,
        fake_ioctl,
        fake_close,
    };
    ssu_reqshim_map_spec_t map = {};
    int failed = 0;

    snprintf(map.phys_dev, sizeof(map.phys_dev), "/dev/nvme1n1");
    snprintf(map.ns_id, sizeof(map.ns_id), "1");
    map.length = 4096;

    reset_fake();
    fake.open_errno = ENOENT;
    failed |= expect_err("optional missing ctl",
                         ssu_reqshim_mount_logdev("/dev/ssu0", &map, 1, 1,
                                                  fake_log, NULL, &ops,
                                                  "/missing/ssu-ctl"),
                         SSU_OK);
    if (fake.call_count != 0 || strstr(fake.log, "skip ioctl") == NULL) {
        fputs("optional missing ctl should skip ioctl with a log\n", stderr);
        failed = 1;
    }

    reset_fake();
    fake.open_errno = ENOENT;
    failed |= expect_err("strict missing ctl",
                         ssu_reqshim_mount_logdev("/dev/ssu0", &map, 1, 0,
                                                  fake_log, NULL, &ops,
                                                  "/missing/ssu-ctl"),
                         SSU_ERR_KERNEL);

    return failed;
}

static int run_minor_parser(void)
{
    uint32_t minor = 99;
    int failed = 0;

    failed |= expect_err("parse /dev/ssu12",
                         ssu_reqshim_parse_logical_minor("/dev/ssu12",
                                                         &minor),
                         SSU_OK);
    failed |= minor != 12;
    failed |= expect_err("parse bad",
                         ssu_reqshim_parse_logical_minor("/dev/nvme0n1",
                                                         &minor),
                         SSU_ERR_INVALID);

    return failed;
}

int main(void)
{
    int failed = 0;

    failed |= run_minor_parser();
    failed |= run_mount_builds_expected_ioctls();
    failed |= run_unmount_builds_expected_ioctls();
    failed |= run_missing_ctl_policy();

    return failed == 0 ? 0 : 1;
}
