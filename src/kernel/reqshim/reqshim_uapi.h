#ifndef SSU_REQSHIM_UAPI_H
#define SSU_REQSHIM_UAPI_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t __u32;
typedef uint64_t __u64;
#endif

#define SSU_IOCTL_MAGIC 'S'
#define SSU_REQSHIM_UAPI_VERSION 1U

struct ssu_logdev_req {
    __u32 minor;
    __u64 capacity_sectors;
    __u32 logical_block_size;
    __u32 flags;
};

struct ssu_map_entry {
    __u32 logical_minor;
    __u64 logical_sector;
    __u64 length_sectors;
    char phys_dev[64];
    __u32 nsid;
    __u64 phys_sector;
};

struct ssu_map_query {
    __u32 logical_minor;
    __u64 logical_sector;
    struct ssu_map_entry entry;
};

#define SSU_IOC_LOGDEV_CREATE   _IOW(SSU_IOCTL_MAGIC, 0x00, struct ssu_logdev_req)
#define SSU_IOC_MAP_ADD         _IOW(SSU_IOCTL_MAGIC, 0x01, struct ssu_map_entry)
#define SSU_IOC_MAP_DEL         _IOW(SSU_IOCTL_MAGIC, 0x02, struct ssu_map_entry)
#define SSU_IOC_MAP_QUERY       _IOWR(SSU_IOCTL_MAGIC, 0x03, struct ssu_map_query)
#define SSU_IOC_LOGDEV_DESTROY  _IOW(SSU_IOCTL_MAGIC, 0x04, struct ssu_logdev_req)
#define SSU_IOC_GET_VERSION     _IOR(SSU_IOCTL_MAGIC, 0x7f, __u32)

#endif
