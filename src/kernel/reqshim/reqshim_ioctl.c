#include <linux/errno.h>
#include <linux/uaccess.h>

#include "reqshim_internal.h"

long ssu_reqshim_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    __u32 version = SSU_REQSHIM_UAPI_VERSION;

    (void)file;

    switch (cmd) {
    case SSU_IOC_GET_VERSION:
        ssu_reqshim_info("ioctl get_version version=%u\n", version);
        if (copy_to_user((void __user *)arg, &version, sizeof(version)))
            return -EFAULT;
        return 0;
    case SSU_IOC_LOGDEV_CREATE: {
        struct ssu_logdev_req req;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        ssu_reqshim_info(
            "ioctl logdev_create minor=%u name=%s capacity_sectors=%llu block=%u flags=0x%x\n",
            req.minor, req.disk_name,
            (unsigned long long)req.capacity_sectors,
            req.logical_block_size, req.flags);
        return ssu_reqshim_logdev_create(&req);
    }
    case SSU_IOC_LOGDEV_DESTROY: {
        struct ssu_logdev_req req;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        ssu_reqshim_info("ioctl logdev_destroy minor=%u name=%s\n",
                         req.minor, req.disk_name);
        return ssu_reqshim_logdev_destroy(&req);
    }
    case SSU_IOC_MAP_ADD: {
        struct ssu_map_entry entry;

        if (copy_from_user(&entry, (void __user *)arg, sizeof(entry)))
            return -EFAULT;
        ssu_reqshim_info(
            "ioctl map_add minor=%u logical_sector=%llu len=%llu phys_dev=%s nsid=%u phys_sector=%llu\n",
            entry.logical_minor, (unsigned long long)entry.logical_sector,
            (unsigned long long)entry.length_sectors, entry.phys_dev,
            entry.nsid, (unsigned long long)entry.phys_sector);
        return ssu_reqshim_map_add(&entry);
    }
    case SSU_IOC_MAP_DEL: {
        struct ssu_map_entry entry;

        if (copy_from_user(&entry, (void __user *)arg, sizeof(entry)))
            return -EFAULT;
        ssu_reqshim_info("ioctl map_del minor=%u logical_sector=%llu\n",
                         entry.logical_minor,
                         (unsigned long long)entry.logical_sector);
        return ssu_reqshim_map_del(&entry);
    }
    case SSU_IOC_MAP_QUERY: {
        struct ssu_map_query query;
        int err;

        if (copy_from_user(&query, (void __user *)arg, sizeof(query)))
            return -EFAULT;
        ssu_reqshim_io("ioctl map_query minor=%u logical_sector=%llu\n",
                       query.logical_minor,
                       (unsigned long long)query.logical_sector);
        err = ssu_reqshim_map_query(&query);
        if (err)
            return err;
        if (copy_to_user((void __user *)arg, &query, sizeof(query)))
            return -EFAULT;
        return 0;
    }
    default:
        ssu_reqshim_warn("ioctl unknown cmd=0x%x\n", cmd);
        return -ENOTTY;
    }
}
