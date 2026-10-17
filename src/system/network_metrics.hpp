#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Network counters from /proc/net/dev, IP addresses from getifaddrs, and rate
// calculation.

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
    std::vector<std::string> addresses;
    double rx_bytes_per_second = 0.0;
    double tx_bytes_per_second = 0.0;
    std::uint64_t rx_bytes_total = 0;
    std::uint64_t tx_bytes_total = 0;
};

struct NetInterfaceAddress {
    std::string name;
    std::vector<std::string> addresses;
};

// Parses /proc/net/dev, skipping the loopback interface.
bool parse_proc_net_dev(std::string_view content, std::vector<NetInterfaceCounters>& out);

// Rates in bytes/second between two samples; nullopt-like empty vector when
// the interfaces cannot be matched.
std::vector<NetInterfaceRate> compute_network_rates(const std::vector<NetInterfaceCounters>& previous,
                                                    const std::vector<NetInterfaceCounters>& current,
                                                    double elapsed_seconds);

// IPv4/IPv6 addresses configured on the host, excluding loopback interfaces.
// Uses getifaddrs() so no external command is required.
std::vector<NetInterfaceAddress> read_interface_addresses();

// Formats an IPv4 or IPv6 address for display. Returns an empty string when
// the family/address pair is invalid.
std::string format_ip_address(int family, const void* address);

} // namespace edgedesk
