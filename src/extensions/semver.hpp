#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Strict SemVer 2.0.0 parsing and precedence (build metadata ignored).
// Used for manifest versions and the host-API compatibility range.

namespace edgedesk::extensions {

struct SemVer {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    std::vector<std::string> prerelease;
    std::string build;

    // Returns false and sets `error` for anything that is not a complete,
    // canonical SemVer (leading zeroes, empty identifiers, 'v' prefix, ...).
    static bool parse(std::string_view text, SemVer& out, std::string& error);
    std::string to_string() const;
};

// -1, 0, 1; build metadata is ignored.
int compare(const SemVer& a, const SemVer& b);

inline bool operator<(const SemVer& a, const SemVer& b) { return compare(a, b) < 0; }
inline bool operator<=(const SemVer& a, const SemVer& b) { return compare(a, b) <= 0; }
inline bool operator>(const SemVer& a, const SemVer& b) { return compare(a, b) > 0; }
inline bool operator>=(const SemVer& a, const SemVer& b) { return compare(a, b) >= 0; }

// Host compatibility: min <= host <= max (both endpoints inclusive). The
// caller must have parsed all three values; malformed ranges are never
// interpreted as compatible.
bool host_api_compatible(const SemVer& host, const SemVer& min, const SemVer& max);

} // namespace edgedesk::extensions
