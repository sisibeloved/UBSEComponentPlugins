#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include "reqshim_internal.h"

#define SSU_REQSHIM_DEVICE_NAME "ssu-ctl"

bool ssu_reqshim_trace_io;
module_param_named(trace_io, ssu_reqshim_trace_io, bool, 0644);
MODULE_PARM_DESC(trace_io,
                 "Enable verbose ReqShim data-plane IO logs");

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
    int err;

    ssu_reqshim_info("module init trace_io=%d\n",
                     ssu_reqshim_trace_io ? 1 : 0);

    err = ssu_reqshim_blk_init();
    if (err) {
        ssu_reqshim_warn("blk init failed err=%d\n", err);
        return err;
    }

    err = misc_register(&ssu_reqshim_misc);
    if (err) {
        ssu_reqshim_warn("misc register failed device=%s err=%d\n",
                         SSU_REQSHIM_DEVICE_NAME, err);
        ssu_reqshim_blk_exit();
        return err;
    }

    ssu_reqshim_info("control device ready /dev/%s\n",
                     SSU_REQSHIM_DEVICE_NAME);
    return 0;
}

static void __exit ssu_reqshim_exit(void)
{
    ssu_reqshim_info("module exit\n");
    misc_deregister(&ssu_reqshim_misc);
    ssu_reqshim_blk_exit();
}

module_init(ssu_reqshim_init);
module_exit(ssu_reqshim_exit);

MODULE_AUTHOR("UBSEComponentPlugins");
MODULE_DESCRIPTION("SSU ReqShim block data plane");
MODULE_LICENSE("GPL");
