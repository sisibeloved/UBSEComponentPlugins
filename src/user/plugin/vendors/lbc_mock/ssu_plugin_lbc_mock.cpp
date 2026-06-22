#include "ssu_plugin.h"
#include "ssu_plugin_lbc_mock.h"
#include "../../reqshim_iface.h"

#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <set>
#include <string>

#define LBC_MOCK_MAX_NAMESPACES 32U
#define LBC_MOCK_MAX_MOUNTS 32U
#define LBC_MOCK_NSZE 1048576ULL
#define LBC_MOCK_SECTOR_SIZE 512ULL
#define LBC_MOCK_TOTAL_BYTES (LBC_MOCK_NSZE * LBC_MOCK_SECTOR_SIZE)
#define LBC_MOCK_SUBNQN_MAX 31U

typedef struct {
    char prefix[256];
    char dev_ip[64];
    uint16_t port;
    char subnqn[32];
    char dev_dir[256];
    char configfs_dir[256];
    char log_file[256];
} lbc_mock_config_t;

typedef struct {
    int active;
    char allocate_id[64];
    char ssu_id[64];
    char ns_id[32];
    char dev_path[256];
    uint64_t logical_offset;
    uint64_t length;
    uint64_t phys_sector;
} lbc_mock_namespace_t;

typedef struct {
    int active;
    char allocate_id[64];
    char host_id[64];
    char logical_dev[64];
} lbc_mock_mount_t;

static lbc_mock_config_t lbc_config = {
    ".",
    "127.0.0.1",
    4420,
    "nqn.2025-01.io.ssu:m0",
    "/dev",
    "/sys/kernel/config/nvmet/subsystems",
    "/tmp/ubse-lbc-mock.log",
};
static lbc_mock_namespace_t namespaces[LBC_MOCK_MAX_NAMESPACES];
static lbc_mock_mount_t mounts[LBC_MOCK_MAX_MOUNTS];
static uint32_t next_nsid = 1;
static int target_ready;
static int config_loaded;

static int is_empty_string(const char *s)
{
    return s == NULL || s[0] == '\0';
}

static void lbc_mock_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;
    va_list ap_file;

    va_start(ap, fmt);
    va_copy(ap_file, ap);

    fputs("lbc_mock: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);

    if (!is_empty_string(lbc_config.log_file)) {
        fp = fopen(lbc_config.log_file, "a");
        if (fp != NULL) {
            fputs("lbc_mock: ", fp);
            vfprintf(fp, fmt, ap_file);
            fputc('\n', fp);
            fclose(fp);
        }
    }

    va_end(ap_file);
    va_end(ap);
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

static int checked_join(char *out, size_t n, const char *prefix,
                        const char *suffix)
{
    int rc;

    if (is_empty_string(prefix) || is_empty_string(suffix)) {
        return 0;
    }

    if (prefix[strlen(prefix) - 1] == '/') {
        rc = snprintf(out, n, "%s%s", prefix, suffix);
    } else {
        rc = snprintf(out, n, "%s/%s", prefix, suffix);
    }

    return rc >= 0 && (size_t)rc < n;
}

static char *trim_ascii_space(char *s)
{
    char *end;

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }

    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

static void apply_config_pair(const char *key, const char *value)
{
    char *end = NULL;
    unsigned long port;

    if (strcmp(key, "dev_ip") == 0 || strcmp(key, "dev-ip") == 0) {
        copy_cstr(lbc_config.dev_ip, sizeof(lbc_config.dev_ip), value);
        return;
    }

    if (strcmp(key, "port") == 0) {
        errno = 0;
        port = strtoul(value, &end, 10);
        if (errno == 0 && end != value && *end == '\0' &&
            port > 0 && port <= UINT16_MAX) {
            lbc_config.port = (uint16_t)port;
        } else {
            lbc_mock_log("ignore invalid config port=%s", value);
        }
        return;
    }

    if (strcmp(key, "subnqn") == 0 || strcmp(key, "sub-nqn") == 0) {
        if (strlen(value) <= LBC_MOCK_SUBNQN_MAX) {
            copy_cstr(lbc_config.subnqn, sizeof(lbc_config.subnqn), value);
        } else {
            lbc_mock_log("ignore invalid config subnqn=%s length=%zu max=%u",
                         value, strlen(value), LBC_MOCK_SUBNQN_MAX);
        }
        return;
    }

    if (strcmp(key, "dev_dir") == 0 || strcmp(key, "dev-dir") == 0) {
        copy_cstr(lbc_config.dev_dir, sizeof(lbc_config.dev_dir), value);
        return;
    }

    if (strcmp(key, "configfs_dir") == 0 ||
        strcmp(key, "configfs-dir") == 0) {
        copy_cstr(lbc_config.configfs_dir, sizeof(lbc_config.configfs_dir),
                  value);
        return;
    }

    if (strcmp(key, "log_file") == 0 || strcmp(key, "log-file") == 0) {
        copy_cstr(lbc_config.log_file, sizeof(lbc_config.log_file), value);
        return;
    }

    lbc_mock_log("ignore unknown config key=%s", key);
}

static void load_config_file(const char *path)
{
    FILE *fp;
    char line[512];
    unsigned int line_no = 0;

    fp = fopen(path, "r");
    if (fp == NULL) {
        lbc_mock_log("config file not found, using defaults: %s", path);
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = trim_ascii_space(line);
        char *eq;
        char *key;
        char *value;

        line_no++;
        if (*p == '\0' || *p == '#') {
            continue;
        }

        eq = strchr(p, '=');
        if (eq == NULL) {
            lbc_mock_log("ignore malformed config line %u in %s", line_no,
                         path);
            continue;
        }

        *eq = '\0';
        key = trim_ascii_space(p);
        value = trim_ascii_space(eq + 1);
        apply_config_pair(key, value);
    }

    fclose(fp);
    lbc_mock_log("config file loaded: %s", path);
}

static void ensure_config_loaded(void)
{
    char config_path[512];
    const char *prefix_env;

    if (config_loaded) {
        return;
    }

    config_loaded = 1;
    prefix_env = getenv("LBC_PREFIX");
    if (!is_empty_string(prefix_env)) {
        copy_cstr(lbc_config.prefix, sizeof(lbc_config.prefix), prefix_env);
    }

    if (!checked_join(config_path, sizeof(config_path), lbc_config.prefix,
                      "mock/ssu_lbc_mock.conf")) {
        lbc_mock_log("invalid prefix while loading config: prefix=%s",
                     lbc_config.prefix);
        return;
    }

    load_config_file(config_path);
    lbc_mock_log("config prefix=%s dev_dir=%s configfs_dir=%s subnqn=%s port=%u log_file=%s",
                 lbc_config.prefix, lbc_config.dev_dir,
                 lbc_config.configfs_dir, lbc_config.subnqn,
                 (unsigned int)lbc_config.port, lbc_config.log_file);
}

static int script_exists(const char *relative_path)
{
    char path[512];

    if (!checked_join(path, sizeof(path), lbc_config.prefix,
                      relative_path)) {
        return 0;
    }

    return access(path, X_OK) == 0 || access(path, R_OK) == 0;
}

static ssu_err_t run_command_in_prefix(const char *const argv[])
{
    pid_t pid;
    int status;
    char command[1024];
    size_t used = 0;

    for (int i = 0; argv[i] != NULL; i++) {
        int rc = snprintf(command + used, sizeof(command) - used,
                          "%s%s", i == 0 ? "" : " ", argv[i]);
        if (rc < 0) {
            break;
        }
        if ((size_t)rc >= sizeof(command) - used) {
            used = sizeof(command) - 1;
            break;
        }
        used += (size_t)rc;
    }
    command[used] = '\0';
    lbc_mock_log("run cwd=%s command=%s", lbc_config.prefix, command);

    pid = fork();
    if (pid < 0) {
        lbc_mock_log("fork failed for command=%s errno=%d", command, errno);
        return SSU_ERR_IO;
    }

    if (pid == 0) {
        if (chdir(lbc_config.prefix) != 0) {
            _exit(126);
        }

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    do {
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            lbc_mock_log("waitpid failed for command=%s errno=%d", command,
                         errno);
            return SSU_ERR_IO;
        }
        break;
    } while (1);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            if (exit_code == 126) {
                lbc_mock_log("command failed exit=126; cannot enter prefix directory or command is not executable: cwd=%s command=%s",
                             lbc_config.prefix, command);
            } else if (exit_code == 127) {
                lbc_mock_log("command failed exit=127; executable not found: command=%s",
                             command);
            } else {
                lbc_mock_log("command failed exit=%d command=%s",
                             exit_code, command);
            }
        } else {
            lbc_mock_log("command terminated abnormally command=%s", command);
        }
        return SSU_ERR_IO;
    }

    return SSU_OK;
}

static ssu_err_t ensure_scripts_available(void)
{
    char setup_path[512] = "mock/setup_mock_target.sh";
    char run_path[512] = "mock/run_mock.sh";

    ensure_config_loaded();

    if (!script_exists("mock/setup_mock_target.sh")) {
        checked_join(setup_path, sizeof(setup_path), lbc_config.prefix,
                     "mock/setup_mock_target.sh");
        lbc_mock_log("missing setup script: %s", setup_path);
        return SSU_ERR_NOT_FOUND;
    }

    if (!script_exists("mock/run_mock.sh")) {
        checked_join(run_path, sizeof(run_path), lbc_config.prefix,
                     "mock/run_mock.sh");
        lbc_mock_log("missing run script: %s", run_path);
        return SSU_ERR_NOT_FOUND;
    }

    return SSU_OK;
}

static ssu_err_t ensure_target_ready(void)
{
    char port[16];
    const char *argv[] = {
        "bash",
        "mock/setup_mock_target.sh",
        lbc_config.subnqn,
        port,
        NULL,
    };
    ssu_err_t err;

    if (target_ready) {
        return SSU_OK;
    }

    err = ensure_scripts_available();
    if (err != SSU_OK) {
        return err;
    }

    snprintf(port, sizeof(port), "%u", (unsigned int)lbc_config.port);
    err = run_command_in_prefix(argv);
    if (err != SSU_OK) {
        lbc_mock_log("setup target failed subnqn=%s port=%s err=%d",
                     lbc_config.subnqn, port, err);
        return err;
    }

    target_ready = 1;
    lbc_mock_log("target ready subnqn=%s port=%s", lbc_config.subnqn, port);
    return SSU_OK;
}

static int is_nvme_namespace_name(const char *name)
{
    const char *p;

    if (name == NULL || strncmp(name, "nvme", 4) != 0) {
        return 0;
    }

    p = name + 4;
    while (*p != '\0') {
        if (*p == 'n') {
            return 1;
        }
        p++;
    }

    return 0;
}

static std::set<std::string> scan_nvme_namespaces(void)
{
    std::set<std::string> result;
    DIR *dir;
    struct dirent *entry;

    dir = opendir(lbc_config.dev_dir);
    if (dir == NULL) {
        return result;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[512];

        if (!is_nvme_namespace_name(entry->d_name)) {
            continue;
        }

        if (!checked_join(path, sizeof(path), lbc_config.dev_dir,
                          entry->d_name)) {
            continue;
        }

        result.insert(path);
    }

    closedir(dir);
    return result;
}

static int choose_created_dev(const std::set<std::string> &before,
                              char *out_dev_path, size_t n)
{
    std::set<std::string> after;
    int attempt;

    for (attempt = 0; attempt < 50; attempt++) {
        after = scan_nvme_namespaces();
        for (const std::string &path : after) {
            if (before.find(path) == before.end()) {
                copy_cstr(out_dev_path, n, path.c_str());
                return 1;
            }
        }

        usleep(100000);
    }

    if (before.empty() && !after.empty()) {
        copy_cstr(out_dev_path, n, after.begin()->c_str());
        return 1;
    }

    return 0;
}

static lbc_mock_namespace_t *find_namespace_by_allocate(
    const char *allocate_id)
{
    uint32_t i;

    for (i = 0; i < LBC_MOCK_MAX_NAMESPACES; i++) {
        if (namespaces[i].active &&
            strcmp(namespaces[i].allocate_id, allocate_id) == 0) {
            return &namespaces[i];
        }
    }

    return NULL;
}

static lbc_mock_namespace_t *find_namespace(const char *ssu_id,
                                            const char *ns_id)
{
    uint32_t i;

    for (i = 0; i < LBC_MOCK_MAX_NAMESPACES; i++) {
        if (namespaces[i].active &&
            strcmp(namespaces[i].ssu_id, ssu_id) == 0 &&
            strcmp(namespaces[i].ns_id, ns_id) == 0) {
            return &namespaces[i];
        }
    }

    return NULL;
}

static lbc_mock_mount_t *find_mount(const char *logical_dev)
{
    uint32_t i;

    for (i = 0; i < LBC_MOCK_MAX_MOUNTS; i++) {
        if (mounts[i].active &&
            strcmp(mounts[i].logical_dev, logical_dev) == 0) {
            return &mounts[i];
        }
    }

    return NULL;
}

static void lbc_mock_reqshim_log(void *ctx, const char *message)
{
    (void)ctx;
    lbc_mock_log("%s", message);
}

static int lbc_mock_reqshim_allow_missing(void)
{
    return strcmp(lbc_config.dev_dir, "/dev") != 0 ||
           strcmp(lbc_config.configfs_dir,
                  "/sys/kernel/config/nvmet/subsystems") != 0;
}

static ssu_err_t collect_reqshim_maps(const char *allocate_id,
                                      ssu_reqshim_map_spec_t *maps,
                                      uint32_t max_maps,
                                      uint32_t *out_count)
{
    uint32_t count = 0;
    uint32_t i;

    if (is_empty_string(allocate_id) || maps == NULL ||
        out_count == NULL) {
        return SSU_ERR_INVALID;
    }

    for (i = 0; i < LBC_MOCK_MAX_NAMESPACES; i++) {
        if (!namespaces[i].active ||
            strcmp(namespaces[i].allocate_id, allocate_id) != 0) {
            continue;
        }

        if (count >= max_maps) {
            return SSU_ERR_NO_RESOURCE;
        }

        if (strlen(namespaces[i].dev_path) >= sizeof(maps[count].phys_dev)) {
            lbc_mock_log("ReqShim map failed: dev_path too long dev_path=%s max=%zu",
                         namespaces[i].dev_path,
                         sizeof(maps[count].phys_dev) - 1);
            return SSU_ERR_INVALID;
        }

        memset(&maps[count], 0, sizeof(maps[count]));
        copy_cstr(maps[count].phys_dev, sizeof(maps[count].phys_dev),
                  namespaces[i].dev_path);
        copy_cstr(maps[count].ns_id, sizeof(maps[count].ns_id),
                  namespaces[i].ns_id);
        maps[count].logical_offset = namespaces[i].logical_offset;
        maps[count].length = namespaces[i].length;
        maps[count].phys_sector = namespaces[i].phys_sector;
        count++;
    }

    *out_count = count;
    return count == 0 ? SSU_ERR_NOT_FOUND : SSU_OK;
}

ssu_err_t ssu_lbc_mock_configure(const ssu_lbc_mock_config_t *config)
{
    if (config == NULL || is_empty_string(config->prefix)) {
        lbc_mock_log("configure failed: prefix is required");
        return SSU_ERR_INVALID;
    }

    if (config->subnqn != NULL &&
        strlen(config->subnqn) > LBC_MOCK_SUBNQN_MAX) {
        lbc_mock_log("configure failed: subnqn too long length=%zu max=%u value=%s",
                     strlen(config->subnqn), LBC_MOCK_SUBNQN_MAX,
                     config->subnqn);
        return SSU_ERR_INVALID;
    }

    memset(&lbc_config, 0, sizeof(lbc_config));
    copy_cstr(lbc_config.prefix, sizeof(lbc_config.prefix), config->prefix);
    copy_cstr(lbc_config.dev_ip, sizeof(lbc_config.dev_ip),
              is_empty_string(config->dev_ip) ? "127.0.0.1" :
                                                config->dev_ip);
    lbc_config.port = config->port == 0 ? 4420 : config->port;
    copy_cstr(lbc_config.subnqn, sizeof(lbc_config.subnqn),
              is_empty_string(config->subnqn) ?
                  "nqn.2025-01.io.ssu:m0" : config->subnqn);
    copy_cstr(lbc_config.dev_dir, sizeof(lbc_config.dev_dir),
              is_empty_string(config->dev_dir) ? "/dev" : config->dev_dir);
    copy_cstr(lbc_config.configfs_dir, sizeof(lbc_config.configfs_dir),
              is_empty_string(config->configfs_dir) ?
                  "/sys/kernel/config/nvmet/subsystems" :
                  config->configfs_dir);
    copy_cstr(lbc_config.log_file, sizeof(lbc_config.log_file),
              is_empty_string(config->log_file) ?
                  "/tmp/ubse-lbc-mock.log" : config->log_file);

    memset(namespaces, 0, sizeof(namespaces));
    memset(mounts, 0, sizeof(mounts));
    next_nsid = 1;
    target_ready = 0;
    config_loaded = 1;
    lbc_mock_log("configure prefix=%s dev_dir=%s configfs_dir=%s subnqn=%s port=%u log_file=%s",
                 lbc_config.prefix, lbc_config.dev_dir,
                 lbc_config.configfs_dir, lbc_config.subnqn,
                 (unsigned int)lbc_config.port, lbc_config.log_file);
    return SSU_OK;
}

static const char *lbc_mock_name(void)
{
    return "lbc_mock";
}

static ssu_err_t lbc_mock_discover(ssu_resource_info_t *out,
                                   uint32_t *inout_count)
{
    ensure_config_loaded();

    if (inout_count == NULL) {
        return SSU_ERR_INVALID;
    }

    if (out == NULL || *inout_count < 1) {
        *inout_count = 1;
        return SSU_ERR_BUFFER_TOO_SMALL;
    }

    memset(out, 0, sizeof(out[0]));
    copy_cstr(out[0].ssu_id, sizeof(out[0].ssu_id), "lbc-mock-ssu0");
    copy_cstr(out[0].host_id, sizeof(out[0].host_id), "lbc-mock-host0");
    copy_cstr(out[0].state, sizeof(out[0].state), "ONLINE");
    out[0].total_bytes = LBC_MOCK_TOTAL_BYTES;
    out[0].used_bytes = 0;
    *inout_count = 1;
    return SSU_OK;
}

static ssu_err_t lbc_mock_connect(const char *ssu_id, char *out_devname,
                                  size_t n)
{
    uint32_t i;

    ensure_config_loaded();

    if (is_empty_string(ssu_id) || out_devname == NULL || n == 0) {
        lbc_mock_log("connect failed: invalid arguments ssu_id=%s out=%p n=%zu",
                     ssu_id == NULL ? "(null)" : ssu_id,
                     (void *)out_devname, n);
        return SSU_ERR_INVALID;
    }

    if (strcmp(ssu_id, "lbc-mock-ssu0") != 0) {
        lbc_mock_log("connect failed: unknown ssu_id=%s", ssu_id);
        return SSU_ERR_NOT_FOUND;
    }

    for (i = 0; i < LBC_MOCK_MAX_NAMESPACES; i++) {
        if (namespaces[i].active &&
            !is_empty_string(namespaces[i].dev_path)) {
            copy_cstr(out_devname, n, namespaces[i].dev_path);
            return SSU_OK;
        }
    }

    lbc_mock_log("connect failed: no active namespace for ssu_id=%s", ssu_id);
    return SSU_ERR_NOT_FOUND;
}

static ssu_err_t lbc_mock_health_check(const char *ssu_id, char *out_state,
                                       size_t n)
{
    ensure_config_loaded();

    if (is_empty_string(ssu_id) || out_state == NULL || n == 0) {
        lbc_mock_log("health_check failed: invalid arguments ssu_id=%s out=%p n=%zu",
                     ssu_id == NULL ? "(null)" : ssu_id,
                     (void *)out_state, n);
        return SSU_ERR_INVALID;
    }

    if (strcmp(ssu_id, "lbc-mock-ssu0") != 0) {
        lbc_mock_log("health_check failed: unknown ssu_id=%s", ssu_id);
        return SSU_ERR_NOT_FOUND;
    }

    copy_cstr(out_state, n, "ONLINE");
    return SSU_OK;
}

static ssu_err_t lbc_mock_create_ns(const ssu_extent_create_req_t *extent_req,
                                    char *out_ns_id, size_t n,
                                    uint64_t *out_phys_sector)
{
    lbc_mock_namespace_t *slot = NULL;
    std::set<std::string> before;
    char ns_id[32];
    char nsze[32];
    char port[16];
    char dev_path[256];
    const char *argv[] = {
        "bash",
        "mock/run_mock.sh",
        "./sample_create_attach",
        "--dev-ip",
        lbc_config.dev_ip,
        "--port",
        port,
        "--sub-nqn",
        lbc_config.subnqn,
        "--nsze",
        nsze,
        NULL,
    };
    ssu_err_t err;
    uint32_t i;

    ensure_config_loaded();

    if (extent_req == NULL || out_ns_id == NULL || n == 0 ||
        out_phys_sector == NULL || is_empty_string(extent_req->allocate_id) ||
        is_empty_string(extent_req->ssu_id)) {
        lbc_mock_log("create_ns failed: invalid arguments req=%p out_ns_id=%p n=%zu out_phys_sector=%p",
                     (const void *)extent_req, (void *)out_ns_id, n,
                     (void *)out_phys_sector);
        return SSU_ERR_INVALID;
    }

    lbc_mock_log("create_ns start allocate_id=%s ssu_id=%s length=%llu policy=%d dev_dir=%s subnqn=%s",
                 extent_req->allocate_id, extent_req->ssu_id,
                 (unsigned long long)extent_req->length,
                 (int)extent_req->policy, lbc_config.dev_dir,
                 lbc_config.subnqn);

    if (strcmp(extent_req->ssu_id, "lbc-mock-ssu0") != 0) {
        lbc_mock_log("create_ns failed: unknown ssu_id=%s allocate_id=%s",
                     extent_req->ssu_id, extent_req->allocate_id);
        return SSU_ERR_NOT_FOUND;
    }

    if (extent_req->policy != SSU_RELIABILITY_STRIPE) {
        lbc_mock_log("create_ns failed: unsupported policy=%d allocate_id=%s",
                     (int)extent_req->policy, extent_req->allocate_id);
        return SSU_ERR_UNSUPPORTED;
    }

    if (find_namespace_by_allocate(extent_req->allocate_id) != NULL) {
        lbc_mock_log("create_ns failed: allocate_id already exists allocate_id=%s",
                     extent_req->allocate_id);
        return SSU_ERR_NS_EXISTS;
    }

    for (i = 0; i < LBC_MOCK_MAX_NAMESPACES; i++) {
        if (!namespaces[i].active) {
            slot = &namespaces[i];
            break;
        }
    }

    if (slot == NULL) {
        lbc_mock_log("create_ns failed: namespace table full max=%u",
                     LBC_MOCK_MAX_NAMESPACES);
        return SSU_ERR_NO_RESOURCE;
    }

    err = ensure_target_ready();
    if (err != SSU_OK) {
        lbc_mock_log("create_ns failed: target not ready allocate_id=%s err=%d",
                     extent_req->allocate_id, err);
        return err;
    }

    if (access(lbc_config.dev_dir, R_OK | X_OK) != 0) {
        lbc_mock_log("create_ns warning: cannot scan dev_dir before create dev_dir=%s errno=%d",
                     lbc_config.dev_dir, errno);
    }
    before = scan_nvme_namespaces();
    snprintf(port, sizeof(port), "%u", (unsigned int)lbc_config.port);
    snprintf(nsze, sizeof(nsze), "%llu",
             (unsigned long long)LBC_MOCK_NSZE);
    err = run_command_in_prefix(argv);
    if (err != SSU_OK) {
        lbc_mock_log("create_ns failed: create_attach command failed allocate_id=%s err=%d",
                     extent_req->allocate_id, err);
        return err;
    }

    memset(dev_path, 0, sizeof(dev_path));
    if (!choose_created_dev(before, dev_path, sizeof(dev_path))) {
        std::set<std::string> after = scan_nvme_namespaces();
        lbc_mock_log("create_attach completed but no new NVMe namespace was found: dev_dir=%s before=%zu after=%zu nsze=%s subnqn=%s",
                     lbc_config.dev_dir, before.size(), after.size(), nsze,
                     lbc_config.subnqn);
        return SSU_ERR_NOT_FOUND;
    }

    snprintf(ns_id, sizeof(ns_id), "%u", next_nsid);

    memset(slot, 0, sizeof(*slot));
    slot->active = 1;
    copy_cstr(slot->allocate_id, sizeof(slot->allocate_id),
              extent_req->allocate_id);
    copy_cstr(slot->ssu_id, sizeof(slot->ssu_id), extent_req->ssu_id);
    copy_cstr(slot->ns_id, sizeof(slot->ns_id), ns_id);
    copy_cstr(slot->dev_path, sizeof(slot->dev_path), dev_path);
    slot->logical_offset = extent_req->logical_offset;
    slot->length = extent_req->length;
    slot->phys_sector = 0;

    copy_cstr(out_ns_id, n, ns_id);
    *out_phys_sector = 0;
    next_nsid++;
    lbc_mock_log("create_ns success allocate_id=%s ns_id=%s dev_path=%s length=%llu",
                 extent_req->allocate_id, ns_id, dev_path,
                 (unsigned long long)extent_req->length);
    return SSU_OK;
}

static ssu_err_t lbc_mock_delete_ns(const char *ssu_id, const char *ns_id)
{
    lbc_mock_namespace_t *ns;
    char port[16];
    const char *argv[] = {
        "bash",
        "mock/run_mock.sh",
        "./sample_detach_delete",
        "--nsid",
        NULL,
        "--dev-path",
        NULL,
        "--dev-ip",
        lbc_config.dev_ip,
        "--port",
        port,
        "--sub-nqn",
        lbc_config.subnqn,
        NULL,
    };
    ssu_err_t err;

    ensure_config_loaded();

    if (is_empty_string(ssu_id) || is_empty_string(ns_id)) {
        lbc_mock_log("delete_ns failed: invalid arguments ssu_id=%s ns_id=%s",
                     ssu_id == NULL ? "(null)" : ssu_id,
                     ns_id == NULL ? "(null)" : ns_id);
        return SSU_ERR_INVALID;
    }

    lbc_mock_log("delete_ns start ssu_id=%s ns_id=%s", ssu_id, ns_id);
    ns = find_namespace(ssu_id, ns_id);
    if (ns == NULL) {
        lbc_mock_log("delete_ns failed: namespace not found ssu_id=%s ns_id=%s",
                     ssu_id, ns_id);
        return SSU_ERR_NOT_FOUND;
    }

    snprintf(port, sizeof(port), "%u", (unsigned int)lbc_config.port);
    argv[4] = ns->ns_id;
    argv[6] = ns->dev_path;
    err = run_command_in_prefix(argv);
    if (err != SSU_OK) {
        lbc_mock_log("delete_ns failed: detach_delete command failed ssu_id=%s ns_id=%s dev_path=%s err=%d",
                     ssu_id, ns_id, ns->dev_path, err);
        return err;
    }

    lbc_mock_log("delete_ns success ssu_id=%s ns_id=%s dev_path=%s",
                 ssu_id, ns_id, ns->dev_path);
    memset(ns, 0, sizeof(*ns));
    return SSU_OK;
}

static ssu_err_t lbc_mock_mount(const char *allocate_id, const char *host_id,
                                const char *logical_dev)
{
    ssu_reqshim_map_spec_t maps[LBC_MOCK_MAX_NAMESPACES];
    lbc_mock_mount_t *slot = NULL;
    uint32_t map_count = 0;
    ssu_err_t err;
    uint32_t i;

    ensure_config_loaded();

    if (is_empty_string(allocate_id) || is_empty_string(host_id) ||
        is_empty_string(logical_dev)) {
        lbc_mock_log("mount failed: invalid arguments allocate_id=%s host_id=%s logical_dev=%s",
                     allocate_id == NULL ? "(null)" : allocate_id,
                     host_id == NULL ? "(null)" : host_id,
                     logical_dev == NULL ? "(null)" : logical_dev);
        return SSU_ERR_INVALID;
    }

    lbc_mock_log("mount start allocate_id=%s host_id=%s logical_dev=%s",
                 allocate_id, host_id, logical_dev);
    if (find_namespace_by_allocate(allocate_id) == NULL) {
        lbc_mock_log("mount failed: allocation namespace not found allocate_id=%s",
                     allocate_id);
        return SSU_ERR_NOT_FOUND;
    }

    if (find_mount(logical_dev) != NULL) {
        lbc_mock_log("mount failed: logical device busy logical_dev=%s",
                     logical_dev);
        return SSU_ERR_BUSY;
    }

    for (i = 0; i < LBC_MOCK_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            slot = &mounts[i];
            break;
        }
    }

    if (slot == NULL) {
        lbc_mock_log("mount failed: mount table full max=%u",
                     LBC_MOCK_MAX_MOUNTS);
        return SSU_ERR_NO_RESOURCE;
    }

    memset(maps, 0, sizeof(maps));
    err = collect_reqshim_maps(allocate_id, maps, LBC_MOCK_MAX_NAMESPACES,
                               &map_count);
    if (err != SSU_OK) {
        lbc_mock_log("mount failed: build ReqShim maps failed allocate_id=%s err=%d",
                     allocate_id, err);
        return err;
    }

    err = ssu_reqshim_mount_logdev(logical_dev, maps, map_count,
                                   lbc_mock_reqshim_allow_missing(),
                                   lbc_mock_reqshim_log, NULL,
                                   NULL, NULL);
    if (err != SSU_OK) {
        lbc_mock_log("mount failed: ReqShim mount failed allocate_id=%s logical_dev=%s err=%d",
                     allocate_id, logical_dev, err);
        return err;
    }

    memset(slot, 0, sizeof(*slot));
    slot->active = 1;
    copy_cstr(slot->allocate_id, sizeof(slot->allocate_id), allocate_id);
    copy_cstr(slot->host_id, sizeof(slot->host_id), host_id);
    copy_cstr(slot->logical_dev, sizeof(slot->logical_dev), logical_dev);
    lbc_mock_log("mount success allocate_id=%s host_id=%s logical_dev=%s",
                 allocate_id, host_id, logical_dev);
    return SSU_OK;
}

static ssu_err_t lbc_mock_unmount(const char *logical_dev)
{
    ssu_reqshim_map_spec_t maps[LBC_MOCK_MAX_NAMESPACES];
    lbc_mock_mount_t *mount;
    uint32_t map_count = 0;
    ssu_err_t err;

    ensure_config_loaded();

    if (is_empty_string(logical_dev)) {
        lbc_mock_log("unmount failed: invalid logical_dev=%s",
                     logical_dev == NULL ? "(null)" : logical_dev);
        return SSU_ERR_INVALID;
    }

    lbc_mock_log("unmount start logical_dev=%s", logical_dev);
    mount = find_mount(logical_dev);
    if (mount == NULL) {
        lbc_mock_log("unmount failed: logical device not found logical_dev=%s",
                     logical_dev);
        return SSU_ERR_NOT_FOUND;
    }

    memset(maps, 0, sizeof(maps));
    err = collect_reqshim_maps(mount->allocate_id, maps,
                               LBC_MOCK_MAX_NAMESPACES, &map_count);
    if (err != SSU_OK) {
        lbc_mock_log("unmount failed: build ReqShim maps failed allocate_id=%s logical_dev=%s err=%d",
                     mount->allocate_id, logical_dev, err);
        return err;
    }

    err = ssu_reqshim_unmount_logdev(logical_dev, maps, map_count,
                                     lbc_mock_reqshim_allow_missing(),
                                     lbc_mock_reqshim_log, NULL,
                                     NULL, NULL);
    if (err != SSU_OK) {
        lbc_mock_log("unmount failed: ReqShim unmount failed allocate_id=%s logical_dev=%s err=%d",
                     mount->allocate_id, logical_dev, err);
        return err;
    }

    lbc_mock_log("unmount success logical_dev=%s allocate_id=%s host_id=%s",
                 logical_dev, mount->allocate_id, mount->host_id);
    memset(mount, 0, sizeof(*mount));
    return SSU_OK;
}

static const ssu_plugin_ops_t ops = {
    lbc_mock_name,
    lbc_mock_discover,
    lbc_mock_connect,
    lbc_mock_health_check,
    lbc_mock_create_ns,
    lbc_mock_delete_ns,
    lbc_mock_mount,
    lbc_mock_unmount,
};

const ssu_plugin_ops_t *ssu_plugin_entry(void)
{
    return &ops;
}
