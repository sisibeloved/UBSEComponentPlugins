#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/version.h>

#include "reqshim_internal.h"

struct ssu_reqshim_bdev_handle {
    struct block_device *bdev;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
    struct bdev_handle *handle;
#endif
};

static int ssu_reqshim_open_bdev(const char *path,
                                 struct ssu_reqshim_bdev_handle *handle)
{
    if (!path || !handle)
        return -EINVAL;

    memset(handle, 0, sizeof(*handle));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
    handle->handle = bdev_open_by_path(path,
                                       BLK_OPEN_READ | BLK_OPEN_WRITE,
                                       NULL, NULL);
    if (IS_ERR(handle->handle))
        return PTR_ERR(handle->handle);

    handle->bdev = handle->handle->bdev;
#else
    handle->bdev = blkdev_get_by_path(path, FMODE_READ | FMODE_WRITE, NULL);
    if (IS_ERR(handle->bdev))
        return PTR_ERR(handle->bdev);
#endif

    return 0;
}

static void ssu_reqshim_close_bdev(struct ssu_reqshim_bdev_handle *handle)
{
    if (!handle || !handle->bdev)
        return;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
    bdev_release(handle->handle);
#else
    blkdev_put(handle->bdev, FMODE_READ | FMODE_WRITE);
#endif
    memset(handle, 0, sizeof(*handle));
}

enum ssu_reqshim_phys_backend ssu_reqshim_phys_backend_for_entry(
    const struct ssu_map_entry *entry)
{
    if (!entry)
        return SSU_REQSHIM_PHYS_BACKEND_MOCK_BIO;

    if (strncmp(entry->phys_dev, "/dev/nvme", 9) == 0)
        return SSU_REQSHIM_PHYS_BACKEND_NVME_QUEUE;

    return SSU_REQSHIM_PHYS_BACKEND_MOCK_BIO;
}

static const char *ssu_reqshim_backend_name(enum ssu_reqshim_phys_backend backend)
{
    switch (backend) {
    case SSU_REQSHIM_PHYS_BACKEND_NVME_QUEUE:
        return "nvme-queue";
    case SSU_REQSHIM_PHYS_BACKEND_MOCK_BIO:
        return "block-bio";
    default:
        return "unknown";
    }
}

blk_status_t ssu_reqshim_phys_submit_bvec(const struct ssu_map_entry *entry,
                                          sector_t phys_sector,
                                          blk_opf_t opf,
                                          struct page *page,
                                          unsigned int len,
                                          unsigned int offset)
{
    struct ssu_reqshim_bdev_handle handle;
    enum ssu_reqshim_phys_backend backend;
    struct bio *bio;
    int err;

    if (!entry || !page || len == 0) {
        ssu_reqshim_warn_rl("phys submit invalid entry=%p page=%p len=%u\n",
                            entry, page, len);
        return BLK_STS_IOERR;
    }

    backend = ssu_reqshim_phys_backend_for_entry(entry);
    ssu_reqshim_io(
        "phys submit dev=%s backend=%s nsid=%u phys_sector=%llu len=%u offset=%u opf=0x%x\n",
        entry->phys_dev, ssu_reqshim_backend_name(backend), entry->nsid,
        (unsigned long long)phys_sector, len, offset, (unsigned int)opf);

    if (backend == SSU_REQSHIM_PHYS_BACKEND_NVME_QUEUE) {
        /*
         * MVP keeps NVMe queue submission behind the same physical backend
         * boundary. Until the LBC INI queue contract is wired in, submit the
         * namespace through the normal block device queue.
         */
        ssu_reqshim_io("phys backend fallback dev=%s backend=%s using=block-bio\n",
                       entry->phys_dev, ssu_reqshim_backend_name(backend));
    }

    err = ssu_reqshim_open_bdev(entry->phys_dev, &handle);
    if (err) {
        ssu_reqshim_warn_rl("phys open failed dev=%s err=%d\n",
                            entry->phys_dev, err);
        return BLK_STS_IOERR;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
    bio = bio_alloc(handle.bdev, 1, opf, GFP_NOIO);
#else
    bio = bio_alloc(GFP_NOIO, 1);
    if (bio) {
        bio_set_dev(bio, handle.bdev);
        bio->bi_opf = opf;
    }
#endif
    if (!bio) {
        ssu_reqshim_warn_rl("phys bio_alloc failed dev=%s len=%u\n",
                            entry->phys_dev, len);
        ssu_reqshim_close_bdev(&handle);
        return BLK_STS_RESOURCE;
    }

    bio->bi_iter.bi_sector = phys_sector;
    if (bio_add_page(bio, page, len, offset) != len) {
        ssu_reqshim_warn_rl(
            "phys bio_add_page failed dev=%s sector=%llu len=%u offset=%u\n",
            entry->phys_dev, (unsigned long long)phys_sector, len, offset);
        bio_put(bio);
        ssu_reqshim_close_bdev(&handle);
        return BLK_STS_RESOURCE;
    }

    err = submit_bio_wait(bio);
    if (err) {
        ssu_reqshim_warn_rl(
            "phys submit done dev=%s sector=%llu len=%u err=%d status=ioerr\n",
            entry->phys_dev, (unsigned long long)phys_sector, len, err);
    } else {
        ssu_reqshim_io(
            "phys submit done dev=%s sector=%llu len=%u err=0 status=ok\n",
            entry->phys_dev, (unsigned long long)phys_sector, len);
    }
    bio_put(bio);
    ssu_reqshim_close_bdev(&handle);
    return err ? BLK_STS_IOERR : BLK_STS_OK;
}
