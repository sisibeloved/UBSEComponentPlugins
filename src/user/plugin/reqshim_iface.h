#ifndef SSU_PLUGIN_REQSHIM_IFACE_H
#define SSU_PLUGIN_REQSHIM_IFACE_H

#include <stddef.h>
#include <stdint.h>

#include "ssu_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSU_REQSHIM_DEFAULT_CTL_PATH "/dev/ssu-ctl"
#define SSU_REQSHIM_SECTOR_SIZE 512ULL

typedef struct {
    char phys_dev[64];
    char ns_id[32];
    uint64_t logical_offset;
    uint64_t length;
    uint64_t phys_sector;
} ssu_reqshim_map_spec_t;

typedef void (*ssu_reqshim_log_fn)(void *ctx, const char *message);

typedef int (*ssu_reqshim_open_fn)(const char *path, int flags);
typedef int (*ssu_reqshim_ioctl_fn)(int fd, unsigned long request, void *arg);
typedef int (*ssu_reqshim_close_fn)(int fd);

typedef struct {
    ssu_reqshim_open_fn open_fn;
    ssu_reqshim_ioctl_fn ioctl_fn;
    ssu_reqshim_close_fn close_fn;
} ssu_reqshim_sys_ops_t;

ssu_err_t ssu_reqshim_parse_logical_minor(const char *logical_dev,
                                          uint32_t *out_minor);

ssu_err_t ssu_reqshim_mount_logdev_for_allocate(
    const char *logical_dev,
    const char *allocate_id,
    const ssu_reqshim_map_spec_t *maps,
    uint32_t map_count,
    int allow_missing_ctl,
    ssu_reqshim_log_fn log_fn,
    void *log_ctx,
    const ssu_reqshim_sys_ops_t *ops,
    const char *ctl_path);

ssu_err_t ssu_reqshim_mount_logdev(
    const char *logical_dev,
    const ssu_reqshim_map_spec_t *maps,
    uint32_t map_count,
    int allow_missing_ctl,
    ssu_reqshim_log_fn log_fn,
    void *log_ctx,
    const ssu_reqshim_sys_ops_t *ops,
    const char *ctl_path);

ssu_err_t ssu_reqshim_unmount_logdev_for_allocate(
    const char *logical_dev,
    const char *allocate_id,
    const ssu_reqshim_map_spec_t *maps,
    uint32_t map_count,
    int allow_missing_ctl,
    ssu_reqshim_log_fn log_fn,
    void *log_ctx,
    const ssu_reqshim_sys_ops_t *ops,
    const char *ctl_path);

ssu_err_t ssu_reqshim_unmount_logdev(
    const char *logical_dev,
    const ssu_reqshim_map_spec_t *maps,
    uint32_t map_count,
    int allow_missing_ctl,
    ssu_reqshim_log_fn log_fn,
    void *log_ctx,
    const ssu_reqshim_sys_ops_t *ops,
    const char *ctl_path);

#ifdef __cplusplus
}
#endif

#endif
