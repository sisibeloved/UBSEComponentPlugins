#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include "reqshim_internal.h"

#define SSU_REQSHIM_DEVICE_NAME "ssu-ctl"

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
MODULE_DESCRIPTION("SSU ReqShim control plane");
MODULE_LICENSE("GPL");
