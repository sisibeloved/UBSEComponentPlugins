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
    struct file *file;
#endif
};

static int ssu_reqshim_open_bdev(const char *path,
                                 struct ssu_reqshim_bdev_handle *handle)
{
    if (!path || !handle)
        return -EINVAL;

    memset(handle, 0, sizeof(*handle));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
    handle->file = bdev_file_open_by_path(path,
                                          BLK_OPEN_READ | BLK_OPEN_WRITE,
                                          NULL, NULL);
    if (IS_ERR(handle->file))
        return PTR_ERR(handle->file);

    handle->bdev = file_bdev(handle->file);
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
    fput(handle->file);
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

blk_status_t ssu_reqshim_phys_submit_bvec(const struct ssu_map_entry *entry,
                                          sector_t phys_sector,
                                          blk_opf_t opf,
                                          struct page *page,
                                          unsigned int len,
                                          unsigned int offset)
{
    struct ssu_reqshim_bdev_handle handle;
    struct bio *bio;
    int err;

    if (!entry || !page || len == 0)
        return BLK_STS_IOERR;

    if (ssu_reqshim_phys_backend_for_entry(entry) ==
        SSU_REQSHIM_PHYS_BACKEND_NVME_QUEUE) {
        /*
         * MVP keeps NVMe queue submission behind the same physical backend
         * boundary. Until the LBC INI queue contract is wired in, submit the
         * namespace through the normal block device queue.
         */
    }

    err = ssu_reqshim_open_bdev(entry->phys_dev, &handle);
    if (err)
        return BLK_STS_IOERR;

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
        ssu_reqshim_close_bdev(&handle);
        return BLK_STS_RESOURCE;
    }

    bio->bi_iter.bi_sector = phys_sector;
    if (bio_add_page(bio, page, len, offset) != len) {
        bio_put(bio);
        ssu_reqshim_close_bdev(&handle);
        return BLK_STS_RESOURCE;
    }

    err = submit_bio_wait(bio);
    bio_put(bio);
    ssu_reqshim_close_bdev(&handle);
    return err ? BLK_STS_IOERR : BLK_STS_OK;
}
