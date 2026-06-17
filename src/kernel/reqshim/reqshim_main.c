#include <linux/copy_to_user.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include "reqshim_uapi.h"

#define SSU_REQSHIM_DEVICE_NAME "ssu-ctl"

static long ssu_reqshim_ioctl(struct file *file, unsigned int cmd,
                              unsigned long arg)
{
    __u32 version = SSU_REQSHIM_UAPI_VERSION;

    (void)file;

    switch (cmd) {
    case SSU_IOC_GET_VERSION:
        if (copy_to_user((void __user *)arg, &version, sizeof(version))) {
            return -EFAULT;
        }
        return 0;
    default:
        return -ENOTTY;
    }
}

static const struct file_operations ssu_reqshim_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = ssu_reqshim_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = ssu_reqshim_ioctl,
#endif
};

static struct miscdevice ssu_reqshim_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = SSU_REQSHIM_DEVICE_NAME,
    .fops = &ssu_reqshim_fops,
    .mode = 0600,
};

static int __init ssu_reqshim_init(void)
{
    return misc_register(&ssu_reqshim_misc);
}

static void __exit ssu_reqshim_exit(void)
{
    misc_deregister(&ssu_reqshim_misc);
}

module_init(ssu_reqshim_init);
module_exit(ssu_reqshim_exit);

MODULE_AUTHOR("UBSEComponentPlugins");
MODULE_DESCRIPTION("SSU ReqShim skeleton");
MODULE_LICENSE("GPL");
