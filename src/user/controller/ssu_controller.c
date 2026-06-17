#include "ssu_controller.h"

#include <stdio.h>
#include <string.h>

#define SSU_CONTROLLER_MAX_RESOURCES 128U
#define SSU_CONTROLLER_MAX_ALLOCATIONS 128U
#define SSU_CONTROLLER_MAX_RELEASED 128U

static ssu_resource_info_t pool[SSU_CONTROLLER_MAX_RESOURCES];
static uint32_t pool_count;
static uint64_t next_allocation_id;

typedef struct {
    int active;
    ssu_allocation_info_t info;
} controller_allocation_t;

static controller_allocation_t allocations[SSU_CONTROLLER_MAX_ALLOCATIONS];
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

ssu_err_t ssu_controller_alloc(const ssu_plugin_ops_t *plugin,
                               const ssu_alloc_req_t *req,
                               ssu_alloc_result_t *out,
                               ssu_alloc_extent_t *out_extents,
                               uint32_t *inout_extent_count)
{
    controller_allocation_t *slot = NULL;
    ssu_extent_create_req_t extent_req;
    char allocate_id[64];
    char ns_id[32];
    uint64_t phys_sector = 0;
    ssu_err_t err;
    uint32_t i;

    if (plugin == NULL || plugin->create_ns == NULL || req == NULL ||
        out == NULL || inout_extent_count == NULL) {
        return SSU_ERR_INVALID;
    }

    if (req->reliability != SSU_RELIABILITY_STRIPE) {
        return SSU_ERR_UNSUPPORTED;
    }

    if (pool_count == 0) {
        return SSU_ERR_NO_RESOURCE;
    }

    if (out_extents == NULL || *inout_extent_count < 1) {
        *inout_extent_count = 1;
        return SSU_ERR_BUFFER_TOO_SMALL;
    }

    for (i = 0; i < SSU_CONTROLLER_MAX_ALLOCATIONS; i++) {
        if (!allocations[i].active) {
            slot = &allocations[i];
            break;
        }
    }

    if (slot == NULL) {
        return SSU_ERR_NO_RESOURCE;
    }

    snprintf(allocate_id, sizeof(allocate_id), "alloc-%llu",
             (unsigned long long)next_allocation_id);

    memset(&extent_req, 0, sizeof(extent_req));
    extent_req.allocate_id = allocate_id;
    extent_req.ssu_id = pool[0].ssu_id;
    extent_req.logical_offset = 0;
    extent_req.length = req->size_bytes;
    extent_req.phys_offset_hint = 0;
    extent_req.policy = req->reliability;
    extent_req.role_index = 0;
    extent_req.tenant = req->tenant;

    memset(ns_id, 0, sizeof(ns_id));
    err = plugin->create_ns(&extent_req, ns_id, sizeof(ns_id), &phys_sector);
    if (err != SSU_OK) {
        return err;
    }

    memset(slot, 0, sizeof(*slot));
    slot->active = 1;
    copy_cstr(slot->info.allocate_id, sizeof(slot->info.allocate_id),
              allocate_id);
    copy_cstr(slot->info.tenant, sizeof(slot->info.tenant), req->tenant);
    slot->info.policy = req->reliability;
    slot->info.share_type = req->share_type;
    copy_cstr(slot->info.state, sizeof(slot->info.state), "ACTIVE");
    copy_cstr(slot->info.ssu_id, sizeof(slot->info.ssu_id), pool[0].ssu_id);
    copy_cstr(slot->info.ns_id, sizeof(slot->info.ns_id), ns_id);
    slot->info.logical_offset = 0;
    slot->info.length = req->size_bytes;
    slot->info.phys_sector = phys_sector;
    slot->info.role_index = 0;

    memset(out, 0, sizeof(*out));
    copy_cstr(out->allocate_id, sizeof(out->allocate_id), allocate_id);
    out->logical_size_bytes = req->size_bytes;
    out->extent_count = 1;

    memset(out_extents, 0, sizeof(out_extents[0]));
    copy_cstr(out_extents[0].ssu_id, sizeof(out_extents[0].ssu_id),
              pool[0].ssu_id);
    copy_cstr(out_extents[0].host_id, sizeof(out_extents[0].host_id),
              pool[0].host_id);
    copy_cstr(out_extents[0].ns_id, sizeof(out_extents[0].ns_id), ns_id);
    out_extents[0].logical_offset = 0;
    out_extents[0].length = req->size_bytes;
    *inout_extent_count = 1;

    pool[0].used_bytes += req->size_bytes;
    next_allocation_id++;
    return SSU_OK;
}

ssu_err_t ssu_controller_release(const ssu_plugin_ops_t *plugin,
                                 const char *allocate_id)
{
    uint32_t i;

    if (plugin == NULL || plugin->delete_ns == NULL || allocate_id == NULL ||
        allocate_id[0] == '\0') {
        return SSU_ERR_INVALID;
    }

    for (i = 0; i < SSU_CONTROLLER_MAX_ALLOCATIONS; i++) {
        if (allocations[i].active &&
            strcmp(allocations[i].info.allocate_id, allocate_id) == 0) {
            ssu_err_t err = plugin->delete_ns(allocations[i].info.ssu_id,
                                              allocations[i].info.ns_id);
            if (err != SSU_OK) {
                return err;
            }

            if (pool_count > 0 &&
                pool[0].used_bytes >= allocations[i].info.length) {
                pool[0].used_bytes -= allocations[i].info.length;
            }
            allocations[i].active = 0;
            remember_released_id(allocate_id);
            return SSU_OK;
        }
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
