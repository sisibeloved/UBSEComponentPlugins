#include "reqshim_iface.h"

#include "../../kernel/reqshim/reqshim_uapi.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <vector>

static int default_open_ctl(const char *path, int flags)
{
    return open(path, flags);
}

static int default_ioctl_ctl(int fd, unsigned long request, void *arg)
{
    return ioctl(fd, request, arg);
}

static int default_close_ctl(int fd)
{
    return close(fd);
}

static const ssu_reqshim_sys_ops_t default_ops = {
    default_open_ctl,
    default_ioctl_ctl,
    default_close_ctl,
};

static int is_empty_string(const char *s)
{
    return s == NULL || s[0] == '\0';
}

static void reqshim_log(ssu_reqshim_log_fn log_fn, void *ctx,
                        const char *fmt, ...)
{
    char message[512];
    va_list ap;

    if (log_fn == NULL) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    log_fn(ctx, message);
}

static const ssu_reqshim_sys_ops_t *choose_ops(
    const ssu_reqshim_sys_ops_t *ops)
{
    if (ops == NULL) {
        return &default_ops;
    }

    if (ops->open_fn == NULL || ops->ioctl_fn == NULL ||
        ops->close_fn == NULL) {
        return NULL;
    }

    return ops;
}

static ssu_err_t errno_to_ssu_err(int saved_errno)
{
    switch (saved_errno) {
    case EINVAL:
        return SSU_ERR_INVALID;
    case ENOMEM:
    case ENOSPC:
        return SSU_ERR_NO_RESOURCE;
    case ENOENT:
    case ENODEV:
        return SSU_ERR_NOT_FOUND;
    case EBUSY:
    case EEXIST:
        return SSU_ERR_BUSY;
    case EOPNOTSUPP:
    case ENOSYS:
        return SSU_ERR_UNSUPPORTED;
    default:
        return SSU_ERR_KERNEL;
    }
}

static int parse_uint_tail(const char *s, uint32_t *out)
{
    const char *p;
    char *end = NULL;
    unsigned long value;

    if (is_empty_string(s) || out == NULL) {
        return 0;
    }

    p = s + strlen(s);
    while (p > s && isdigit((unsigned char)p[-1])) {
        p--;
    }

    if (p == s + strlen(s)) {
        *out = 0;
        return 1;
    }

    errno = 0;
    value = strtoul(p, &end, 10);
    if (errno != 0 || end == p || *end != '\0' || value > UINT32_MAX) {
        return 0;
    }

    *out = (uint32_t)value;
    return 1;
}

ssu_err_t ssu_reqshim_parse_logical_minor(const char *logical_dev,
                                          uint32_t *out_minor)
{
    const char *prefix = "ssu";
    const char *base;
    const char *digits;
    char *end = NULL;
    unsigned long value;

    if (is_empty_string(logical_dev) || out_minor == NULL) {
        return SSU_ERR_INVALID;
    }

    base = strrchr(logical_dev, '/');
    base = base == NULL ? logical_dev : base + 1;
    if (strncmp(base, prefix, strlen(prefix)) != 0) {
        return SSU_ERR_INVALID;
    }
    digits = base + strlen(prefix);

    if (*digits == '\0') {
        return SSU_ERR_INVALID;
    }

    errno = 0;
    value = strtoul(digits, &end, 10);
    if (errno != 0 || end == digits || *end != '\0' ||
        value > UINT32_MAX) {
        return SSU_ERR_INVALID;
    }

    *out_minor = (uint32_t)value;
    return SSU_OK;
}

static ssu_err_t parse_allocate_minor(const char *allocate_id,
                                      uint32_t *out_minor)
{
    const char *prefix = "alloc-";
    const char *digits;
    char *end = NULL;
    unsigned long value;

    if (is_empty_string(allocate_id) || out_minor == NULL ||
        strncmp(allocate_id, prefix, strlen(prefix)) != 0) {
        return SSU_ERR_INVALID;
    }

    digits = allocate_id + strlen(prefix);
    errno = 0;
    value = strtoul(digits, &end, 10);
    if (errno != 0 || end == digits || *end != '\0' ||
        value > UINT32_MAX) {
        return SSU_ERR_INVALID;
    }

    *out_minor = (uint32_t)value;
    return SSU_OK;
}

static int logical_disk_name_char_is_valid(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.' || c == ':';
}

static ssu_err_t copy_logical_disk_name(const char *logical_dev,
                                        char *out,
                                        size_t n)
{
    const char *base;
    size_t len;
    size_t i;

    if (is_empty_string(logical_dev) || out == NULL || n == 0) {
        return SSU_ERR_INVALID;
    }

    base = strrchr(logical_dev, '/');
    base = base == NULL ? logical_dev : base + 1;
    if (is_empty_string(base)) {
        return SSU_ERR_INVALID;
    }

    len = strlen(base);
    if (len >= n || len > SSU_API_MAX_DISK_NAME_LEN) {
        return SSU_ERR_INVALID;
    }

    for (i = 0; i < len; i++) {
        if (!logical_disk_name_char_is_valid(base[i])) {
            return SSU_ERR_INVALID;
        }
    }

    snprintf(out, n, "%s", base);
    return SSU_OK;
}

static ssu_err_t build_map_entry(const ssu_reqshim_map_spec_t *spec,
                                 uint32_t minor,
                                 struct ssu_map_entry *entry)
{
    uint32_t nsid;

    if (spec == NULL || entry == NULL || is_empty_string(spec->phys_dev) ||
        spec->length == 0 ||
        spec->logical_offset % SSU_REQSHIM_SECTOR_SIZE != 0 ||
        spec->length % SSU_REQSHIM_SECTOR_SIZE != 0 ||
        strlen(spec->phys_dev) >= sizeof(entry->phys_dev)) {
        return SSU_ERR_INVALID;
    }

    if (!parse_uint_tail(spec->ns_id, &nsid)) {
        return SSU_ERR_INVALID;
    }

    memset(entry, 0, sizeof(*entry));
    entry->logical_minor = minor;
    entry->logical_sector = spec->logical_offset / SSU_REQSHIM_SECTOR_SIZE;
    entry->length_sectors = spec->length / SSU_REQSHIM_SECTOR_SIZE;
    snprintf(entry->phys_dev, sizeof(entry->phys_dev), "%s", spec->phys_dev);
    entry->nsid = nsid;
    entry->phys_sector = spec->phys_sector;
    return SSU_OK;
}

static ssu_err_t build_entries(const ssu_reqshim_map_spec_t *maps,
                               uint32_t map_count,
                               uint32_t minor,
                               std::vector<struct ssu_map_entry> *entries,
                               uint64_t *out_capacity_sectors)
{
    uint64_t capacity_bytes = 0;
    uint32_t i;

    if (maps == NULL || entries == NULL || out_capacity_sectors == NULL) {
        return SSU_ERR_INVALID;
    }

    entries->clear();
    for (i = 0; i < map_count; i++) {
        struct ssu_map_entry entry;
        uint64_t end_byte;
        ssu_err_t err;

        if (maps[i].logical_offset > UINT64_MAX - maps[i].length) {
            return SSU_ERR_INVALID;
        }

        err = build_map_entry(&maps[i], minor, &entry);
        if (err != SSU_OK) {
            return err;
        }

        end_byte = maps[i].logical_offset + maps[i].length;
        if (end_byte > capacity_bytes) {
            capacity_bytes = end_byte;
        }
        entries->push_back(entry);
    }

    if (capacity_bytes == 0 ||
        capacity_bytes % SSU_REQSHIM_SECTOR_SIZE != 0) {
        return SSU_ERR_INVALID;
    }

    *out_capacity_sectors = capacity_bytes / SSU_REQSHIM_SECTOR_SIZE;
    return SSU_OK;
}

static ssu_err_t open_reqshim_ctl(const ssu_reqshim_sys_ops_t *ops,
                                  const char *ctl_path,
                                  int allow_missing_ctl,
                                  ssu_reqshim_log_fn log_fn,
                                  void *log_ctx,
                                  int *out_fd,
                                  int *out_skipped)
{
    const char *path = is_empty_string(ctl_path) ?
                           SSU_REQSHIM_DEFAULT_CTL_PATH : ctl_path;
    int fd;
    int saved_errno;

    *out_fd = -1;
    *out_skipped = 0;

    errno = 0;
    fd = ops->open_fn(path, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        *out_fd = fd;
        return SSU_OK;
    }

    saved_errno = errno;
    if (allow_missing_ctl &&
        (saved_errno == ENOENT || saved_errno == ENODEV)) {
        reqshim_log(log_fn, log_ctx,
                    "ReqShim control device unavailable, skip ioctl path ctl=%s errno=%d",
                    path, saved_errno);
        *out_skipped = 1;
        return SSU_OK;
    }

    reqshim_log(log_fn, log_ctx,
                "ReqShim open failed ctl=%s errno=%d", path, saved_errno);
    return SSU_ERR_KERNEL;
}

static ssu_err_t do_ioctl(const ssu_reqshim_sys_ops_t *ops,
                          int fd,
                          unsigned long request,
                          void *arg,
                          const char *name,
                          ssu_reqshim_log_fn log_fn,
                          void *log_ctx)
{
    int saved_errno;

    errno = 0;
    if (ops->ioctl_fn(fd, request, arg) == 0) {
        return SSU_OK;
    }

    saved_errno = errno;
    reqshim_log(log_fn, log_ctx,
                "ReqShim ioctl failed op=%s errno=%d", name, saved_errno);
    return errno_to_ssu_err(saved_errno);
}

static ssu_err_t check_version(const ssu_reqshim_sys_ops_t *ops,
                               int fd,
                               ssu_reqshim_log_fn log_fn,
                               void *log_ctx)
{
    uint32_t version = 0;
    ssu_err_t err;

    err = do_ioctl(ops, fd, SSU_IOC_GET_VERSION, &version,
                   "SSU_IOC_GET_VERSION", log_fn, log_ctx);
    if (err != SSU_OK) {
        return err;
    }

    if (version != SSU_REQSHIM_UAPI_VERSION) {
        reqshim_log(log_fn, log_ctx,
                    "ReqShim UAPI version mismatch got=%u expected=%u",
                    version, SSU_REQSHIM_UAPI_VERSION);
        return SSU_ERR_UNSUPPORTED;
    }

    return SSU_OK;
}

static ssu_err_t mount_logdev_with_minor(
    const char *logical_dev,
    uint32_t minor,
    const ssu_reqshim_map_spec_t *maps,
    uint32_t map_count,
    int allow_missing_ctl,
    ssu_reqshim_log_fn log_fn,
    void *log_ctx,
    const ssu_reqshim_sys_ops_t *ops_arg,
    const char *ctl_path)
{
    const ssu_reqshim_sys_ops_t *ops = choose_ops(ops_arg);
    std::vector<struct ssu_map_entry> entries;
    struct ssu_logdev_req logdev_req;
    uint64_t capacity_sectors = 0;
    int fd = -1;
    int skipped = 0;
    uint32_t i;
    ssu_err_t err;

    if (ops == NULL || map_count == 0) {
        return SSU_ERR_INVALID;
    }

    err = build_entries(maps, map_count, minor, &entries, &capacity_sectors);
    if (err != SSU_OK) {
        return err;
    }

    memset(&logdev_req, 0, sizeof(logdev_req));
    logdev_req.minor = minor;
    logdev_req.capacity_sectors = capacity_sectors;
    logdev_req.logical_block_size = SSU_REQSHIM_SECTOR_SIZE;
    err = copy_logical_disk_name(logical_dev, logdev_req.disk_name,
                                 sizeof(logdev_req.disk_name));
    if (err != SSU_OK) {
        return err;
    }

    err = open_reqshim_ctl(ops, ctl_path, allow_missing_ctl, log_fn, log_ctx,
                           &fd, &skipped);
    if (err != SSU_OK || skipped) {
        return err;
    }

    err = check_version(ops, fd, log_fn, log_ctx);
    if (err != SSU_OK) {
        ops->close_fn(fd);
        return err;
    }

    err = do_ioctl(ops, fd, SSU_IOC_LOGDEV_CREATE, &logdev_req,
                   "SSU_IOC_LOGDEV_CREATE", log_fn, log_ctx);
    if (err != SSU_OK) {
        ops->close_fn(fd);
        return err;
    }
    reqshim_log(log_fn, log_ctx,
                "ReqShim logdev create logical_dev=%s minor=%u disk_name=%s capacity_sectors=%llu logical_block=%u maps=%u",
                logical_dev, minor, logdev_req.disk_name,
                (unsigned long long)logdev_req.capacity_sectors,
                logdev_req.logical_block_size, map_count);

    for (i = 0; i < entries.size(); i++) {
        reqshim_log(log_fn, log_ctx,
                    "ReqShim map add logical_dev=%s minor=%u logical_sector=%llu len=%llu phys_dev=%s nsid=%u phys_sector=%llu",
                    logical_dev, entries[i].logical_minor,
                    (unsigned long long)entries[i].logical_sector,
                    (unsigned long long)entries[i].length_sectors,
                    entries[i].phys_dev, entries[i].nsid,
                    (unsigned long long)entries[i].phys_sector);
        err = do_ioctl(ops, fd, SSU_IOC_MAP_ADD, &entries[i],
                       "SSU_IOC_MAP_ADD", log_fn, log_ctx);
        if (err != SSU_OK) {
            while (i > 0) {
                i--;
                do_ioctl(ops, fd, SSU_IOC_MAP_DEL, &entries[i],
                         "SSU_IOC_MAP_DEL", log_fn, log_ctx);
            }
            do_ioctl(ops, fd, SSU_IOC_LOGDEV_DESTROY, &logdev_req,
                     "SSU_IOC_LOGDEV_DESTROY", log_fn, log_ctx);
            ops->close_fn(fd);
            return err;
        }
    }

    ops->close_fn(fd);
    reqshim_log(log_fn, log_ctx,
                "ReqShim mount success logical_dev=%s minor=%u maps=%u capacity_sectors=%llu",
                logical_dev, minor, map_count,
                (unsigned long long)capacity_sectors);
    return SSU_OK;
}

ssu_err_t ssu_reqshim_mount_logdev_for_allocate(
    const char *logical_dev,
    const char *allocate_id,
    const ssu_reqshim_map_spec_t *maps,
    uint32_t map_count,
    int allow_missing_ctl,
    ssu_reqshim_log_fn log_fn,
    void *log_ctx,
    const ssu_reqshim_sys_ops_t *ops,
    const char *ctl_path)
{
    uint32_t minor;
    ssu_err_t err;

    err = parse_allocate_minor(allocate_id, &minor);
    if (err != SSU_OK) {
        err = ssu_reqshim_parse_logical_minor(logical_dev, &minor);
        if (err != SSU_OK) {
            return err;
        }
    }

    return mount_logdev_with_minor(logical_dev, minor, maps, map_count,
                                   allow_missing_ctl, log_fn, log_ctx,
                                   ops, ctl_path);
}

ssu_err_t ssu_reqshim_mount_logdev(
    const char *logical_dev,
    const ssu_reqshim_map_spec_t *maps,
    uint32_t map_count,
    int allow_missing_ctl,
    ssu_reqshim_log_fn log_fn,
    void *log_ctx,
    const ssu_reqshim_sys_ops_t *ops,
    const char *ctl_path)
{
    uint32_t minor;
    ssu_err_t err;

    err = ssu_reqshim_parse_logical_minor(logical_dev, &minor);
    if (err != SSU_OK) {
        return err;
    }

    return mount_logdev_with_minor(logical_dev, minor, maps, map_count,
                                   allow_missing_ctl, log_fn, log_ctx,
                                   ops, ctl_path);
}

static ssu_err_t unmount_logdev_with_minor(
    const char *logical_dev,
    uint32_t minor,
    const ssu_reqshim_map_spec_t *maps,
    uint32_t map_count,
    int allow_missing_ctl,
    ssu_reqshim_log_fn log_fn,
    void *log_ctx,
    const ssu_reqshim_sys_ops_t *ops_arg,
    const char *ctl_path)
{
    const ssu_reqshim_sys_ops_t *ops = choose_ops(ops_arg);
    std::vector<struct ssu_map_entry> entries;
    struct ssu_logdev_req logdev_req;
    uint64_t capacity_sectors = 0;
    int fd = -1;
    int skipped = 0;
    uint32_t i;
    ssu_err_t err;

    if (ops == NULL) {
        return SSU_ERR_INVALID;
    }

    if (map_count > 0) {
        err = build_entries(maps, map_count, minor, &entries,
                            &capacity_sectors);
        if (err != SSU_OK) {
            return err;
        }
    }

    err = open_reqshim_ctl(ops, ctl_path, allow_missing_ctl, log_fn, log_ctx,
                           &fd, &skipped);
    if (err != SSU_OK || skipped) {
        return err;
    }

    err = check_version(ops, fd, log_fn, log_ctx);
    if (err != SSU_OK) {
        ops->close_fn(fd);
        return err;
    }

    for (i = 0; i < entries.size(); i++) {
        reqshim_log(log_fn, log_ctx,
                    "ReqShim map del logical_dev=%s minor=%u logical_sector=%llu len=%llu phys_dev=%s nsid=%u phys_sector=%llu",
                    logical_dev, entries[i].logical_minor,
                    (unsigned long long)entries[i].logical_sector,
                    (unsigned long long)entries[i].length_sectors,
                    entries[i].phys_dev, entries[i].nsid,
                    (unsigned long long)entries[i].phys_sector);
        do_ioctl(ops, fd, SSU_IOC_MAP_DEL, &entries[i],
                 "SSU_IOC_MAP_DEL", log_fn, log_ctx);
    }

    memset(&logdev_req, 0, sizeof(logdev_req));
    logdev_req.minor = minor;
    logdev_req.capacity_sectors = capacity_sectors;
    logdev_req.logical_block_size = SSU_REQSHIM_SECTOR_SIZE;

    err = do_ioctl(ops, fd, SSU_IOC_LOGDEV_DESTROY, &logdev_req,
                   "SSU_IOC_LOGDEV_DESTROY", log_fn, log_ctx);
    ops->close_fn(fd);
    if (err != SSU_OK) {
        return err;
    }

    reqshim_log(log_fn, log_ctx,
                "ReqShim unmount success logical_dev=%s minor=%u maps=%u",
                logical_dev, minor, map_count);
    return SSU_OK;
}

ssu_err_t ssu_reqshim_unmount_logdev_for_allocate(
    const char *logical_dev,
    const char *allocate_id,
    const ssu_reqshim_map_spec_t *maps,
    uint32_t map_count,
    int allow_missing_ctl,
    ssu_reqshim_log_fn log_fn,
    void *log_ctx,
    const ssu_reqshim_sys_ops_t *ops,
    const char *ctl_path)
{
    uint32_t minor;
    ssu_err_t err;

    err = parse_allocate_minor(allocate_id, &minor);
    if (err != SSU_OK) {
        err = ssu_reqshim_parse_logical_minor(logical_dev, &minor);
        if (err != SSU_OK) {
            return err;
        }
    }

    return unmount_logdev_with_minor(logical_dev, minor, maps, map_count,
                                     allow_missing_ctl, log_fn, log_ctx,
                                     ops, ctl_path);
}

ssu_err_t ssu_reqshim_unmount_logdev(
    const char *logical_dev,
    const ssu_reqshim_map_spec_t *maps,
    uint32_t map_count,
    int allow_missing_ctl,
    ssu_reqshim_log_fn log_fn,
    void *log_ctx,
    const ssu_reqshim_sys_ops_t *ops,
    const char *ctl_path)
{
    uint32_t minor;
    ssu_err_t err;

    err = ssu_reqshim_parse_logical_minor(logical_dev, &minor);
    if (err != SSU_OK) {
        return err;
    }

    return unmount_logdev_with_minor(logical_dev, minor, maps, map_count,
                                     allow_missing_ctl, log_fn, log_ctx,
                                     ops, ctl_path);
}
