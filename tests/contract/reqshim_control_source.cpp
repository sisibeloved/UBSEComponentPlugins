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
    std::string iface;
    std::string ioctl;
    std::string main;
    std::string mock;
    std::string lbc_mock;
    int failed = 0;

    if (argc != 2) {
        std::cerr << "usage: reqshim_control_source SOURCE_ROOT\n";
        return 1;
    }

    root = argv[1];
    iface = read_file(root + "/src/user/plugin/reqshim_iface.cpp");
    ioctl = read_file(root + "/src/kernel/reqshim/reqshim_ioctl.c");
    main = read_file(root + "/src/kernel/reqshim/reqshim_main.c");
    mock = read_file(root + "/src/user/plugin/vendors/mock/ssu_plugin_mock.cpp");
    lbc_mock = read_file(root + "/src/user/plugin/vendors/lbc_mock/ssu_plugin_lbc_mock.cpp");

    if (ioctl.empty() || main.empty() || mock.empty() || lbc_mock.empty()) {
        return 1;
    }

    failed |= expect_contains("reqshim_iface", iface,
                              "SSU_REQSHIM_DEFAULT_CTL_PATH");
    failed |= expect_contains("reqshim_iface", iface,
                              "SSU_IOC_GET_VERSION");
    failed |= expect_contains("reqshim_main", main, "ssu-ctl");
    failed |= expect_not_contains("reqshim_main", main, "ssu/ctl");
    failed |= expect_contains("reqshim_iface", iface,
                              "SSU_IOC_LOGDEV_CREATE");
    failed |= expect_contains("reqshim_iface", iface,
                              "SSU_IOC_MAP_ADD");
    failed |= expect_contains("reqshim_iface", iface,
                              "SSU_IOC_MAP_DEL");
    failed |= expect_contains("reqshim_iface", iface,
                              "SSU_IOC_LOGDEV_DESTROY");
    failed |= expect_contains("reqshim_ioctl", ioctl,
                              "#include <linux/uaccess.h>");
    failed |= expect_not_contains("reqshim_ioctl", ioctl,
                                  "#include <linux/copy_to_user.h>");
    failed |= expect_contains("mock plugin", mock,
                              "ssu_reqshim_mount_logdev");
    failed |= expect_contains("mock plugin", mock,
                              "ssu_reqshim_unmount_logdev");
    failed |= expect_contains("lbc_mock plugin", lbc_mock,
                              "ssu_reqshim_mount_logdev");
    failed |= expect_contains("lbc_mock plugin", lbc_mock,
                              "ssu_reqshim_unmount_logdev");
    failed |= expect_contains("lbc_mock plugin", lbc_mock,
                              "logical_offset");
    failed |= expect_contains("lbc_mock plugin", lbc_mock,
                              "phys_sector");
    failed |= expect_not_contains("lbc_mock plugin", lbc_mock,
                                  "/dev/ssu/ctl");

    return failed == 0 ? 0 : 1;
}
