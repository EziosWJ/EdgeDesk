#include "system/network_metrics.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <map>
#include <netinet/in.h>

namespace edgedesk {
namespace {

bool read_u64(std::string_view text, std::size_t& pos, std::uint64_t& out) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
    std::uint64_t value = 0;
    bool any = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        any = true;
        value = value * 10 + static_cast<std::uint64_t>(text[pos] - '0');
        ++pos;
    }
    out = value;
    return any;
}

struct IfAddrsGuard {
    ifaddrs* list = nullptr;
    ~IfAddrsGuard() {
        if (list != nullptr) ::freeifaddrs(list);
    }
};

} // namespace

std::string format_ip_address(int family, const void* address) {
    if (address == nullptr || (family != AF_INET && family != AF_INET6)) return {};
    char text[INET6_ADDRSTRLEN] = {0};
    if (::inet_ntop(family, address, text, sizeof(text)) == nullptr) return {};
    return text;
}

std::vector<NetInterfaceAddress> read_interface_addresses() {
    std::vector<NetInterfaceAddress> result;
    IfAddrsGuard guard;
    if (::getifaddrs(&guard.list) != 0) return result;

    std::map<std::string, std::vector<std::string>> addresses_by_name;
    for (const ifaddrs* item = guard.list; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || item->ifa_name == nullptr) continue;
        const std::string name = item->ifa_name;
        if (name == "lo") continue;
        const int family = item->ifa_addr->sa_family;
        const void* address = nullptr;
        if (family == AF_INET) {
            address = &reinterpret_cast<const sockaddr_in*>(item->ifa_addr)->sin_addr;
        } else if (family == AF_INET6) {
            address = &reinterpret_cast<const sockaddr_in6*>(item->ifa_addr)->sin6_addr;
        } else {
            continue;
        }
        const std::string text = format_ip_address(family, address);
        if (text.empty()) continue;
        auto& addresses = addresses_by_name[name];
        if (std::find(addresses.begin(), addresses.end(), text) == addresses.end()) {
            addresses.push_back(text);
        }
    }

    result.reserve(addresses_by_name.size());
    for (auto& entry : addresses_by_name) {
        // IPv4 first so the compact UI can show the most familiar address.
        std::stable_sort(entry.second.begin(), entry.second.end(),
                         [](const std::string& left, const std::string& right) {
                             const bool left_v4 = left.find(':') == std::string::npos;
                             const bool right_v4 = right.find(':') == std::string::npos;
                             if (left_v4 != right_v4) return left_v4;
                             return left < right;
                         });
        NetInterfaceAddress interface;
        interface.name = entry.first;
        interface.addresses = std::move(entry.second);
        if (!interface.addresses.empty()) result.push_back(std::move(interface));
    }
    return result;
}

bool parse_proc_net_dev(std::string_view content, std::vector<NetInterfaceCounters>& out) {
    out.clear();
    std::size_t pos = 0;
    while (pos < content.size()) {
        const auto end = content.find('\n', pos);
        auto line = content.substr(pos, end == std::string_view::npos ? content.size() - pos : end - pos);
        pos = end == std::string_view::npos ? content.size() : end + 1;
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        std::string name = std::string(line.substr(0, colon));
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        if (name.empty() || name == "lo" || name.rfind("lo:", 0) == 0) continue;
        std::size_t field_pos = colon + 1;
        std::uint64_t rx_bytes = 0;
        std::uint64_t rx_packets = 0;
        std::uint64_t tx_bytes = 0;
        std::uint64_t tx_packets = 0;
        std::uint64_t scratch = 0;
        if (!read_u64(line, field_pos, rx_bytes)) continue;
        if (!read_u64(line, field_pos, rx_packets)) continue;
        // Skip rx errs/drop/fifo/frame/compressed/multicast (6 fields).
        for (int index = 0; index < 6; ++index) {
            if (!read_u64(line, field_pos, scratch)) break;
        }
        if (!read_u64(line, field_pos, tx_bytes)) continue;
        if (!read_u64(line, field_pos, tx_packets)) continue;
        NetInterfaceCounters counters;
        counters.name = name;
        counters.rx_bytes = rx_bytes;
        counters.tx_bytes = tx_bytes;
        counters.rx_packets = rx_packets;
        counters.tx_packets = tx_packets;
        out.push_back(std::move(counters));
    }
    return true;
}

std::vector<NetInterfaceRate> compute_network_rates(const std::vector<NetInterfaceCounters>& previous,
                                                    const std::vector<NetInterfaceCounters>& current,
                                                    double elapsed_seconds) {
    std::vector<NetInterfaceRate> rates;
    if (elapsed_seconds <= 0.0) return rates;
    std::map<std::string, const NetInterfaceCounters*> previous_by_name;
    for (const auto& counters : previous) {
        previous_by_name[counters.name] = &counters;
    }
    for (const auto& counters : current) {
        NetInterfaceRate rate;
        rate.name = counters.name;
        rate.rx_bytes_total = counters.rx_bytes;
        rate.tx_bytes_total = counters.tx_bytes;
        const auto it = previous_by_name.find(counters.name);
        if (it != previous_by_name.end()) {
            const auto* before = it->second;
            const double rx_delta = counters.rx_bytes >= before->rx_bytes
                                        ? static_cast<double>(counters.rx_bytes - before->rx_bytes)
                                        : 0.0;
            const double tx_delta = counters.tx_bytes >= before->tx_bytes
                                        ? static_cast<double>(counters.tx_bytes - before->tx_bytes)
                                        : 0.0;
            rate.rx_bytes_per_second = rx_delta / elapsed_seconds;
            rate.tx_bytes_per_second = tx_delta / elapsed_seconds;
        }
        rates.push_back(std::move(rate));
    }
    return rates;
}

} // namespace edgedesk
