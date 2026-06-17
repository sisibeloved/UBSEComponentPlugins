#include "ssu_dataplane.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    char path[] = "/tmp/ssu-dataplane-XXXXXX";
    int fd;
    ssu_logdev_info_t logdev = {
        .logical_offset = 0,
        .length = 4096,
        .phys_sector = 4,
    };
    unsigned char pattern[512];
    unsigned char readback[512];
    unsigned char direct[512];
    uint32_t i;

    fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(fd);

    snprintf(logdev.logical_dev, sizeof(logdev.logical_dev), "/dev/ssu0");
    snprintf(logdev.phys_dev, sizeof(logdev.phys_dev), "%s", path);
    snprintf(logdev.allocate_id, sizeof(logdev.allocate_id), "alloc-0");
    snprintf(logdev.ns_id, sizeof(logdev.ns_id), "mock-ns0");

    for (i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (unsigned char)(i ^ 0x5aU);
    }
    memset(readback, 0, sizeof(readback));
    memset(direct, 0, sizeof(direct));

    if (expect_err("write mapped data",
                   ssu_dataplane_write(&logdev, 512, pattern,
                                       sizeof(pattern)),
                   SSU_OK) != 0) {
        unlink(path);
        return 1;
    }

    if (expect_err("read mapped data",
                   ssu_dataplane_read(&logdev, 512, readback,
                                      sizeof(readback)),
                   SSU_OK) != 0) {
        unlink(path);
        return 1;
    }

    if (memcmp(pattern, readback, sizeof(pattern)) != 0) {
        fputs("readback did not match written pattern\n", stderr);
        unlink(path);
        return 1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        unlink(path);
        return 1;
    }

    if (pread(fd, direct, sizeof(direct), 4 * 512 + 512) !=
        (ssize_t)sizeof(direct)) {
        perror("pread direct");
        close(fd);
        unlink(path);
        return 1;
    }
    close(fd);

    if (memcmp(pattern, direct, sizeof(pattern)) != 0) {
        fputs("physical backend was not written at translated offset\n",
              stderr);
        unlink(path);
        return 1;
    }

    if (expect_err("write beyond logdev",
                   ssu_dataplane_write(&logdev, 4096, pattern, 1),
                   SSU_ERR_INVALID) != 0) {
        unlink(path);
        return 1;
    }

    unlink(path);
    return 0;
}
