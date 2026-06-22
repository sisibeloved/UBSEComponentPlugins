#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/limits.h>
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/numa.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "reqshim_internal.h"

#define SSU_REQSHIM_DISK_NAME "ssu"
#define SSU_REQSHIM_QUEUE_DEPTH 128U
#define SSU_REQSHIM_SECTOR_SHIFT 9U
#define SSU_REQSHIM_SECTOR_SIZE (1U << SSU_REQSHIM_SECTOR_SHIFT)

struct reqshim_logdev_slot {
    bool active;
    struct ssu_logdev_req req;
    struct gendisk *disk;
    struct blk_mq_tag_set tag_set;
};

struct reqshim_rq_work {
    struct work_struct work;
    struct reqshim_logdev_slot *slot;
    struct request *rq;
};

static DEFINE_MUTEX(logdev_lock);
static struct reqshim_logdev_slot logdevs[SSU_REQSHIM_MAX_LOGDEVS];
static int reqshim_major;

static blk_status_t ssu_reqshim_errno_to_blk_status(int err)
{
    switch (err) {
    case 0:
        return BLK_STS_OK;
    case -ENOMEM:
    case -ENOSPC:
        return BLK_STS_RESOURCE;
    case -EOPNOTSUPP:
        return BLK_STS_NOTSUPP;
    default:
        return BLK_STS_IOERR;
    }
}

static blk_status_t ssu_reqshim_submit_bvec(
    struct reqshim_logdev_slot *slot,
    struct request *rq,
    struct bio_vec *bvec,
    sector_t *logical_sector)
{
    unsigned int done = 0;

    if ((bvec->bv_len & (SSU_REQSHIM_SECTOR_SIZE - 1U)) != 0)
        return BLK_STS_IOERR;

    while (done < bvec->bv_len) {
        struct ssu_map_entry entry;
        __u64 phys_sector;
        __u64 entry_end;
        __u64 sectors_left;
        unsigned int bytes_left;
        unsigned int max_bytes;
        unsigned int chunk;
        int err;
        blk_status_t status;

        err = ssu_reqshim_translate_sector(slot->req.minor,
                                           (__u64)*logical_sector,
                                           &entry, &phys_sector);
        if (err)
            return ssu_reqshim_errno_to_blk_status(err);

        entry_end = entry.logical_sector + entry.length_sectors;
        if ((__u64)*logical_sector >= entry_end)
            return BLK_STS_IOERR;

        sectors_left = entry_end - (__u64)*logical_sector;
        bytes_left = bvec->bv_len - done;
        if (sectors_left > (UINT_MAX >> SSU_REQSHIM_SECTOR_SHIFT))
            max_bytes = UINT_MAX & ~(SSU_REQSHIM_SECTOR_SIZE - 1U);
        else
            max_bytes = (unsigned int)sectors_left <<
                        SSU_REQSHIM_SECTOR_SHIFT;

        chunk = min(bytes_left, max_bytes);
        chunk &= ~(SSU_REQSHIM_SECTOR_SIZE - 1U);
        if (chunk == 0)
            return BLK_STS_IOERR;

        status = ssu_reqshim_phys_submit_bvec(&entry, (sector_t)phys_sector,
                                              rq->cmd_flags, bvec->bv_page,
                                              chunk,
                                              bvec->bv_offset + done);
        if (status != BLK_STS_OK)
            return status;

        done += chunk;
        *logical_sector += chunk >> SSU_REQSHIM_SECTOR_SHIFT;
    }

    return BLK_STS_OK;
}

static void ssu_reqshim_rq_workfn(struct work_struct *work)
{
    struct reqshim_rq_work *ctx =
        container_of(work, struct reqshim_rq_work, work);
    struct request *rq = ctx->rq;
    struct reqshim_logdev_slot *slot = ctx->slot;
    struct req_iterator iter;
    struct bio_vec bvec;
    sector_t logical_sector = blk_rq_pos(rq);
    blk_status_t status = BLK_STS_OK;

    if (!slot || !slot->active || blk_rq_is_passthrough(rq)) {
        status = BLK_STS_IOERR;
        goto done;
    }

    if (req_op(rq) != REQ_OP_READ && req_op(rq) != REQ_OP_WRITE) {
        status = BLK_STS_NOTSUPP;
        goto done;
    }

    rq_for_each_segment(bvec, rq, iter) {
        status = ssu_reqshim_submit_bvec(slot, rq, &bvec,
                                         &logical_sector);
        if (status != BLK_STS_OK)
            break;
    }

done:
    blk_mq_end_request(rq, status);
    kfree(ctx);
}

static blk_status_t ssu_reqshim_queue_rq(struct blk_mq_hw_ctx *hctx,
                                         const struct blk_mq_queue_data *bd)
{
    struct reqshim_rq_work *ctx;

    ctx = kzalloc(sizeof(*ctx), GFP_ATOMIC);
    if (!ctx)
        return BLK_STS_RESOURCE;

    blk_mq_start_request(bd->rq);
    ctx->rq = bd->rq;
    ctx->slot = hctx->queue->queuedata;
    INIT_WORK(&ctx->work, ssu_reqshim_rq_workfn);
    queue_work(system_unbound_wq, &ctx->work);
    return BLK_STS_OK;
}

static const struct blk_mq_ops ssu_reqshim_mq_ops = {
    .queue_rq = ssu_reqshim_queue_rq,
};

static const struct block_device_operations ssu_reqshim_bdev_ops = {
    .owner = THIS_MODULE,
};

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

static int ssu_reqshim_validate_logdev_req(const struct ssu_logdev_req *req)
{
    if (!req || req->minor >= SSU_REQSHIM_MAX_LOGDEVS ||
        req->capacity_sectors == 0 || req->logical_block_size == 0)
        return -EINVAL;

    if (req->logical_block_size < SSU_REQSHIM_SECTOR_SIZE ||
        !is_power_of_2(req->logical_block_size))
        return -EINVAL;

    return 0;
}

int ssu_reqshim_logdev_create(const struct ssu_logdev_req *req)
{
    struct reqshim_logdev_slot *slot;
    struct gendisk *disk;
    int err;

    err = ssu_reqshim_validate_logdev_req(req);
    if (err)
        return err;

    if (reqshim_major <= 0)
        return -ENODEV;

    mutex_lock(&logdev_lock);
    if (logdevs[req->minor].active) {
        mutex_unlock(&logdev_lock);
        return -EBUSY;
    }

    slot = &logdevs[req->minor];
    memset(slot, 0, sizeof(*slot));
    slot->req = *req;

    slot->tag_set.ops = &ssu_reqshim_mq_ops;
    slot->tag_set.nr_hw_queues = 1;
    slot->tag_set.queue_depth = SSU_REQSHIM_QUEUE_DEPTH;
    slot->tag_set.numa_node = NUMA_NO_NODE;
    slot->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;

    err = blk_mq_alloc_tag_set(&slot->tag_set);
    if (err)
        goto fail_clear;

    disk = blk_mq_alloc_disk(&slot->tag_set, slot);
    if (IS_ERR(disk)) {
        err = PTR_ERR(disk);
        goto fail_tag_set;
    }

    slot->disk = disk;
    disk->major = reqshim_major;
    disk->first_minor = req->minor;
    disk->minors = 1;
    disk->fops = &ssu_reqshim_bdev_ops;
    disk->private_data = slot;
    snprintf(disk->disk_name, DISK_NAME_LEN, "ssu%u", req->minor);
    blk_queue_logical_block_size(disk->queue, req->logical_block_size);
    set_capacity(disk, req->capacity_sectors);

    err = add_disk(disk);
    if (err)
        goto fail_disk;

    slot->active = true;
    mutex_unlock(&logdev_lock);
    return 0;

fail_disk:
    put_disk(disk);
    slot->disk = NULL;
fail_tag_set:
    blk_mq_free_tag_set(&slot->tag_set);
fail_clear:
    memset(slot, 0, sizeof(*slot));
    mutex_unlock(&logdev_lock);
    return err;
}

int ssu_reqshim_logdev_destroy(const struct ssu_logdev_req *req)
{
    struct reqshim_logdev_slot *slot;
    struct gendisk *disk;

    if (!req || req->minor >= SSU_REQSHIM_MAX_LOGDEVS)
        return -EINVAL;

    mutex_lock(&logdev_lock);
    slot = &logdevs[req->minor];
    if (!slot->active) {
        mutex_unlock(&logdev_lock);
        return -ENOENT;
    }

    slot->active = false;
    disk = slot->disk;
    slot->disk = NULL;
    mutex_unlock(&logdev_lock);

    if (disk) {
        del_gendisk(disk);
        put_disk(disk);
    }
    blk_mq_free_tag_set(&slot->tag_set);
    memset(slot, 0, sizeof(*slot));
    ssu_reqshim_map_clear_minor(req->minor);
    return 0;
}

int ssu_reqshim_blk_init(void)
{
    reqshim_major = register_blkdev(0, SSU_REQSHIM_DISK_NAME);
    if (reqshim_major <= 0)
        return reqshim_major == 0 ? -EBUSY : reqshim_major;

    return 0;
}

void ssu_reqshim_blk_exit(void)
{
    __u32 minor;

    for (minor = 0; minor < SSU_REQSHIM_MAX_LOGDEVS; minor++) {
        struct ssu_logdev_req req = {
            .minor = minor,
        };

        if (ssu_reqshim_logdev_exists(minor))
            ssu_reqshim_logdev_destroy(&req);
    }

    if (reqshim_major > 0) {
        unregister_blkdev(reqshim_major, SSU_REQSHIM_DISK_NAME);
        reqshim_major = 0;
    }
}
