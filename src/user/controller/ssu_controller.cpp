#include "ssu_controller.h"

#include <stdio.h>
#include <string.h>

#define SSU_CONTROLLER_MAX_RESOURCES 128U
#define SSU_CONTROLLER_MAX_ALLOCATIONS 128U
#define SSU_CONTROLLER_MAX_RELEASED 128U
#define SSU_CONTROLLER_MAX_LOGDEVS 128U
#define SSU_CONTROLLER_MAX_HOSTS 32U
#define SSU_CONTROLLER_SECTOR_SIZE 512ULL
#define SSU_CONTROLLER_HOST_KEY_SIZE 256U

static ssu_resource_info_t pool[SSU_CONTROLLER_MAX_RESOURCES];
static uint32_t pool_count;
static uint64_t next_allocation_id;

typedef struct {
    int active;
    ssu_allocation_info_t info;
    char host_key[SSU_CONTROLLER_HOST_KEY_SIZE];
} controller_allocation_t;

typedef struct {
    int active;
    ssu_logdev_info_t info;
} controller_logdev_t;

static controller_allocation_t allocations[SSU_CONTROLLER_MAX_ALLOCATIONS];
static controller_logdev_t logdevs[SSU_CONTROLLER_MAX_LOGDEVS];
static char released_ids[SSU_CONTROLLER_MAX_RELEASED][64];
static uint32_t released_count;

static void copy_cstr(char *dst, size_t n, const char *src)
{
    if (n == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, n, "%s", src);
}

static uint32_t active_allocation_count(void)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = 0; i < SSU_CONTROLLER_MAX_ALLOCATIONS; i++) {
        if (allocations[i].active) {
            count++;
        }
    }

    return count;
}

static uint32_t active_logdev_count(void)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = 0; i < SSU_CONTROLLER_MAX_LOGDEVS; i++) {
        if (logdevs[i].active) {
            count++;
        }
    }

    return count;
}

static int resource_is_online(const ssu_resource_info_t *resource)
{
    return resource != NULL && strcmp(resource->state, "ONLINE") == 0;
}

static uint32_t online_pool_indexes(uint32_t *indexes)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = 0; i < pool_count; i++) {
        if (resource_is_online(&pool[i])) {
            indexes[count++] = i;
        }
    }

    return count;
}

static int pool_index_already_selected(const uint32_t *indexes,
                                       uint32_t count, uint32_t index)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (indexes[i] == index) {
            return 1;
        }
    }

    return 0;
}

static int pool_index_has_capacity(uint32_t pool_index, uint64_t length)
{
    if (pool_index >= pool_count ||
        pool[pool_index].used_bytes > pool[pool_index].total_bytes ||
        pool[pool_index].used_bytes % SSU_CONTROLLER_SECTOR_SIZE != 0) {
        return 0;
    }

    return length <= pool[pool_index].total_bytes -
                         pool[pool_index].used_bytes;
}

static ssu_err_t select_pool_indexes(uint32_t *online_indexes,
                                     uint32_t online_count,
                                     uint32_t extent_count,
                                     uint64_t base_length,
                                     uint64_t remainder,
                                     uint32_t *selected_indexes)
{
    uint32_t i;

    for (i = 0; i < extent_count; i++) {
        uint64_t length = base_length + (i < remainder ? 1ULL : 0ULL);
        uint32_t j;
        int found = 0;

        for (j = 0; j < online_count; j++) {
            uint32_t pool_index = online_indexes[j];

            if (pool_index_already_selected(selected_indexes, i,
                                            pool_index)) {
                continue;
            }

            if (!pool_index_has_capacity(pool_index, length)) {
                continue;
            }

            selected_indexes[i] = pool_index;
            found = 1;
            break;
        }

        if (!found) {
            return SSU_ERR_NO_RESOURCE;
        }
    }

    return SSU_OK;
}

static uint32_t active_allocation_extent_count(const char *allocate_id)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = 0; i < SSU_CONTROLLER_MAX_ALLOCATIONS; i++) {
        if (allocations[i].active &&
            strcmp(allocations[i].info.allocate_id, allocate_id) == 0) {
            count++;
        }
    }

    return count;
}

static uint32_t free_allocation_slot_count(void)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = 0; i < SSU_CONTROLLER_MAX_ALLOCATIONS; i++) {
        if (!allocations[i].active) {
            count++;
        }
    }

    return count;
}

static uint32_t free_logdev_slot_count(void)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = 0; i < SSU_CONTROLLER_MAX_LOGDEVS; i++) {
        if (!logdevs[i].active) {
            count++;
        }
    }

    return count;
}

static int is_empty_string(const char *s)
{
    return s == NULL || s[0] == '\0';
}

static int is_disk_name_char(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.' || c == ':';
}

static int allocation_name_is_valid(const char *name)
{
    size_t i;
    size_t len;

    if (is_empty_string(name)) {
        return 0;
    }

    len = strlen(name);
    if (len > SSU_API_MAX_DISK_NAME_LEN) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        if (!is_disk_name_char(name[i])) {
            return 0;
        }
    }

    return 1;
}

static int nullable_cstr_equal(const char *a, const char *b)
{
    const char *left = a == NULL ? "" : a;
    const char *right = b == NULL ? "" : b;

    return strcmp(left, right) == 0;
}

static int append_host_key(char *dst, size_t n, const char *host,
                           int needs_comma)
{
    size_t used;
    int written;

    if (dst == NULL || n == 0 || is_empty_string(host) ||
        strchr(host, ',') != NULL) {
        return 0;
    }

    used = strlen(dst);
    if (used >= n) {
        return 0;
    }

    written = snprintf(dst + used, n - used, "%s%s",
                       needs_comma ? "," : "", host);
    return written >= 0 && (size_t)written < n - used;
}

static int build_host_key(const char *const *hosts, uint32_t host_count,
                          char *out, size_t n)
{
    const char *ordered[SSU_CONTROLLER_MAX_HOSTS];
    uint32_t i;
    uint32_t j;

    if (out == NULL || n == 0) {
        return 0;
    }

    out[0] = '\0';
    if (host_count == 0) {
        return 1;
    }

    if (hosts == NULL) {
        return 0;
    }

    if (host_count > SSU_CONTROLLER_MAX_HOSTS) {
        return 0;
    }

    for (i = 0; i < host_count; i++) {
        if (is_empty_string(hosts[i]) || strchr(hosts[i], ',') != NULL) {
            return 0;
        }
        ordered[i] = hosts[i];
    }

    for (i = 1; i < host_count; i++) {
        const char *host = ordered[i];

        j = i;
        while (j > 0 && strcmp(ordered[j - 1], host) > 0) {
            ordered[j] = ordered[j - 1];
            j--;
        }
        ordered[j] = host;
    }

    for (i = 0; i < host_count; i++) {
        if (!append_host_key(out, n, ordered[i], i > 0)) {
            return 0;
        }
    }

    return 1;
}

static controller_logdev_t *find_logdev_by_dev(const char *logical_dev)
{
    uint32_t i;

    for (i = 0; i < SSU_CONTROLLER_MAX_LOGDEVS; i++) {
        if (logdevs[i].active &&
            strcmp(logdevs[i].info.logical_dev, logical_dev) == 0) {
            return &logdevs[i];
        }
    }

    return NULL;
}

static uint32_t find_allocation_rows_by_id(
    const char *allocate_id,
    controller_allocation_t **out,
    uint32_t max_count)
{
    uint32_t count = 0;
    uint32_t i;

    if (is_empty_string(allocate_id)) {
        return 0;
    }

    for (i = 0; i < SSU_CONTROLLER_MAX_ALLOCATIONS; i++) {
        if (!allocations[i].active ||
            strcmp(allocations[i].info.allocate_id, allocate_id) != 0) {
            continue;
        }

        if (out != NULL && count < max_count) {
            out[count] = &allocations[i];
        }
        count++;
    }

    return count;
}

static uint32_t find_allocation_rows_by_disk_name(
    const char *disk_name,
    controller_allocation_t **out,
    uint32_t max_count)
{
    uint32_t count = 0;
    uint32_t i;

    if (is_empty_string(disk_name)) {
        return 0;
    }

    for (i = 0; i < SSU_CONTROLLER_MAX_ALLOCATIONS; i++) {
        if (!allocations[i].active ||
            strcmp(allocations[i].info.disk_name, disk_name) != 0) {
            continue;
        }

        if (out != NULL && count < max_count) {
            out[count] = &allocations[i];
        }
        count++;
    }

    return count;
}

static int allocation_id_is_active(const char *allocate_id)
{
    return find_allocation_rows_by_id(allocate_id, NULL, 0) > 0;
}

static int disk_name_is_active(const char *disk_name)
{
    return find_allocation_rows_by_disk_name(disk_name, NULL, 0) > 0;
}

static int allocation_has_logdev(const char *allocate_id)
{
    uint32_t i;

    for (i = 0; i < SSU_CONTROLLER_MAX_LOGDEVS; i++) {
        if (logdevs[i].active &&
            strcmp(logdevs[i].info.allocate_id, allocate_id) == 0) {
            return 1;
        }
    }

    return 0;
}

static void set_allocation_state(const char *allocate_id, const char *state)
{
    uint32_t i;

    for (i = 0; i < SSU_CONTROLLER_MAX_ALLOCATIONS; i++) {
        if (allocations[i].active &&
            strcmp(allocations[i].info.allocate_id, allocate_id) == 0) {
            copy_cstr(allocations[i].info.state,
                      sizeof(allocations[i].info.state), state);
        }
    }
}

static controller_allocation_t *next_free_allocation_slot(void)
{
    uint32_t i;

    for (i = 0; i < SSU_CONTROLLER_MAX_ALLOCATIONS; i++) {
        if (!allocations[i].active) {
            return &allocations[i];
        }
    }

    return NULL;
}

static int released_id_exists(const char *allocate_id)
{
    uint32_t i;

    for (i = 0; i < released_count; i++) {
        if (strcmp(released_ids[i], allocate_id) == 0) {
            return 1;
        }
    }

    return 0;
}

static void remember_released_id(const char *allocate_id)
{
    if (released_id_exists(allocate_id) ||
        released_count >= SSU_CONTROLLER_MAX_RELEASED) {
        return;
    }

    copy_cstr(released_ids[released_count], sizeof(released_ids[0]),
              allocate_id);
    released_count++;
}

static void next_auto_allocate_id(char *out, size_t n)
{
    for (;;) {
        snprintf(out, n, "alloc-%llu",
                 (unsigned long long)next_allocation_id);
        if (!allocation_id_is_active(out) && !released_id_exists(out)) {
            return;
        }
        next_allocation_id++;
    }
}

static int find_resource_index(const ssu_resource_info_t *resources,
                               uint32_t count, const char *ssu_id)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(resources[i].ssu_id, ssu_id) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static void refresh_resource_state(const ssu_plugin_ops_t *plugin,
                                   ssu_resource_info_t *resource)
{
    char state[sizeof(resource->state)];

    if (plugin->health_check == NULL) {
        return;
    }

    memset(state, 0, sizeof(state));
    if (plugin->health_check(resource->ssu_id, state, sizeof(state)) ==
        SSU_OK) {
        copy_cstr(resource->state, sizeof(resource->state), state);
        return;
    }

    copy_cstr(resource->state, sizeof(resource->state), "OFFLINE");
}

ssu_err_t ssu_controller_refresh_pool(const ssu_plugin_ops_t *plugin)
{
    ssu_resource_info_t discovered[SSU_CONTROLLER_MAX_RESOURCES];
    ssu_resource_info_t next_pool[SSU_CONTROLLER_MAX_RESOURCES];
    uint32_t count = SSU_CONTROLLER_MAX_RESOURCES;
    uint32_t next_count;
    uint32_t i;
    ssu_err_t err;

    if (plugin == NULL || plugin->discover == NULL) {
        return SSU_ERR_INVALID;
    }

    memset(discovered, 0, sizeof(discovered));
    err = plugin->discover(discovered, &count);
    if (err != SSU_OK) {
        return err;
    }

    if (count > SSU_CONTROLLER_MAX_RESOURCES) {
        return SSU_ERR_INTERNAL;
    }

    memset(next_pool, 0, sizeof(next_pool));
    for (i = 0; i < count; i++) {
        int old_index;

        next_pool[i] = discovered[i];
        old_index = find_resource_index(pool, pool_count,
                                        discovered[i].ssu_id);
        if (old_index >= 0) {
            next_pool[i].used_bytes = pool[old_index].used_bytes;
        }

        refresh_resource_state(plugin, &next_pool[i]);
    }
    next_count = count;

    for (i = 0; i < pool_count; i++) {
        if (find_resource_index(next_pool, next_count, pool[i].ssu_id) >= 0) {
            continue;
        }

        if (next_count >= SSU_CONTROLLER_MAX_RESOURCES) {
            return SSU_ERR_INTERNAL;
        }

        next_pool[next_count] = pool[i];
        refresh_resource_state(plugin, &next_pool[next_count]);
        next_count++;
    }

    memcpy(pool, next_pool, sizeof(pool[0]) * next_count);
    pool_count = next_count;
    return SSU_OK;
}

ssu_err_t ssu_controller_query_pool(ssu_resource_info_t *out,
                                    uint32_t *inout_count)
{
    if (inout_count == NULL) {
        return SSU_ERR_INVALID;
    }

    if (out == NULL || *inout_count < pool_count) {
        *inout_count = pool_count;
        return pool_count == 0 ? SSU_OK : SSU_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(out, pool, sizeof(pool[0]) * pool_count);
    *inout_count = pool_count;
    return SSU_OK;
}

static ssu_err_t copy_existing_allocation(
    const ssu_alloc_req_t *req,
    const char *disk_name,
    ssu_alloc_result_t *out,
    ssu_alloc_extent_t *out_extents,
    uint32_t *inout_extent_count)
{
    controller_allocation_t *rows[SSU_CONTROLLER_MAX_ALLOCATIONS];
    char host_key[SSU_CONTROLLER_HOST_KEY_SIZE];
    uint32_t extent_count;
    uint64_t total_size = 0;
    uint32_t i;

    memset(rows, 0, sizeof(rows));
    extent_count = find_allocation_rows_by_disk_name(
        disk_name, rows, SSU_CONTROLLER_MAX_ALLOCATIONS);
    if (extent_count == 0) {
        return SSU_ERR_NOT_FOUND;
    }

    for (i = 0; i < extent_count; i++) {
        total_size += rows[i]->info.length;
    }

    if (!build_host_key(req->host_ids, req->host_count, host_key,
                        sizeof(host_key))) {
        return SSU_ERR_INVALID;
    }

    if (total_size != req->size_bytes ||
        (req->physical_disk_count > 0 &&
         req->physical_disk_count != extent_count) ||
        rows[0]->info.policy != req->reliability ||
        rows[0]->info.share_type != req->share_type ||
        !nullable_cstr_equal(rows[0]->info.tenant, req->tenant) ||
        strcmp(rows[0]->host_key, host_key) != 0) {
        return SSU_ERR_NS_EXISTS;
    }

    if (out_extents == NULL || *inout_extent_count < extent_count) {
        *inout_extent_count = extent_count;
        return SSU_ERR_BUFFER_TOO_SMALL;
    }

    memset(out, 0, sizeof(*out));
    memset(out_extents, 0, sizeof(out_extents[0]) * extent_count);
    for (i = 0; i < extent_count; i++) {
        int pool_index = find_resource_index(pool, pool_count,
                                             rows[i]->info.ssu_id);

        copy_cstr(out_extents[i].ssu_id, sizeof(out_extents[i].ssu_id),
                  rows[i]->info.ssu_id);
        if (pool_index >= 0) {
            copy_cstr(out_extents[i].host_id, sizeof(out_extents[i].host_id),
                      pool[pool_index].host_id);
        }
        copy_cstr(out_extents[i].ns_id, sizeof(out_extents[i].ns_id),
                  rows[i]->info.ns_id);
        out_extents[i].logical_offset = rows[i]->info.logical_offset;
        out_extents[i].length = rows[i]->info.length;
    }

    copy_cstr(out->allocate_id, sizeof(out->allocate_id),
              rows[0]->info.allocate_id);
    out->logical_size_bytes = total_size;
    out->extent_count = extent_count;
    *inout_extent_count = extent_count;
    return SSU_OK;
}

ssu_err_t ssu_controller_alloc(const ssu_plugin_ops_t *plugin,
                               const ssu_alloc_req_t *req,
                               ssu_alloc_result_t *out,
                               ssu_alloc_extent_t *out_extents,
                               uint32_t *inout_extent_count)
{
    controller_allocation_t *created_slots[SSU_CONTROLLER_MAX_ALLOCATIONS];
    ssu_extent_create_req_t extent_req;
    uint32_t pool_indexes[SSU_CONTROLLER_MAX_RESOURCES];
    uint32_t selected_pool_indexes[SSU_CONTROLLER_MAX_RESOURCES];
    char allocate_id[64];
    char ns_id[32];
    uint32_t extent_count;
    uint32_t online_count;
    uint64_t logical_offset = 0;
    uint64_t base_length;
    uint64_t remainder;
    ssu_err_t err;
    uint32_t i;
    uint32_t created_count = 0;
    int named_allocation;
    char host_key[SSU_CONTROLLER_HOST_KEY_SIZE];

    if (plugin == NULL || plugin->create_ns == NULL || req == NULL ||
        out == NULL || inout_extent_count == NULL) {
        return SSU_ERR_INVALID;
    }

    if (req->reliability != SSU_RELIABILITY_STRIPE) {
        return SSU_ERR_UNSUPPORTED;
    }

    if (req->size_bytes == 0) {
        return SSU_ERR_INVALID;
    }

    if (!build_host_key(req->host_ids, req->host_count, host_key,
                        sizeof(host_key))) {
        return SSU_ERR_INVALID;
    }

    named_allocation = !is_empty_string(req->disk_name);
    if (named_allocation && !allocation_name_is_valid(req->disk_name)) {
        return SSU_ERR_INVALID;
    }

    if (named_allocation) {
        if (disk_name_is_active(req->disk_name)) {
            return copy_existing_allocation(req, req->disk_name, out,
                                            out_extents,
                                            inout_extent_count);
        }
    }

    next_auto_allocate_id(allocate_id, sizeof(allocate_id));

    online_count = online_pool_indexes(pool_indexes);
    if (online_count == 0) {
        return SSU_ERR_NO_RESOURCE;
    }

    extent_count = online_count;

    if (req->physical_disk_count > 0) {
        if (req->physical_disk_count > online_count) {
            return SSU_ERR_NO_RESOURCE;
        }
        extent_count = req->physical_disk_count;
    }

    if ((uint64_t)extent_count > req->size_bytes) {
        extent_count = (uint32_t)req->size_bytes;
    }

    if (out_extents == NULL || *inout_extent_count < extent_count) {
        *inout_extent_count = extent_count;
        return SSU_ERR_BUFFER_TOO_SMALL;
    }

    if (free_allocation_slot_count() < extent_count) {
        return SSU_ERR_NO_RESOURCE;
    }

    memset(out, 0, sizeof(*out));
    memset(out_extents, 0, sizeof(out_extents[0]) * extent_count);
    memset(created_slots, 0, sizeof(created_slots));
    memset(selected_pool_indexes, 0, sizeof(selected_pool_indexes));

    base_length = req->size_bytes / extent_count;
    remainder = req->size_bytes % extent_count;
    err = select_pool_indexes(pool_indexes, online_count, extent_count,
                              base_length, remainder, selected_pool_indexes);
    if (err != SSU_OK) {
        return err;
    }

    for (i = 0; i < extent_count; i++) {
        controller_allocation_t *slot = next_free_allocation_slot();
        uint32_t pool_index = selected_pool_indexes[i];
        uint64_t length = base_length + (i < remainder ? 1ULL : 0ULL);
        uint64_t phys_sector = 0;

        if (slot == NULL) {
            err = SSU_ERR_NO_RESOURCE;
            goto rollback;
        }
        if (pool[pool_index].used_bytes > pool[pool_index].total_bytes ||
            length > pool[pool_index].total_bytes -
                         pool[pool_index].used_bytes) {
            err = SSU_ERR_NO_RESOURCE;
            goto rollback;
        }
        if (pool[pool_index].used_bytes % SSU_CONTROLLER_SECTOR_SIZE != 0) {
            err = SSU_ERR_INVALID;
            goto rollback;
        }

        memset(&extent_req, 0, sizeof(extent_req));
        extent_req.allocate_id = allocate_id;
        extent_req.ssu_id = pool[pool_index].ssu_id;
        extent_req.logical_offset = logical_offset;
        extent_req.length = length;
        extent_req.phys_offset_hint =
            pool[pool_index].used_bytes / SSU_CONTROLLER_SECTOR_SIZE;
        extent_req.policy = req->reliability;
        extent_req.role_index = i;
        extent_req.tenant = req->tenant;

        memset(ns_id, 0, sizeof(ns_id));
        err = plugin->create_ns(&extent_req, ns_id, sizeof(ns_id),
                                &phys_sector);
        if (err != SSU_OK) {
            goto rollback;
        }

        memset(slot, 0, sizeof(*slot));
        slot->active = 1;
        copy_cstr(slot->info.allocate_id, sizeof(slot->info.allocate_id),
                  allocate_id);
        copy_cstr(slot->info.disk_name, sizeof(slot->info.disk_name),
                  req->disk_name);
        copy_cstr(slot->info.tenant, sizeof(slot->info.tenant), req->tenant);
        copy_cstr(slot->host_key, sizeof(slot->host_key), host_key);
        slot->info.policy = req->reliability;
        slot->info.share_type = req->share_type;
        copy_cstr(slot->info.state, sizeof(slot->info.state), "ACTIVE");
        copy_cstr(slot->info.ssu_id, sizeof(slot->info.ssu_id),
                  pool[pool_index].ssu_id);
        copy_cstr(slot->info.ns_id, sizeof(slot->info.ns_id), ns_id);
        slot->info.logical_offset = logical_offset;
        slot->info.length = length;
        slot->info.phys_sector = phys_sector;
        slot->info.role_index = i;
        created_slots[created_count++] = slot;

        copy_cstr(out_extents[i].ssu_id, sizeof(out_extents[i].ssu_id),
                  pool[pool_index].ssu_id);
        copy_cstr(out_extents[i].host_id, sizeof(out_extents[i].host_id),
                  pool[pool_index].host_id);
        copy_cstr(out_extents[i].ns_id, sizeof(out_extents[i].ns_id),
                  ns_id);
        out_extents[i].logical_offset = logical_offset;
        out_extents[i].length = length;

        pool[pool_index].used_bytes += length;
        logical_offset += length;
    }

    copy_cstr(out->allocate_id, sizeof(out->allocate_id), allocate_id);
    out->logical_size_bytes = req->size_bytes;
    out->extent_count = extent_count;
    *inout_extent_count = extent_count;
    next_allocation_id++;
    return SSU_OK;

rollback:
    while (created_count > 0) {
        controller_allocation_t *slot = created_slots[created_count - 1];
        int pool_index = find_resource_index(pool, pool_count,
                                             slot->info.ssu_id);

        if (plugin->delete_ns != NULL) {
            plugin->delete_ns(slot->info.ssu_id, slot->info.ns_id);
        }
        if (pool_index >= 0 &&
            pool[pool_index].used_bytes >= slot->info.length) {
            pool[pool_index].used_bytes -= slot->info.length;
        }
        memset(slot, 0, sizeof(*slot));
        created_count--;
    }

    memset(out, 0, sizeof(*out));
    memset(out_extents, 0, sizeof(out_extents[0]) * extent_count);
    return err;
}

ssu_err_t ssu_controller_release(const ssu_plugin_ops_t *plugin,
                                 const char *allocate_id)
{
    uint32_t i;
    int found = 0;

    if (plugin == NULL || plugin->delete_ns == NULL || allocate_id == NULL ||
        allocate_id[0] == '\0') {
        return SSU_ERR_INVALID;
    }

    if (allocation_has_logdev(allocate_id)) {
        return SSU_ERR_BUSY;
    }

    for (i = 0; i < SSU_CONTROLLER_MAX_ALLOCATIONS; i++) {
        if (allocations[i].active &&
            strcmp(allocations[i].info.allocate_id, allocate_id) == 0) {
            int pool_index;
            ssu_err_t err = plugin->delete_ns(allocations[i].info.ssu_id,
                                              allocations[i].info.ns_id);
            if (err != SSU_OK) {
                return err;
            }

            pool_index = find_resource_index(pool, pool_count,
                                             allocations[i].info.ssu_id);
            if (pool_index >= 0 &&
                pool[pool_index].used_bytes >= allocations[i].info.length) {
                pool[pool_index].used_bytes -= allocations[i].info.length;
            }
            allocations[i].active = 0;
            found = 1;
        }
    }

    if (found) {
        remember_released_id(allocate_id);
        return SSU_OK;
    }

    return released_id_exists(allocate_id) ? SSU_OK : SSU_ERR_NOT_FOUND;
}

ssu_err_t ssu_controller_query_allocations(ssu_allocation_info_t *out,
                                           uint32_t *inout_count)
{
    uint32_t needed;
    uint32_t written = 0;
    uint32_t i;

    if (inout_count == NULL) {
        return SSU_ERR_INVALID;
    }

    needed = active_allocation_count();
    if (out == NULL || *inout_count < needed) {
        *inout_count = needed;
        return needed == 0 ? SSU_OK : SSU_ERR_BUFFER_TOO_SMALL;
    }

    for (i = 0; i < SSU_CONTROLLER_MAX_ALLOCATIONS; i++) {
        if (allocations[i].active) {
            out[written] = allocations[i].info;
            written++;
        }
    }

    *inout_count = written;
    return SSU_OK;
}

ssu_err_t ssu_controller_mount(const ssu_plugin_ops_t *plugin,
                               const ssu_mount_req_t *req)
{
    controller_allocation_t *allocation = NULL;
    controller_allocation_t *allocation_rows[SSU_CONTROLLER_MAX_ALLOCATIONS];
    controller_logdev_t *logdev_slots[SSU_CONTROLLER_MAX_LOGDEVS];
    char phys_devs[SSU_CONTROLLER_MAX_LOGDEVS][64];
    uint32_t extent_count = 0;
    ssu_err_t err;
    uint32_t i;

    if (plugin == NULL || plugin->mount == NULL || plugin->connect == NULL ||
        req == NULL || is_empty_string(req->allocate_id) ||
        is_empty_string(req->host_id) ||
        is_empty_string(req->logical_dev)) {
        return SSU_ERR_INVALID;
    }

    memset(allocation_rows, 0, sizeof(allocation_rows));
    memset(logdev_slots, 0, sizeof(logdev_slots));
    memset(phys_devs, 0, sizeof(phys_devs));
    for (i = 0; i < SSU_CONTROLLER_MAX_ALLOCATIONS; i++) {
        if (allocations[i].active &&
            strcmp(allocations[i].info.allocate_id, req->allocate_id) == 0) {
            if (allocation == NULL) {
                allocation = &allocations[i];
            }
            allocation_rows[extent_count++] = &allocations[i];
        }
    }

    if (extent_count == 0) {
        return SSU_ERR_NOT_FOUND;
    }

    if (find_logdev_by_dev(req->logical_dev) != NULL) {
        return SSU_ERR_BUSY;
    }

    if (allocation->info.share_type != SSU_SHARE_SHARED &&
        allocation_has_logdev(req->allocate_id)) {
        return SSU_ERR_BUSY;
    }

    if (free_logdev_slot_count() < extent_count) {
        return SSU_ERR_NO_RESOURCE;
    }

    for (i = 0; i < SSU_CONTROLLER_MAX_LOGDEVS && extent_count > 0; i++) {
        uint32_t slot_index;

        if (logdevs[i].active) {
            continue;
        }

        for (slot_index = 0; slot_index < extent_count; slot_index++) {
            if (logdev_slots[slot_index] == NULL) {
                logdev_slots[slot_index] = &logdevs[i];
                break;
            }
        }
    }

    for (i = 0; i < extent_count; i++) {
        if (logdev_slots[i] == NULL) {
            return SSU_ERR_NO_RESOURCE;
        }

        err = plugin->connect(allocation_rows[i]->info.ssu_id, phys_devs[i],
                              sizeof(phys_devs[i]));
        if (err != SSU_OK) {
            return err;
        }
    }

    err = plugin->mount(req->allocate_id, req->host_id, req->logical_dev);
    if (err != SSU_OK) {
        return err;
    }

    for (i = 0; i < extent_count; i++) {
        controller_logdev_t *slot = logdev_slots[i];

        memset(slot, 0, sizeof(*slot));
        slot->active = 1;
        copy_cstr(slot->info.logical_dev, sizeof(slot->info.logical_dev),
                  req->logical_dev);
        copy_cstr(slot->info.host_id, sizeof(slot->info.host_id),
                  req->host_id);
        copy_cstr(slot->info.allocate_id, sizeof(slot->info.allocate_id),
                  req->allocate_id);
        slot->info.logical_offset = allocation_rows[i]->info.logical_offset;
        slot->info.length = allocation_rows[i]->info.length;
        copy_cstr(slot->info.phys_dev, sizeof(slot->info.phys_dev),
                  phys_devs[i]);
        copy_cstr(slot->info.ns_id, sizeof(slot->info.ns_id),
                  allocation_rows[i]->info.ns_id);
        slot->info.phys_sector = allocation_rows[i]->info.phys_sector;
    }
    set_allocation_state(req->allocate_id, "MOUNTED");

    return SSU_OK;
}

ssu_err_t ssu_controller_unmount(const ssu_plugin_ops_t *plugin,
                                 const char *logical_dev)
{
    controller_logdev_t *logdev;
    char allocate_id[64];
    ssu_err_t err;
    uint32_t i;

    if (plugin == NULL || plugin->unmount == NULL ||
        is_empty_string(logical_dev)) {
        return SSU_ERR_INVALID;
    }

    logdev = find_logdev_by_dev(logical_dev);
    if (logdev == NULL) {
        return plugin->unmount(logical_dev);
    }

    copy_cstr(allocate_id, sizeof(allocate_id), logdev->info.allocate_id);
    err = plugin->unmount(logical_dev);
    if (err != SSU_OK) {
        return err;
    }

    for (i = 0; i < SSU_CONTROLLER_MAX_LOGDEVS; i++) {
        if (logdevs[i].active &&
            strcmp(logdevs[i].info.logical_dev, logical_dev) == 0) {
            memset(&logdevs[i], 0, sizeof(logdevs[i]));
        }
    }

    if (active_allocation_extent_count(allocate_id) > 0) {
        const char *state = allocation_has_logdev(allocate_id) ?
                            "MOUNTED" : "ACTIVE";

        set_allocation_state(allocate_id, state);
    }

    return SSU_OK;
}

ssu_err_t ssu_controller_query_logdevs(ssu_logdev_info_t *out,
                                       uint32_t *inout_count)
{
    uint32_t needed;
    uint32_t written = 0;
    uint32_t i;

    if (inout_count == NULL) {
        return SSU_ERR_INVALID;
    }

    needed = active_logdev_count();
    if (out == NULL || *inout_count < needed) {
        *inout_count = needed;
        return needed == 0 ? SSU_OK : SSU_ERR_BUFFER_TOO_SMALL;
    }

    for (i = 0; i < SSU_CONTROLLER_MAX_LOGDEVS; i++) {
        if (logdevs[i].active) {
            out[written] = logdevs[i].info;
            written++;
        }
    }

    *inout_count = written;
    return SSU_OK;
}
