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

    if (!entry || entry->length_sectors == 0 || entry->phys_dev[0] == '\0')
        return -EINVAL;

    if (!ssu_reqshim_logdev_exists(entry->logical_minor))
        return -ENOENT;

    mutex_lock(&map_lock);
    for (i = 0; i < SSU_REQSHIM_MAX_MAPS; i++) {
        if (maps[i].active &&
            maps[i].entry.logical_minor == entry->logical_minor &&
            maps[i].entry.logical_sector == entry->logical_sector) {
            mutex_unlock(&map_lock);
            return -EEXIST;
        }

        if (!maps[i].active && slot == NULL)
            slot = &maps[i];
    }

    if (slot == NULL) {
        mutex_unlock(&map_lock);
        return -ENOSPC;
    }

    slot->active = true;
    slot->entry = *entry;
    mutex_unlock(&map_lock);
    return 0;
}

int ssu_reqshim_map_del(const struct ssu_map_entry *entry)
{
    unsigned int i;

    if (!entry)
        return -EINVAL;

    mutex_lock(&map_lock);
    for (i = 0; i < SSU_REQSHIM_MAX_MAPS; i++) {
        if (maps[i].active &&
            maps[i].entry.logical_minor == entry->logical_minor &&
            maps[i].entry.logical_sector == entry->logical_sector) {
            memset(&maps[i], 0, sizeof(maps[i]));
            mutex_unlock(&map_lock);
            return 0;
        }
    }

    mutex_unlock(&map_lock);
    return -ENOENT;
}

int ssu_reqshim_map_query(struct ssu_map_query *query)
{
    unsigned int i;

    if (!query)
        return -EINVAL;

    mutex_lock(&map_lock);
    for (i = 0; i < SSU_REQSHIM_MAX_MAPS; i++) {
        if (maps[i].active && map_matches_query(&maps[i].entry, query)) {
            query->entry = maps[i].entry;
            mutex_unlock(&map_lock);
            return 0;
        }
    }

    mutex_unlock(&map_lock);
    return -ENOENT;
}

void ssu_reqshim_map_clear_minor(__u32 minor)
{
    unsigned int i;

    mutex_lock(&map_lock);
    for (i = 0; i < SSU_REQSHIM_MAX_MAPS; i++) {
        if (maps[i].active && maps[i].entry.logical_minor == minor)
            memset(&maps[i], 0, sizeof(maps[i]));
    }
    mutex_unlock(&map_lock);
}
