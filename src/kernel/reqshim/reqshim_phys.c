#include <linux/string.h>

#include "reqshim_internal.h"

enum ssu_reqshim_phys_backend ssu_reqshim_phys_backend_for_entry(
    const struct ssu_map_entry *entry)
{
    if (!entry)
        return SSU_REQSHIM_PHYS_BACKEND_MOCK_BIO;

    if (strncmp(entry->phys_dev, "/dev/nvme", 9) == 0)
        return SSU_REQSHIM_PHYS_BACKEND_NVME_QUEUE;

    return SSU_REQSHIM_PHYS_BACKEND_MOCK_BIO;
}
