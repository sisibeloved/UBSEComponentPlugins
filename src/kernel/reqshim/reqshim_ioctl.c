#include <linux/copy_to_user.h>
#include <linux/errno.h>

#include "reqshim_internal.h"

long ssu_reqshim_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    __u32 version = SSU_REQSHIM_UAPI_VERSION;

    (void)file;

    switch (cmd) {
    case SSU_IOC_GET_VERSION:
        if (copy_to_user((void __user *)arg, &version, sizeof(version)))
            return -EFAULT;
        return 0;
    case SSU_IOC_LOGDEV_CREATE: {
        struct ssu_logdev_req req;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        return ssu_reqshim_logdev_create(&req);
    }
    case SSU_IOC_LOGDEV_DESTROY: {
        struct ssu_logdev_req req;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        return ssu_reqshim_logdev_destroy(&req);
    }
    case SSU_IOC_MAP_ADD: {
        struct ssu_map_entry entry;

        if (copy_from_user(&entry, (void __user *)arg, sizeof(entry)))
            return -EFAULT;
        return ssu_reqshim_map_add(&entry);
    }
    case SSU_IOC_MAP_DEL: {
        struct ssu_map_entry entry;

        if (copy_from_user(&entry, (void __user *)arg, sizeof(entry)))
            return -EFAULT;
        return ssu_reqshim_map_del(&entry);
    }
    case SSU_IOC_MAP_QUERY: {
        struct ssu_map_query query;
        int err;

        if (copy_from_user(&query, (void __user *)arg, sizeof(query)))
            return -EFAULT;
        err = ssu_reqshim_map_query(&query);
        if (err)
            return err;
        if (copy_to_user((void __user *)arg, &query, sizeof(query)))
            return -EFAULT;
        return 0;
    }
    default:
        return -ENOTTY;
    }
}
