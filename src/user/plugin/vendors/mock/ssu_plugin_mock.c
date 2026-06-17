#include "ssu_plugin.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MOCK_TOTAL_BYTES (1ULL << 30)
#define MOCK_MAX_NULLBLK 128U
#define MOCK_MAX_NAMESPACES 128U
#define MOCK_MAX_MOUNTS 128U

typedef struct {
    int active;
    char allocate_id[64];
    char ssu_id[64];
    char ns_id[32];
    uint64_t logical_offset;
    uint64_t length;
    uint64_t phys_sector;
} mock_namespace_t;

typedef struct {
    int active;
    char allocate_id[64];
    char host_id[64];
    char logical_dev[64];
} mock_mount_t;

static mock_namespace_t namespaces[MOCK_MAX_NAMESPACES];
static mock_mount_t mounts[MOCK_MAX_MOUNTS];
static uint64_t next_ns_id;

static int parse_uint32_tail(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;

    if (s == NULL || s[0] == '\0' || out == NULL) {
        return 0;
    }

    errno = 0;
    value = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value > UINT32_MAX) {
        return 0;
    }

    *out = (uint32_t)value;
    return 1;
}

static int configured_mock_count(uint32_t *out)
{
    const char *configured_count = getenv("SSU_MOCK_SSU_COUNT");

    if (configured_count == NULL || configured_count[0] == '\0') {
        return 0;
    }

    return parse_uint32_tail(configured_count, out) ? 1 : -1;
}

static int parse_mock_ssu_id(const char *ssu_id, uint32_t *index)
{
    const char prefix[] = "mock-ssu";
    size_t prefix_len = sizeof(prefix) - 1;

    if (ssu_id == NULL || strncmp(ssu_id, prefix, prefix_len) != 0) {
        return 0;
    }

    return parse_uint32_tail(ssu_id + prefix_len, index);
}

static int nullblk_exists(uint32_t index)
{
    char path[64];

    snprintf(path, sizeof(path), "/sys/block/nullb%u", index);
    return access(path, F_OK) == 0;
}

static uint64_t nullblk_total_bytes(uint32_t index)
{
    char path[64];
    FILE *fp;
    unsigned long long sectors = 0;

    snprintf(path, sizeof(path), "/sys/block/nullb%u/size", index);
    fp = fopen(path, "r");
    if (fp == NULL) {
        return MOCK_TOTAL_BYTES;
    }

    if (fscanf(fp, "%llu", &sectors) != 1) {
        fclose(fp);
        return MOCK_TOTAL_BYTES;
    }

    fclose(fp);
    if (sectors == 0 || sectors > UINT64_MAX / 512ULL) {
        return MOCK_TOTAL_BYTES;
    }

    return (uint64_t)sectors * 512ULL;
}

static void fill_resource(ssu_resource_info_t *resource, uint32_t index,
                          uint64_t total_bytes, const char *state)
{
    memset(resource, 0, sizeof(*resource));
    snprintf(resource->ssu_id, sizeof(resource->ssu_id), "mock-ssu%u",
             index);
    snprintf(resource->host_id, sizeof(resource->host_id), "mock-host%u",
             index);
    resource->total_bytes = total_bytes;
    resource->used_bytes = 0;
    snprintf(resource->state, sizeof(resource->state), "%s", state);
}

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

static mock_namespace_t *find_namespace_by_allocate(const char *allocate_id)
{
    uint32_t i;

    for (i = 0; i < MOCK_MAX_NAMESPACES; i++) {
        if (namespaces[i].active &&
            strcmp(namespaces[i].allocate_id, allocate_id) == 0) {
            return &namespaces[i];
        }
    }

    return NULL;
}

static mock_namespace_t *find_namespace(const char *ssu_id, const char *ns_id)
{
    uint32_t i;

    for (i = 0; i < MOCK_MAX_NAMESPACES; i++) {
        if (namespaces[i].active &&
            strcmp(namespaces[i].ssu_id, ssu_id) == 0 &&
            strcmp(namespaces[i].ns_id, ns_id) == 0) {
            return &namespaces[i];
        }
    }

    return NULL;
}

static mock_mount_t *find_mount(const char *logical_dev)
{
    uint32_t i;

    for (i = 0; i < MOCK_MAX_MOUNTS; i++) {
        if (mounts[i].active &&
            strcmp(mounts[i].logical_dev, logical_dev) == 0) {
            return &mounts[i];
        }
    }

    return NULL;
}

static void insert_index(uint32_t *indexes, uint32_t *count, uint32_t index)
{
    uint32_t pos;
    uint32_t i;

    if (*count >= MOCK_MAX_NULLBLK) {
        return;
    }

    for (pos = 0; pos < *count; pos++) {
        if (indexes[pos] == index) {
            return;
        }

        if (indexes[pos] > index) {
            break;
        }
    }

    for (i = *count; i > pos; i--) {
        indexes[i] = indexes[i - 1];
    }

    indexes[pos] = index;
    *count += 1;
}

static uint32_t scan_nullblk_indexes(uint32_t *indexes)
{
    DIR *dir;
    struct dirent *entry;
    uint32_t count = 0;

    dir = opendir("/sys/block");
    if (dir == NULL) {
        return 0;
    }

    while ((entry = readdir(dir)) != NULL) {
        uint32_t index;

        if (strncmp(entry->d_name, "nullb", 5) != 0) {
            continue;
        }

        if (!parse_uint32_tail(entry->d_name + 5, &index)) {
            continue;
        }

        insert_index(indexes, &count, index);
    }

    closedir(dir);
    return count;
}

static ssu_err_t discover_configured(ssu_resource_info_t *out,
                                     uint32_t *inout_count,
                                     uint32_t count)
{
    uint32_t i;

    if (out == NULL || *inout_count < count) {
        *inout_count = count;
        return count == 0 ? SSU_OK : SSU_ERR_BUFFER_TOO_SMALL;
    }

    for (i = 0; i < count; i++) {
        fill_resource(&out[i], i, MOCK_TOTAL_BYTES, "ONLINE");
    }

    *inout_count = count;
    return SSU_OK;
}

static ssu_err_t discover_nullblk(ssu_resource_info_t *out,
                                  uint32_t *inout_count)
{
    uint32_t indexes[MOCK_MAX_NULLBLK];
    uint32_t count;
    uint32_t i;

    memset(indexes, 0, sizeof(indexes));
    count = scan_nullblk_indexes(indexes);
    if (out == NULL || *inout_count < count) {
        *inout_count = count;
        return count == 0 ? SSU_OK : SSU_ERR_BUFFER_TOO_SMALL;
    }

    for (i = 0; i < count; i++) {
        fill_resource(&out[i], indexes[i], nullblk_total_bytes(indexes[i]),
                      "ONLINE");
    }

    *inout_count = count;
    return SSU_OK;
}

static const char *mock_name(void)
{
    return "mock";
}

static ssu_err_t mock_discover(ssu_resource_info_t *out, uint32_t *inout_count)
{
    uint32_t count = 0;
    int configured;

    if (inout_count == NULL) {
        return SSU_ERR_INVALID;
    }

    configured = configured_mock_count(&count);
    if (configured < 0) {
        return SSU_ERR_INVALID;
    }

    if (configured > 0) {
        return discover_configured(out, inout_count, count);
    }

    return discover_nullblk(out, inout_count);
}

static ssu_err_t mock_connect(const char *ssu_id, char *out_devname, size_t n)
{
    uint32_t index;
    uint32_t count = 0;
    int configured;

    if (ssu_id == NULL || out_devname == NULL || n == 0) {
        return SSU_ERR_INVALID;
    }

    if (!parse_mock_ssu_id(ssu_id, &index)) {
        return SSU_ERR_INVALID;
    }

    configured = configured_mock_count(&count);
    if (configured < 0) {
        return SSU_ERR_INVALID;
    }

    if ((configured > 0 && index >= count) ||
        (configured == 0 && !nullblk_exists(index))) {
        return SSU_ERR_NOT_FOUND;
    }

    snprintf(out_devname, n, "/dev/nullb%u", index);
    return SSU_OK;
}

static ssu_err_t mock_health_check(const char *ssu_id, char *out_state, size_t n)
{
    uint32_t index;
    uint32_t count = 0;
    int configured;
    int online;

    if (ssu_id == NULL || out_state == NULL || n == 0) {
        return SSU_ERR_INVALID;
    }

    if (!parse_mock_ssu_id(ssu_id, &index)) {
        return SSU_ERR_INVALID;
    }

    configured = configured_mock_count(&count);
    if (configured < 0) {
        return SSU_ERR_INVALID;
    }

    online = configured > 0 ? index < count : nullblk_exists(index);
    snprintf(out_state, n, "%s", online ? "ONLINE" : "OFFLINE");
    return SSU_OK;
}

static ssu_err_t mock_create_ns(const ssu_extent_create_req_t *extent_req,
                                char *out_ns_id, size_t n,
                                uint64_t *out_phys_sector)
{
    mock_namespace_t *slot = NULL;
    uint32_t i;

    if (extent_req == NULL || out_ns_id == NULL || n == 0 ||
        out_phys_sector == NULL || extent_req->allocate_id == NULL ||
        extent_req->ssu_id == NULL) {
        return SSU_ERR_INVALID;
    }

    if (extent_req->policy != SSU_RELIABILITY_STRIPE) {
        return SSU_ERR_UNSUPPORTED;
    }

    if (find_namespace_by_allocate(extent_req->allocate_id) != NULL) {
        return SSU_ERR_NS_EXISTS;
    }

    for (i = 0; i < MOCK_MAX_NAMESPACES; i++) {
        if (!namespaces[i].active) {
            slot = &namespaces[i];
            break;
        }
    }

    if (slot == NULL) {
        return SSU_ERR_NO_RESOURCE;
    }

    memset(slot, 0, sizeof(*slot));
    slot->active = 1;
    copy_cstr(slot->allocate_id, sizeof(slot->allocate_id),
              extent_req->allocate_id);
    copy_cstr(slot->ssu_id, sizeof(slot->ssu_id), extent_req->ssu_id);
    snprintf(slot->ns_id, sizeof(slot->ns_id), "mock-ns%llu",
             (unsigned long long)next_ns_id);
    slot->logical_offset = extent_req->logical_offset;
    slot->length = extent_req->length;
    slot->phys_sector = extent_req->phys_offset_hint;
    next_ns_id++;

    snprintf(out_ns_id, n, "%s", slot->ns_id);
    *out_phys_sector = extent_req->phys_offset_hint;
    return SSU_OK;
}

static ssu_err_t mock_delete_ns(const char *ssu_id, const char *ns_id)
{
    mock_namespace_t *ns;

    if (ssu_id == NULL || ns_id == NULL) {
        return SSU_ERR_INVALID;
    }

    ns = find_namespace(ssu_id, ns_id);
    if (ns == NULL) {
        return SSU_ERR_NOT_FOUND;
    }

    memset(ns, 0, sizeof(*ns));
    return SSU_OK;
}

static ssu_err_t mock_mount(const char *allocate_id, const char *host_id,
                            const char *logical_dev)
{
    mock_mount_t *slot = NULL;
    uint32_t i;

    if (allocate_id == NULL || host_id == NULL || logical_dev == NULL) {
        return SSU_ERR_INVALID;
    }

    if (find_namespace_by_allocate(allocate_id) == NULL) {
        return SSU_ERR_NOT_FOUND;
    }

    if (find_mount(logical_dev) != NULL) {
        return SSU_ERR_BUSY;
    }

    for (i = 0; i < MOCK_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            slot = &mounts[i];
            break;
        }
    }

    if (slot == NULL) {
        return SSU_ERR_NO_RESOURCE;
    }

    memset(slot, 0, sizeof(*slot));
    slot->active = 1;
    copy_cstr(slot->allocate_id, sizeof(slot->allocate_id), allocate_id);
    copy_cstr(slot->host_id, sizeof(slot->host_id), host_id);
    copy_cstr(slot->logical_dev, sizeof(slot->logical_dev), logical_dev);
    return SSU_OK;
}

static ssu_err_t mock_unmount(const char *logical_dev)
{
    mock_mount_t *mount;

    if (logical_dev == NULL) {
        return SSU_ERR_INVALID;
    }

    mount = find_mount(logical_dev);
    if (mount == NULL) {
        return SSU_ERR_NOT_FOUND;
    }

    memset(mount, 0, sizeof(*mount));
    return SSU_OK;
}

static const ssu_plugin_ops_t ops = {
    .name = mock_name,
    .discover = mock_discover,
    .connect = mock_connect,
    .health_check = mock_health_check,
    .create_ns = mock_create_ns,
    .delete_ns = mock_delete_ns,
    .mount = mock_mount,
    .unmount = mock_unmount,
};

const ssu_plugin_ops_t *ssu_plugin_entry(void)
{
    return &ops;
}
