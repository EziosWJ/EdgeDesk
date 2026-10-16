#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// CPU / load / information parsing. Pure functions take file contents so unit
// tests do not depend on the host machine state.

namespace edgedesk {

struct CpuCounters {
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
    std::uint64_t steal = 0;

    std::uint64_t total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
    std::uint64_t busy() const { return total() - idle - iowait; }
};

// Parses the aggregate "cpu ..." line of /proc/stat.
bool parse_proc_stat_cpu(std::string_view content, CpuCounters& out);

struct LoadAverage {
    double load1 = 0.0;
    double load5 = 0.0;
    double load15 = 0.0;
    bool valid = false;
};

bool parse_loadavg(std::string_view content, LoadAverage& out);

// Percentage 0..100 from two counter snapshots; nullopt when deltas are zero.
std::optional<double> cpu_usage_percent(const CpuCounters& previous, const CpuCounters& current);

// Uptime seconds from /proc/uptime.
bool parse_uptime(std::string_view content, double& seconds);

} // namespace edgedesk
