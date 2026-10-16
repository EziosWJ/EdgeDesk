#include "system/disk_metrics.hpp"

#include <sys/statvfs.h>

#include <algorithm>
#include <set>

namespace edgedesk {
namespace {

std::string unescape_mount_field(std::string_view field) {
    // /proc/mounts escapes space, tab, newline and backslash as octal.
    std::string out;
    out.reserve(field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size() && field[i + 1] >= '0' && field[i + 1] <= '7' &&
            field[i + 2] >= '0' && field[i + 2] <= '7' && field[i + 3] >= '0' && field[i + 3] <= '7') {
            const int value = (field[i + 1] - '0') * 64 + (field[i + 2] - '0') * 8 + (field[i + 3] - '0');
            out.push_back(static_cast<char>(value));
            i += 3;
        } else {
            out.push_back(field[i]);
        }
    }
    return out;
}

} // namespace

bool is_pseudo_filesystem(std::string_view filesystem) {
    static const std::set<std::string> kPseudo = {
        "proc",      "sysfs",     "devtmpfs", "devpts",   "tmpfs",    "cgroup",   "cgroup2",
        "overlay",   "squashfs",  "ramfs",    "debugfs",  "tracefs",  "securityfs",
        "pstore",    "bpf",       "autofs",   "mqueue",   "hugetlbfs", "fusectl",
        "configfs",  "binfmt_misc", "efivarfs", "nsfs",   "selinuxfs", "rpc_pipefs",
    };
    return kPseudo.count(std::string(filesystem)) > 0;
}

std::vector<DiskUsage> parse_proc_mounts(std::string_view content) {
    std::vector<DiskUsage> mounts;
    std::set<std::string> seen;
    std::size_t pos = 0;
    while (pos < content.size()) {
        const auto end = content.find('\n', pos);
        const auto line = content.substr(pos, end == std::string_view::npos ? content.size() - pos : end - pos);
        pos = end == std::string_view::npos ? content.size() : end + 1;
        if (line.empty()) continue;
        // device mountpoint fstype options dump pass
        std::string fields[6];
        std::size_t field_index = 0;
        std::size_t field_start = 0;
        for (std::size_t i = 0; i <= line.size() && field_index < 6; ++i) {
            if (i == line.size() || line[i] == ' ') {
                if (field_index < 6) fields[field_index++] = std::string(line.substr(field_start, i - field_start));
                field_start = i + 1;
            }
        }
        if (field_index < 3) continue;
        const std::string& device = fields[0];
        const std::string mount = unescape_mount_field(fields[1]);
        const std::string filesystem = fields[2];
        if (mount.empty()) continue;
        if (is_pseudo_filesystem(filesystem)) continue;
        if (device.rfind("/dev/", 0) != 0) continue;
        if (mount.rfind("/proc", 0) == 0 || mount.rfind("/sys", 0) == 0 || mount.rfind("/dev", 0) == 0) {
            if (mount == "/dev" || mount.rfind("/dev/", 0) == 0 || mount == "/proc" || mount == "/sys") continue;
        }
        if (!seen.insert(mount).second) continue;
        DiskUsage usage;
        usage.mount = mount;
        usage.filesystem = filesystem;
        mounts.push_back(std::move(usage));
    }
    std::sort(mounts.begin(), mounts.end(),
              [](const DiskUsage& a, const DiskUsage& b) { return a.mount < b.mount; });
    return mounts;
}

bool read_mount_capacity(const std::string& mount, DiskUsage& usage) {
    struct statvfs info {};
    if (::statvfs(mount.c_str(), &info) != 0) return false;
    const auto fragment = static_cast<std::uint64_t>(info.f_frsize != 0 ? info.f_frsize : info.f_bsize);
    usage.mount = mount;
    usage.total_bytes = static_cast<std::uint64_t>(info.f_blocks) * fragment;
    usage.available_bytes = static_cast<std::uint64_t>(info.f_bavail) * fragment;
    const auto free_blocks = static_cast<std::uint64_t>(info.f_bfree);
    const auto used_blocks = static_cast<std::uint64_t>(info.f_blocks) > free_blocks
                                 ? static_cast<std::uint64_t>(info.f_blocks) - free_blocks
                                 : 0;
    usage.used_bytes = used_blocks * fragment;
    return true;
}

} // namespace edgedesk
