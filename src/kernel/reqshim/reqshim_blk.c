#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include "reqshim_internal.h"

struct reqshim_logdev_slot {
    bool active;
    struct ssu_logdev_req req;
};

static DEFINE_MUTEX(logdev_lock);
static struct reqshim_logdev_slot logdevs[SSU_REQSHIM_MAX_LOGDEVS];

bool ssu_reqshim_logdev_exists(__u32 minor)
{
    bool exists = false;

    if (minor >= SSU_REQSHIM_MAX_LOGDEVS)
        return false;

    mutex_lock(&logdev_lock);
    exists = logdevs[minor].active;
    mutex_unlock(&logdev_lock);
    return exists;
}

int ssu_reqshim_logdev_create(const struct ssu_logdev_req *req)
{
    if (!req || req->minor >= SSU_REQSHIM_MAX_LOGDEVS ||
        req->capacity_sectors == 0 || req->logical_block_size == 0)
        return -EINVAL;

    mutex_lock(&logdev_lock);
    if (logdevs[req->minor].active) {
        mutex_unlock(&logdev_lock);
        return -EBUSY;
    }

    logdevs[req->minor].active = true;
    logdevs[req->minor].req = *req;
    mutex_unlock(&logdev_lock);
    return 0;
}

int ssu_reqshim_logdev_destroy(const struct ssu_logdev_req *req)
{
    if (!req || req->minor >= SSU_REQSHIM_MAX_LOGDEVS)
        return -EINVAL;

    mutex_lock(&logdev_lock);
    if (!logdevs[req->minor].active) {
        mutex_unlock(&logdev_lock);
        return -ENOENT;
    }

    memset(&logdevs[req->minor], 0, sizeof(logdevs[req->minor]));
    mutex_unlock(&logdev_lock);
    ssu_reqshim_map_clear_minor(req->minor);
    return 0;
}
