#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Network counters from /proc/net/dev plus rate calculation.

namespace edgedesk {

struct NetInterfaceCounters {
    std::string name;
    std::uint64_t rx_bytes = 0;
    std::uint64_t tx_bytes = 0;
    std::uint64_t rx_packets = 0;
    std::uint64_t tx_packets = 0;
};

struct NetInterfaceRate {
    std::string name;
    double rx_bytes_per_second = 0.0;
    double tx_bytes_per_second = 0.0;
    std::uint64_t rx_bytes_total = 0;
    std::uint64_t tx_bytes_total = 0;
};

// Parses /proc/net/dev, skipping the loopback interface.
bool parse_proc_net_dev(std::string_view content, std::vector<NetInterfaceCounters>& out);

// Rates in bytes/second between two samples; nullopt-like empty vector when
// the interfaces cannot be matched.
std::vector<NetInterfaceRate> compute_network_rates(const std::vector<NetInterfaceCounters>& previous,
                                                    const std::vector<NetInterfaceCounters>& current,
                                                    double elapsed_seconds);

} // namespace edgedesk
