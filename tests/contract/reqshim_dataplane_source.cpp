#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static std::string read_file(const std::string &path)
{
    std::ifstream in(path);
    std::ostringstream out;

    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        return "";
    }

    out << in.rdbuf();
    return out.str();
}

static int expect_contains(const std::string &label,
                           const std::string &text,
                           const std::string &needle)
{
    if (text.find(needle) != std::string::npos) {
        return 0;
    }

    std::cerr << label << " missing " << needle << "\n";
    return 1;
}

static int expect_not_contains(const std::string &label,
                               const std::string &text,
                               const std::string &needle)
{
    if (text.find(needle) == std::string::npos) {
        return 0;
    }

    std::cerr << label << " should not contain " << needle << "\n";
    return 1;
}

int main(int argc, char **argv)
{
    std::string root;
    std::string blk;
    std::string cmd;
    std::string map;
    std::string phys;
    std::string main;
    int failed = 0;

    if (argc != 2) {
        std::cerr << "usage: reqshim_dataplane_source SOURCE_ROOT\n";
        return 1;
    }

    root = argv[1];
    blk = read_file(root + "/src/kernel/reqshim/reqshim_blk.c");
    cmd = read_file(root + "/src/kernel/reqshim/reqshim_cmd.c");
    map = read_file(root + "/src/kernel/reqshim/reqshim_map.c");
    phys = read_file(root + "/src/kernel/reqshim/reqshim_phys.c");
    main = read_file(root + "/src/kernel/reqshim/reqshim_main.c");

    if (blk.empty() || cmd.empty() || map.empty() || phys.empty() ||
        main.empty()) {
        return 1;
    }

    failed |= expect_contains("reqshim_main", main, "ssu_reqshim_blk_init");
    failed |= expect_contains("reqshim_main", main, "ssu_reqshim_blk_exit");
    failed |= expect_contains("reqshim_main", main, "module_param_named(trace_io");
    failed |= expect_contains("reqshim_blk", blk, "register_blkdev");
    failed |= expect_contains("reqshim_blk", blk, "\"ssu/%s\"");
    failed |= expect_contains("reqshim_blk", blk, "blk_mq_ops");
    failed |= expect_contains("reqshim_blk", blk, "queue_rq");
    failed |= expect_contains("reqshim_blk", blk, "blk_mq_alloc_disk");
    failed |= expect_contains("reqshim_blk", blk, "queue_rq minor=%u");
    failed |= expect_contains("reqshim_blk", blk, "submit chunk minor=%u");
    failed |= expect_contains("reqshim_cmd", cmd, "translate miss minor=%u");
    failed |= expect_contains("reqshim_cmd", cmd, "translate hit minor=%u");
    failed |= expect_contains("reqshim_map", map, "map add minor=%u");
    failed |= expect_contains("reqshim_map", map, "map query miss minor=%u");
    failed |= expect_contains("reqshim_phys", phys, "submit_bio_wait");
    failed |= expect_contains("reqshim_phys", phys, "bdev_open_by_path");
    failed |= expect_contains("reqshim_phys", phys, "bdev_release");
    failed |= expect_contains("reqshim_phys", phys, "phys submit dev=%s");
    failed |= expect_contains("reqshim_phys", phys, "phys submit done dev=%s");
    failed |= expect_not_contains("reqshim_phys", phys,
                                  "bdev_file_open_by_path");
    failed |= expect_not_contains("reqshim_phys", phys, "file_bdev");

    return failed == 0 ? 0 : 1;
}
