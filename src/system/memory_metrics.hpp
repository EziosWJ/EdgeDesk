#pragma once

#include <cstdint>
#include <string_view>

// /proc/meminfo parsing. Values are kept in KiB exactly as the kernel reports
// them; derived percentages live in the HTTP layer.

namespace edgedesk {

struct MemoryInfo {
    bool valid = false;
    std::uint64_t total_kb = 0;
    std::uint64_t available_kb = 0;
    std::uint64_t free_kb = 0;
    std::uint64_t buffers_kb = 0;
    std::uint64_t cached_kb = 0;
    std::uint64_t swap_total_kb = 0;
    std::uint64_t swap_free_kb = 0;

    std::uint64_t used_kb() const {
        const std::uint64_t available = available_kb <= total_kb ? available_kb : total_kb;
        return total_kb - available;
    }
};

bool parse_meminfo(std::string_view content, MemoryInfo& out);

} // namespace edgedesk
