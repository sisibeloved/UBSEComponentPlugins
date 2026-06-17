#ifndef SSU_REQSHIM_INTERNAL_H
#define SSU_REQSHIM_INTERNAL_H

#include <linux/fs.h>

#include "reqshim_uapi.h"

#define SSU_REQSHIM_MAX_LOGDEVS 256U
#define SSU_REQSHIM_MAX_MAPS 1024U

long ssu_reqshim_ioctl(struct file *file, unsigned int cmd,
                       unsigned long arg);

int ssu_reqshim_logdev_create(const struct ssu_logdev_req *req);
int ssu_reqshim_logdev_destroy(const struct ssu_logdev_req *req);
bool ssu_reqshim_logdev_exists(__u32 minor);

int ssu_reqshim_map_add(const struct ssu_map_entry *entry);
int ssu_reqshim_map_del(const struct ssu_map_entry *entry);
int ssu_reqshim_map_query(struct ssu_map_query *query);
void ssu_reqshim_map_clear_minor(__u32 minor);

#endif
