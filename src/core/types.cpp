#include "core/types.hpp"

#include <algorithm>
#include <cctype>

namespace edgedesk {

bool parse_uint64(std::string_view text, std::uint64_t& out) {
    if (text.empty() || text.size() > 20) return false;
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool parse_byte_size(std::string_view text, std::uint64_t& out) {
    if (text.empty()) return false;
    std::uint64_t multiplier = 1;
    switch (text.back()) {
        case 'k': case 'K': multiplier = kKiB; text.remove_suffix(1); break;
        case 'm': case 'M': multiplier = kMiB; text.remove_suffix(1); break;
        case 'g': case 'G': multiplier = kGiB; text.remove_suffix(1); break;
        default: break;
    }
    std::uint64_t base = 0;
    if (!parse_uint64(text, base)) return false;
    if (base > UINT64_MAX / multiplier) return false;
    out = base * multiplier;
    return true;
}

bool is_loopback_host(std::string_view host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1" ||
           host == "0:0:0:0:0:0:0:1";
}

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const auto next = text.find(separator, pos);
        const auto end = next == std::string_view::npos ? text.size() : next;
        if (end > pos) parts.emplace_back(text.substr(pos, end - pos));
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }
    return parts;
}

std::string to_lower_ascii(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

bool is_reserved_dir_name(std::string_view name) {
    return name == "." || name == "..";
}

} // namespace edgedesk
