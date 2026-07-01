#include "ssu_plugin.h"
#include "ssu_plugin_lbc_mock.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int expect_err(const char *name, ssu_err_t actual, ssu_err_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
        return 1;
    }

    return 0;
}

static int path_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static int mkdir_p(const char *path)
{
    char tmp[512];
    size_t len;
    size_t i;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] != '/') {
            continue;
        }
        tmp[i] = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
            perror(tmp);
            return 1;
        }
        tmp[i] = '/';
    }

    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
        perror(tmp);
        return 1;
    }

    return 0;
}

static int write_file(const char *path, mode_t mode, const char *fmt, ...)
{
    FILE *fp;
    va_list ap;

    fp = fopen(path, "w");
    if (fp == NULL) {
        perror(path);
        return 1;
    }

    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fclose(fp);

    if (chmod(path, mode) != 0) {
        perror(path);
        return 1;
    }

    return 0;
}

static int file_contains(const char *path, const char *needle)
{
    FILE *fp;
    char buf[8192];
    size_t n;

    fp = fopen(path, "r");
    if (fp == NULL) {
        perror(path);
        return 0;
    }

    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static uint64_t block_device_size_bytes(const char *dev_path)
{
    const char *name;
    char sysfs_size[512];
    FILE *fp;
    unsigned long long sectors = 0;

    name = strrchr(dev_path, '/');
    name = name == NULL ? dev_path : name + 1;
    snprintf(sysfs_size, sizeof(sysfs_size),
             "/sys/class/block/%s/size", name);

    fp = fopen(sysfs_size, "r");
    if (fp == NULL) {
        return 0;
    }

    if (fscanf(fp, "%llu", &sectors) != 1) {
        fclose(fp);
        return 0;
    }

    fclose(fp);
    return (uint64_t)sectors * 512ULL;
}

static int wait_path_gone(const char *path)
{
    int i;

    for (i = 0; i < 50; i++) {
        if (!path_exists(path)) {
            return 1;
        }
        usleep(100000);
    }

    return 0;
}

static int create_fake_lbc_prefix(const char *prefix, const char *dev_dir,
                                  const char *configfs_dir,
                                  const char *log_path)
{
    char mock_dir[512];
    char setup_path[1024];
    char run_path[1024];
    char sample_create[1024];
    char sample_delete[1024];

    snprintf(mock_dir, sizeof(mock_dir), "%s/mock", prefix);
    snprintf(setup_path, sizeof(setup_path), "%s/setup_mock_target.sh",
             mock_dir);
    snprintf(run_path, sizeof(run_path), "%s/run_mock.sh", mock_dir);
    snprintf(sample_create, sizeof(sample_create), "%s/sample_create_attach",
             prefix);
    snprintf(sample_delete, sizeof(sample_delete), "%s/sample_detach_delete",
             prefix);

    if (mkdir_p(mock_dir) != 0 ||
        mkdir_p(dev_dir) != 0 ||
        mkdir_p(configfs_dir) != 0) {
        return 1;
    }

    if (write_file(setup_path, 0700,
                   "#!/bin/sh\n"
                   "set -eu\n"
                   "echo setup \"$1\" \"$2\" >> '%s'\n"
                   "mkdir -p '%s'/\"$1\"/namespaces\n",
                   log_path, configfs_dir) != 0) {
        return 1;
    }

    if (write_file(run_path, 0700,
                   "#!/bin/sh\n"
                   "set -eu\n"
                   "sample=$1\n"
                   "shift\n"
                   "subnqn=\n"
                   "nsid=\n"
                   "nsze=\n"
                   "devpath=\n"
                   "next_nsid() {\n"
                   "  i=1\n"
                   "  while [ -e '%s/nvme1n'\"$i\" ]; do\n"
                   "    i=$((i + 1))\n"
                   "  done\n"
                   "  printf '%%s\\n' \"$i\"\n"
                   "}\n"
                   "echo run \"$sample\" \"$@\" >> '%s'\n"
                   "while [ $# -gt 0 ]; do\n"
                   "  case \"$1\" in\n"
                   "    --sub-nqn) subnqn=$2; shift 2 ;;\n"
                   "    --nsid) nsid=$2; shift 2 ;;\n"
                   "    --nsze) nsze=$2; shift 2 ;;\n"
                   "    --dev-path) devpath=$2; shift 2 ;;\n"
                   "    --dev-ip|--port) shift 2 ;;\n"
                   "    *) shift ;;\n"
                   "  esac\n"
                   "done\n"
                   "if [ \"$sample\" = './sample_create_attach' ]; then\n"
                   "  nsid=$(next_nsid)\n"
                   "  : > '%s/nvme1n'\"$nsid\"\n"
                   "  mkdir -p '%s'/\"$subnqn\"/namespaces/\"$nsid\"\n"
                   "  if [ -e '%s/fail-create-after-side-effects' ]; then\n"
                   "    rm -f '%s/fail-create-after-side-effects'\n"
                   "    exit 1\n"
                   "  fi\n"
                   "elif [ \"$sample\" = './sample_detach_delete' ]; then\n"
                   "  rm -f \"$devpath\"\n"
                   "  rm -rf '%s'/\"$subnqn\"/namespaces/\"$nsid\"\n"
                   "else\n"
                   "  exit 2\n"
                   "fi\n",
                   dev_dir, log_path, dev_dir, configfs_dir,
                   dev_dir, dev_dir, configfs_dir) != 0) {
        return 1;
    }

    if (write_file(sample_create, 0700, "#!/bin/sh\nexit 0\n") != 0 ||
        write_file(sample_delete, 0700, "#!/bin/sh\nexit 0\n") != 0) {
        return 1;
    }

    return 0;
}

static int run_plugin_flow(const char *prefix, const char *dev_dir,
                           const char *configfs_dir,
                           const char *log_path)
{
    ssu_lbc_mock_config_t config = {};
    ssu_lbc_mock_config_t bad_config = {};
    const ssu_plugin_ops_t *plugin;
    ssu_resource_info_t resources[3];
    uint32_t resource_count = 3;
    ssu_extent_create_req_t req = {};
    char ns_id[32];
    char recovered_ns_id[32];
    char dev_path[128];
    char expected_dev[512];
    char expected_recovered_dev[512];
    char failure_marker[512];

    bad_config.prefix = prefix;
    bad_config.dev_ip = "127.0.0.1";
    bad_config.port = 4420;
    bad_config.subnqn = "nqn.2025-01.io.ssu:too-long-for-limit";
    if (expect_err("long subnqn",
                   ssu_lbc_mock_configure(&bad_config),
                   SSU_ERR_INVALID) != 0) {
        return 1;
    }

    config.prefix = prefix;
    config.dev_ip = "127.0.0.1";
    config.port = 4420;
    config.subnqn = "nqn.2025-01.io.ssu:m0";
    config.dev_dir = dev_dir;
    config.configfs_dir = configfs_dir;
    config.log_file = log_path;
    if (expect_err("configure", ssu_lbc_mock_configure(&config),
                   SSU_OK) != 0) {
        return 1;
    }

    plugin = ssu_plugin_entry();
    if (plugin == NULL) {
        fputs("lbc mock plugin did not return ops\n", stderr);
        return 1;
    }

    memset(resources, 0, sizeof(resources));
    if (expect_err("discover", plugin->discover(resources, &resource_count),
                   SSU_OK) != 0) {
        return 1;
    }

    if (resource_count != 3 ||
        strcmp(resources[0].ssu_id, "lbc-mock-ssu0") != 0 ||
        strcmp(resources[1].ssu_id, "lbc-mock-ssu1") != 0 ||
        strcmp(resources[2].ssu_id, "lbc-mock-ssu2") != 0) {
        fputs("discover returned unexpected LBC mock resource\n", stderr);
        return 1;
    }

    req.allocate_id = "alloc-test";
    req.ssu_id = resources[0].ssu_id;
    req.length = 256ULL * 1024ULL * 1024ULL;
    req.phys_offset_hint = 128;
    req.policy = SSU_RELIABILITY_STRIPE;

    memset(ns_id, 0, sizeof(ns_id));
    uint64_t phys_sector = 99;
    if (expect_err("create ns",
                   plugin->create_ns(&req, ns_id, sizeof(ns_id),
                                     &phys_sector),
                   SSU_OK) != 0) {
        return 1;
    }

    if (strcmp(ns_id, "1") != 0 || phys_sector != 0) {
        fputs("create_ns returned unexpected ns metadata\n", stderr);
        return 1;
    }

    memset(dev_path, 0, sizeof(dev_path));
    if (expect_err("connect",
                   plugin->connect(resources[0].ssu_id, dev_path,
                                   sizeof(dev_path)),
                   SSU_OK) != 0) {
        return 1;
    }

    snprintf(expected_dev, sizeof(expected_dev), "%s/nvme1n1", dev_dir);
    if (strcmp(dev_path, expected_dev) != 0 || !path_exists(expected_dev)) {
        fprintf(stderr, "expected %s, got %s\n", expected_dev, dev_path);
        return 1;
    }

    snprintf(failure_marker, sizeof(failure_marker),
             "%s/fail-create-after-side-effects", dev_dir);
    if (write_file(failure_marker, 0600, "1\n") != 0) {
        return 1;
    }

    req.ssu_id = resources[1].ssu_id;
    req.logical_offset = req.length;
    memset(recovered_ns_id, 0, sizeof(recovered_ns_id));
    if (expect_err("recover create after script side effects",
                   plugin->create_ns(&req, recovered_ns_id,
                                     sizeof(recovered_ns_id), &phys_sector),
                   SSU_OK) != 0) {
        return 1;
    }

    snprintf(expected_recovered_dev, sizeof(expected_recovered_dev),
             "%s/nvme1n2", dev_dir);
    if (strcmp(recovered_ns_id, "2") != 0 ||
        !path_exists(expected_recovered_dev)) {
        fprintf(stderr, "expected recovered ns 2 at %s, got ns=%s\n",
                expected_recovered_dev, recovered_ns_id);
        return 1;
    }

    if (expect_err("delete recovered ns",
                   plugin->delete_ns(resources[1].ssu_id, recovered_ns_id),
                   SSU_OK) != 0) {
        return 1;
    }

    if (path_exists(expected_recovered_dev)) {
        fputs("recovered namespace should be deleted cleanly\n", stderr);
        return 1;
    }

    if (expect_err("mount",
                   plugin->mount("alloc-test", "nodeA", "/dev/ssu0"),
                   SSU_OK) != 0 ||
        expect_err("unmount", plugin->unmount("/dev/ssu0"),
                   SSU_OK) != 0) {
        return 1;
    }

    if (expect_err("stale unmount", plugin->unmount("/dev/ssu7"),
                   SSU_OK) != 0) {
        return 1;
    }

    if (expect_err("delete ns",
                   plugin->delete_ns(resources[0].ssu_id, ns_id),
                   SSU_OK) != 0) {
        return 1;
    }

    if (path_exists(expected_dev)) {
        fputs("detach/delete should remove the fake nvme namespace\n",
              stderr);
        return 1;
    }

    if (!file_contains(log_path, "setup nqn.2025-01.io.ssu:m0 4420") ||
        !file_contains(log_path, "--nsze 524288") ||
        !file_contains(log_path, "--nsid 1") ||
        !file_contains(log_path, expected_dev)) {
        fputs("LBC mock scripts did not receive expected arguments\n",
              stderr);
        return 1;
    }

    return 0;
}

static int run_real_plugin_flow(const char *prefix)
{
    ssu_lbc_mock_config_t config = {};
    const char *subnqn = "nqn.2025-01.io.ssu:m0";
    const ssu_plugin_ops_t *plugin;
    ssu_resource_info_t resources[3];
    uint32_t resource_count = 3;
    ssu_extent_create_req_t req = {};
    char ns_id[32];
    char dev_path[128];
    char ns_path[512];
    uint64_t phys_sector = 99;

    config.prefix = prefix;
    config.dev_ip = "127.0.0.1";
    config.port = 4420;
    config.subnqn = subnqn;
    if (expect_err("configure real prefix",
                   ssu_lbc_mock_configure(&config),
                   SSU_OK) != 0) {
        return 1;
    }

    plugin = ssu_plugin_entry();
    if (plugin == NULL) {
        fputs("lbc mock plugin did not return ops\n", stderr);
        return 1;
    }

    memset(resources, 0, sizeof(resources));
    if (expect_err("discover", plugin->discover(resources, &resource_count),
                   SSU_OK) != 0) {
        return 1;
    }

    req.allocate_id = "alloc-real";
    req.ssu_id = resources[0].ssu_id;
    req.length = 512ULL * 1024ULL * 1024ULL;
    req.policy = SSU_RELIABILITY_STRIPE;

    memset(ns_id, 0, sizeof(ns_id));
    if (expect_err("create ns",
                   plugin->create_ns(&req, ns_id, sizeof(ns_id),
                                     &phys_sector),
                   SSU_OK) != 0) {
        return 1;
    }

    if (phys_sector != 0) {
        fputs("real create_ns returned unexpected namespace-local LBA\n",
              stderr);
        return 1;
    }

    memset(dev_path, 0, sizeof(dev_path));
    if (expect_err("connect",
                   plugin->connect(resources[0].ssu_id, dev_path,
                                   sizeof(dev_path)),
                   SSU_OK) != 0) {
        return 1;
    }

    if (strncmp(dev_path, "/dev/nvme", 9) != 0 || !path_exists(dev_path)) {
        fprintf(stderr, "expected live /dev/nvme*n* path, got %s\n",
                dev_path);
        return 1;
    }

    if (block_device_size_bytes(dev_path) != 512ULL * 1024ULL * 1024ULL) {
        fprintf(stderr, "expected %s to match the 512M request\n",
                dev_path);
        return 1;
    }

    snprintf(ns_path, sizeof(ns_path),
             "/sys/kernel/config/nvmet/subsystems/%s/namespaces/%s",
             subnqn, ns_id);
    if (!path_exists(ns_path)) {
        fprintf(stderr, "expected configfs namespace %s\n", ns_path);
        return 1;
    }

    if (expect_err("delete ns",
                   plugin->delete_ns(resources[0].ssu_id, ns_id),
                   SSU_OK) != 0) {
        return 1;
    }

    if (!wait_path_gone(dev_path)) {
        fprintf(stderr, "expected %s to disappear after detach/delete\n",
                dev_path);
        return 1;
    }

    if (path_exists(ns_path)) {
        fprintf(stderr, "expected configfs namespace %s to be removed\n",
                ns_path);
        return 1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    char root[] = "/tmp/ssu-lbc-mock-test-XXXXXX";
    char prefix[512];
    char dev_dir[512];
    char configfs_dir[512];
    char log_path[512];

    if (argc == 3 && strcmp(argv[1], "--real-prefix") == 0) {
        return run_real_plugin_flow(argv[2]);
    }

    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    snprintf(prefix, sizeof(prefix), "%s/prefix", root);
    snprintf(dev_dir, sizeof(dev_dir), "%s/dev", root);
    snprintf(configfs_dir, sizeof(configfs_dir), "%s/configfs", root);
    snprintf(log_path, sizeof(log_path), "%s/lbc.log", root);

    if (create_fake_lbc_prefix(prefix, dev_dir, configfs_dir, log_path) != 0) {
        return 1;
    }

    return run_plugin_flow(prefix, dev_dir, configfs_dir, log_path);
}
