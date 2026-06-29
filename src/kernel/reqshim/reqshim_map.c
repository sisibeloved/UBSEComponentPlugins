#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include "reqshim_internal.h"

struct reqshim_map_slot {
    bool active;
    struct ssu_map_entry entry;
};

static DEFINE_MUTEX(map_lock);
static struct reqshim_map_slot maps[SSU_REQSHIM_MAX_MAPS];

static bool map_matches_query(const struct ssu_map_entry *entry,
                              const struct ssu_map_query *query)
{
    if (entry->logical_minor != query->logical_minor ||
        query->logical_sector < entry->logical_sector)
        return false;

    return query->logical_sector - entry->logical_sector <
           entry->length_sectors;
}

int ssu_reqshim_map_add(const struct ssu_map_entry *entry)
{
    struct reqshim_map_slot *slot = NULL;
    unsigned int i;

    if (!entry || entry->length_sectors == 0 || entry->phys_dev[0] == '\0') {
        ssu_reqshim_warn("map add invalid entry=%p\n", entry);
        return -EINVAL;
    }

    if (!ssu_reqshim_logdev_exists(entry->logical_minor)) {
        ssu_reqshim_warn(
            "map add missing logdev minor=%u logical_sector=%llu len=%llu phys_dev=%s nsid=%u phys_sector=%llu\n",
            entry->logical_minor,
            (unsigned long long)entry->logical_sector,
            (unsigned long long)entry->length_sectors,
            entry->phys_dev, entry->nsid,
            (unsigned long long)entry->phys_sector);
        return -ENOENT;
    }

    mutex_lock(&map_lock);
    for (i = 0; i < SSU_REQSHIM_MAX_MAPS; i++) {
        if (maps[i].active &&
            maps[i].entry.logical_minor == entry->logical_minor &&
            maps[i].entry.logical_sector == entry->logical_sector) {
            mutex_unlock(&map_lock);
            ssu_reqshim_warn(
                "map add exists minor=%u logical_sector=%llu phys_dev=%s\n",
                entry->logical_minor,
                (unsigned long long)entry->logical_sector,
                entry->phys_dev);
            return -EEXIST;
        }

        if (!maps[i].active && slot == NULL)
            slot = &maps[i];
    }

    if (slot == NULL) {
        mutex_unlock(&map_lock);
        ssu_reqshim_warn("map add no space minor=%u logical_sector=%llu\n",
                         entry->logical_minor,
                         (unsigned long long)entry->logical_sector);
        return -ENOSPC;
    }

    slot->active = true;
    slot->entry = *entry;
    mutex_unlock(&map_lock);
    ssu_reqshim_info(
        "map add minor=%u logical_sector=%llu len=%llu phys_dev=%s nsid=%u phys_sector=%llu\n",
        entry->logical_minor, (unsigned long long)entry->logical_sector,
        (unsigned long long)entry->length_sectors, entry->phys_dev,
        entry->nsid, (unsigned long long)entry->phys_sector);
    return 0;
}

int ssu_reqshim_map_del(const struct ssu_map_entry *entry)
{
    unsigned int i;

    if (!entry) {
        ssu_reqshim_warn("map del invalid entry=NULL\n");
        return -EINVAL;
    }

    mutex_lock(&map_lock);
    for (i = 0; i < SSU_REQSHIM_MAX_MAPS; i++) {
        if (maps[i].active &&
            maps[i].entry.logical_minor == entry->logical_minor &&
            maps[i].entry.logical_sector == entry->logical_sector) {
            struct ssu_map_entry old = maps[i].entry;

            memset(&maps[i], 0, sizeof(maps[i]));
            mutex_unlock(&map_lock);
            ssu_reqshim_info(
                "map del minor=%u logical_sector=%llu len=%llu phys_dev=%s nsid=%u phys_sector=%llu\n",
                old.logical_minor, (unsigned long long)old.logical_sector,
                (unsigned long long)old.length_sectors, old.phys_dev,
                old.nsid, (unsigned long long)old.phys_sector);
            return 0;
        }
    }

    mutex_unlock(&map_lock);
    ssu_reqshim_warn("map del missing minor=%u logical_sector=%llu\n",
                     entry->logical_minor,
                     (unsigned long long)entry->logical_sector);
    return -ENOENT;
}

int ssu_reqshim_map_query(struct ssu_map_query *query)
{
    unsigned int i;

    if (!query) {
        ssu_reqshim_warn_rl("map query invalid query=NULL\n");
        return -EINVAL;
    }

    mutex_lock(&map_lock);
    for (i = 0; i < SSU_REQSHIM_MAX_MAPS; i++) {
        if (maps[i].active && map_matches_query(&maps[i].entry, query)) {
            query->entry = maps[i].entry;
            mutex_unlock(&map_lock);
            ssu_reqshim_io(
                "map query hit minor=%u logical_sector=%llu map_start=%llu len=%llu phys_dev=%s phys_sector=%llu\n",
                query->logical_minor,
                (unsigned long long)query->logical_sector,
                (unsigned long long)query->entry.logical_sector,
                (unsigned long long)query->entry.length_sectors,
                query->entry.phys_dev,
                (unsigned long long)query->entry.phys_sector);
            return 0;
        }
    }

    mutex_unlock(&map_lock);
    ssu_reqshim_warn_rl("map query miss minor=%u logical_sector=%llu\n",
                        query->logical_minor,
                        (unsigned long long)query->logical_sector);
    return -ENOENT;
}

void ssu_reqshim_map_clear_minor(__u32 minor)
{
    unsigned int i;

    mutex_lock(&map_lock);
    for (i = 0; i < SSU_REQSHIM_MAX_MAPS; i++) {
        if (maps[i].active && maps[i].entry.logical_minor == minor) {
            ssu_reqshim_info(
                "map clear minor=%u logical_sector=%llu len=%llu phys_dev=%s nsid=%u phys_sector=%llu\n",
                minor, (unsigned long long)maps[i].entry.logical_sector,
                (unsigned long long)maps[i].entry.length_sectors,
                maps[i].entry.phys_dev, maps[i].entry.nsid,
                (unsigned long long)maps[i].entry.phys_sector);
            memset(&maps[i], 0, sizeof(maps[i]));
        }
    }
    mutex_unlock(&map_lock);
}
