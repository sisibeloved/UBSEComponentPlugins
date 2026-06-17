#include <linux/errno.h>
#include "reqshim_internal.h"

int ssu_reqshim_translate_sector(__u32 logical_minor,
                                 __u64 logical_sector,
                                 struct ssu_map_entry *out_entry,
                                 __u64 *out_phys_sector)
{
    struct ssu_map_query query;
    __u64 delta;
    int err;

    if (!out_entry || !out_phys_sector)
        return -EINVAL;

    query.logical_minor = logical_minor;
    query.logical_sector = logical_sector;
    err = ssu_reqshim_map_query(&query);
    if (err)
        return err;

    if (logical_sector < query.entry.logical_sector)
        return -EINVAL;

    delta = logical_sector - query.entry.logical_sector;
    if (delta >= query.entry.length_sectors ||
        query.entry.phys_sector > (~0ULL) - delta)
        return -EINVAL;

    *out_entry = query.entry;
    *out_phys_sector = query.entry.phys_sector + delta;
    return 0;
}
