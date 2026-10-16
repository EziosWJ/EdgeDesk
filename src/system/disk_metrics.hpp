#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Disk capacity metrics: /proc/mounts filtering + statvfs.

namespace edgedesk {

struct DiskUsage {
    std::string mount;
    std::string filesystem;
    std::uint64_t total_bytes = 0;
    std::uint64_t available_bytes = 0; // unprivileged available
    std::uint64_t used_bytes = 0;
};

// Extracts real block-device mount points from /proc/mounts content.
// Pseudo filesystems (proc, sysfs, tmpfs, overlay, ...) are excluded.
std::vector<DiskUsage> parse_proc_mounts(std::string_view content);

// Fills capacity fields for one mount; returns false when statvfs fails.
bool read_mount_capacity(const std::string& mount, DiskUsage& usage);

bool is_pseudo_filesystem(std::string_view filesystem);

} // namespace edgedesk
