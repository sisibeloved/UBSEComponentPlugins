#include "ssu_dataplane.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define SSU_DATAPLANE_SECTOR_SIZE 512ULL

static int is_empty_string(const char *s)
{
    return s == NULL || s[0] == '\0';
}

static ssu_err_t translate_offset(const ssu_logdev_info_t *logdev,
                                  uint64_t logical_offset,
                                  size_t len,
                                  off_t *out_offset)
{
    uint64_t relative_offset;
    uint64_t phys_base;
    uint64_t phys_offset;
    uint64_t last_offset;

    if (logdev == NULL || out_offset == NULL ||
        is_empty_string(logdev->phys_dev)) {
        return SSU_ERR_INVALID;
    }

    if (logical_offset < logdev->logical_offset) {
        return SSU_ERR_INVALID;
    }

    relative_offset = logical_offset - logdev->logical_offset;
    if (relative_offset > logdev->length ||
        len > logdev->length - relative_offset) {
        return SSU_ERR_INVALID;
    }

    if (logdev->phys_sector > UINT64_MAX / SSU_DATAPLANE_SECTOR_SIZE) {
        return SSU_ERR_INVALID;
    }

    phys_base = logdev->phys_sector * SSU_DATAPLANE_SECTOR_SIZE;
    if (relative_offset > UINT64_MAX - phys_base) {
        return SSU_ERR_INVALID;
    }

    phys_offset = phys_base + relative_offset;
    *out_offset = (off_t)phys_offset;
    if ((uint64_t)*out_offset != phys_offset) {
        return SSU_ERR_INVALID;
    }

    if (len > 0) {
        if ((uint64_t)len - 1 > UINT64_MAX - phys_offset) {
            return SSU_ERR_INVALID;
        }

        last_offset = phys_offset + (uint64_t)len - 1;
        if ((uint64_t)(off_t)last_offset != last_offset) {
            return SSU_ERR_INVALID;
        }
    }

    return SSU_OK;
}

static ssu_err_t write_all(int fd, const unsigned char *buf, size_t len,
                           off_t offset)
{
    size_t done = 0;

    while (done < len) {
        ssize_t rc = pwrite(fd, buf + done, len - done, offset + done);

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SSU_ERR_IO;
        }

        if (rc == 0) {
            return SSU_ERR_IO;
        }

        done += (size_t)rc;
    }

    return SSU_OK;
}

static ssu_err_t read_all(int fd, unsigned char *buf, size_t len,
                          off_t offset)
{
    size_t done = 0;

    while (done < len) {
        ssize_t rc = pread(fd, buf + done, len - done, offset + done);

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SSU_ERR_IO;
        }

        if (rc == 0) {
            memset(buf + done, 0, len - done);
            return SSU_OK;
        }

        done += (size_t)rc;
    }

    return SSU_OK;
}

ssu_err_t ssu_dataplane_write(const ssu_logdev_info_t *logdev,
                              uint64_t logical_offset,
                              const void *buf,
                              size_t len)
{
    off_t phys_offset;
    ssu_err_t err;
    int fd;

    if (buf == NULL && len > 0) {
        return SSU_ERR_INVALID;
    }

    err = translate_offset(logdev, logical_offset, len, &phys_offset);
    if (err != SSU_OK) {
        return err;
    }

    fd = open(logdev->phys_dev, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        return SSU_ERR_IO;
    }

    err = write_all(fd, (const unsigned char *)buf, len, phys_offset);
    close(fd);
    return err;
}

ssu_err_t ssu_dataplane_read(const ssu_logdev_info_t *logdev,
                             uint64_t logical_offset,
                             void *buf,
                             size_t len)
{
    off_t phys_offset;
    ssu_err_t err;
    int fd;

    if (buf == NULL && len > 0) {
        return SSU_ERR_INVALID;
    }

    err = translate_offset(logdev, logical_offset, len, &phys_offset);
    if (err != SSU_OK) {
        return err;
    }

    fd = open(logdev->phys_dev, O_RDONLY);
    if (fd < 0) {
        return SSU_ERR_IO;
    }

    err = read_all(fd, (unsigned char *)buf, len, phys_offset);
    close(fd);
    return err;
}
