#ifndef SSU_REQSHIM_INTERNAL_H
#define SSU_REQSHIM_INTERNAL_H

#include <linux/blk_types.h>
#include <linux/fs.h>
#include <linux/mm_types.h>

#include "reqshim_uapi.h"

#define SSU_REQSHIM_MAX_LOGDEVS 256U
#define SSU_REQSHIM_MAX_MAPS 1024U

long ssu_reqshim_ioctl(struct file *file, unsigned int cmd,
                       unsigned long arg);

int ssu_reqshim_blk_init(void);
void ssu_reqshim_blk_exit(void);

int ssu_reqshim_logdev_create(const struct ssu_logdev_req *req);
int ssu_reqshim_logdev_destroy(const struct ssu_logdev_req *req);
bool ssu_reqshim_logdev_exists(__u32 minor);

int ssu_reqshim_map_add(const struct ssu_map_entry *entry);
int ssu_reqshim_map_del(const struct ssu_map_entry *entry);
int ssu_reqshim_map_query(struct ssu_map_query *query);
void ssu_reqshim_map_clear_minor(__u32 minor);

int ssu_reqshim_translate_sector(__u32 logical_minor,
                                 __u64 logical_sector,
                                 struct ssu_map_entry *out_entry,
                                 __u64 *out_phys_sector);

enum ssu_reqshim_phys_backend {
    SSU_REQSHIM_PHYS_BACKEND_MOCK_BIO = 0,
    SSU_REQSHIM_PHYS_BACKEND_NVME_QUEUE = 1,
};

enum ssu_reqshim_phys_backend ssu_reqshim_phys_backend_for_entry(
    const struct ssu_map_entry *entry);

blk_status_t ssu_reqshim_phys_submit_bvec(const struct ssu_map_entry *entry,
                                          sector_t phys_sector,
                                          blk_opf_t opf,
                                          struct page *page,
                                          unsigned int len,
                                          unsigned int offset);

#endif
