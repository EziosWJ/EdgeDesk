#include "system/cpu_metrics.hpp"

#include <cstdio>
#include <cstdlib>

namespace edgedesk {
namespace {

bool read_u64_prefix(std::string_view text, std::size_t& pos, std::uint64_t& out) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
    std::uint64_t value = 0;
    bool any = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        any = true;
        value = value * 10 + static_cast<std::uint64_t>(text[pos] - '0');
        ++pos;
    }
    if (!any) return false;
    out = value;
    return true;
}

} // namespace

bool parse_proc_stat_cpu(std::string_view content, CpuCounters& out) {
    const auto line_end = content.find('\n');
    auto line = content.substr(0, line_end == std::string_view::npos ? content.size() : line_end);
    if (line.rfind("cpu", 0) != 0) return false;
    if (line.size() > 3 && line[3] != ' ' && line[3] != '\t') return false; // "cpu0" etc.
    std::size_t pos = 3;
    std::uint64_t values[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::size_t count = 0;
    while (count < 8 && read_u64_prefix(line, pos, values[count])) ++count;
    if (count < 4) return false;
    out.user = values[0];
    out.nice = values[1];
    out.system = values[2];
    out.idle = values[3];
    out.iowait = count > 4 ? values[4] : 0;
    out.irq = count > 5 ? values[5] : 0;
    out.softirq = count > 6 ? values[6] : 0;
    out.steal = count > 7 ? values[7] : 0;
    return true;
}

bool parse_loadavg(std::string_view content, LoadAverage& out) {
    const auto line = content.substr(0, content.find('\n'));
    double load1 = 0.0;
    double load5 = 0.0;
    double load15 = 0.0;
    const std::string text(line);
    if (std::sscanf(text.c_str(), "%lf %lf %lf", &load1, &load5, &load15) != 3) return false;
    out.load1 = load1;
    out.load5 = load5;
    out.load15 = load15;
    out.valid = true;
    return true;
}

std::optional<double> cpu_usage_percent(const CpuCounters& previous, const CpuCounters& current) {
    const std::uint64_t total_now = current.total();
    const std::uint64_t total_before = previous.total();
    if (total_now <= total_before) return std::nullopt;
    const auto total_delta = total_now - total_before;
    const std::uint64_t busy_now = current.busy();
    const std::uint64_t busy_before = previous.busy();
    const double busy_delta = busy_now >= busy_before ? static_cast<double>(busy_now - busy_before) : 0.0;
    const double usage = 100.0 * busy_delta / static_cast<double>(total_delta);
    return usage < 0.0 ? 0.0 : (usage > 100.0 ? 100.0 : usage);
}

bool parse_uptime(std::string_view content, double& seconds) {
    const auto line = content.substr(0, content.find('\n'));
    const std::string text(line);
    double value = 0.0;
    if (std::sscanf(text.c_str(), "%lf", &value) != 1 || value < 0.0) return false;
    seconds = value;
    return true;
}

} // namespace edgedesk
