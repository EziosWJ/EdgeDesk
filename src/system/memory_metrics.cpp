#include "system/memory_metrics.hpp"

#include <cstdlib>

namespace edgedesk {
namespace {

struct MemField {
    const char* name;
    std::uint64_t MemoryInfo::*member;
};

const MemField kFields[] = {
    {"MemTotal", &MemoryInfo::total_kb},
    {"MemAvailable", &MemoryInfo::available_kb},
    {"MemFree", &MemoryInfo::free_kb},
    {"Buffers", &MemoryInfo::buffers_kb},
    {"Cached", &MemoryInfo::cached_kb},
    {"SwapTotal", &MemoryInfo::swap_total_kb},
    {"SwapFree", &MemoryInfo::swap_free_kb},
};

} // namespace

bool parse_meminfo(std::string_view content, MemoryInfo& out) {
    std::size_t pos = 0;
    bool saw_total = false;
    while (pos < content.size()) {
        const auto end = content.find('\n', pos);
        const auto line = content.substr(pos, end == std::string_view::npos ? content.size() - pos : end - pos);
        pos = end == std::string_view::npos ? content.size() : end + 1;
        if (line.empty()) continue;
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const auto name = line.substr(0, colon);
        std::size_t value_pos = colon + 1;
        while (value_pos < line.size() && line[value_pos] == ' ') ++value_pos;
        std::uint64_t value = 0;
        bool any = false;
        while (value_pos < line.size() && line[value_pos] >= '0' && line[value_pos] <= '9') {
            any = true;
            value = value * 10 + static_cast<std::uint64_t>(line[value_pos] - '0');
            ++value_pos;
        }
        if (!any) continue;
        for (const auto& field : kFields) {
            if (name == field.name) {
                out.*(field.member) = value;
                if (name == "MemTotal") saw_total = true;
            }
        }
    }
    out.valid = saw_total;
    if (out.valid && out.available_kb == 0) {
        // Older kernels: approximate available from free + buffers + cached.
        out.available_kb = out.free_kb + out.buffers_kb + out.cached_kb;
    }
    return out.valid;
}

} // namespace edgedesk
