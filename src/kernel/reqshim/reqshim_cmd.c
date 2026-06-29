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

    if (!out_entry || !out_phys_sector) {
        ssu_reqshim_warn_rl("translate invalid minor=%u logical_sector=%llu out_entry=%p out_phys=%p\n",
                            logical_minor,
                            (unsigned long long)logical_sector,
                            out_entry, out_phys_sector);
        return -EINVAL;
    }

    query.logical_minor = logical_minor;
    query.logical_sector = logical_sector;
    err = ssu_reqshim_map_query(&query);
    if (err) {
        ssu_reqshim_warn_rl("translate miss minor=%u logical_sector=%llu err=%d\n",
                            logical_minor,
                            (unsigned long long)logical_sector, err);
        return err;
    }

    if (logical_sector < query.entry.logical_sector) {
        ssu_reqshim_warn_rl(
            "translate invalid before-map minor=%u logical_sector=%llu map_start=%llu\n",
            logical_minor, (unsigned long long)logical_sector,
            (unsigned long long)query.entry.logical_sector);
        return -EINVAL;
    }

    delta = logical_sector - query.entry.logical_sector;
    if (delta >= query.entry.length_sectors ||
        query.entry.phys_sector > (~0ULL) - delta) {
        ssu_reqshim_warn_rl(
            "translate invalid range minor=%u logical_sector=%llu map_start=%llu map_len=%llu phys_sector=%llu delta=%llu\n",
            logical_minor, (unsigned long long)logical_sector,
            (unsigned long long)query.entry.logical_sector,
            (unsigned long long)query.entry.length_sectors,
            (unsigned long long)query.entry.phys_sector,
            (unsigned long long)delta);
        return -EINVAL;
    }

    *out_entry = query.entry;
    *out_phys_sector = query.entry.phys_sector + delta;
    ssu_reqshim_io(
        "translate hit minor=%u logical_sector=%llu map_start=%llu len=%llu phys_dev=%s base_phys=%llu out_phys=%llu delta=%llu\n",
        logical_minor, (unsigned long long)logical_sector,
        (unsigned long long)query.entry.logical_sector,
        (unsigned long long)query.entry.length_sectors,
        query.entry.phys_dev, (unsigned long long)query.entry.phys_sector,
        (unsigned long long)*out_phys_sector, (unsigned long long)delta);
    return 0;
}
