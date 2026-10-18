#include "extensions/semver.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace edgedesk::extensions {
namespace {

bool all_digits(std::string_view text) {
    if (text.empty()) return false;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
    }
    return true;
}

bool parse_number(std::string_view text, std::uint32_t& out) {
    if (!all_digits(text)) return false;
    if (text.size() > 1 && text.front() == '0') return false; // canonical SemVer
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
    if (value > std::numeric_limits<std::uint32_t>::max()) return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool valid_identifier(std::string_view text, bool numeric_rule) {
    if (text.empty()) return false;
    for (const char ch : text) {
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '-';
        if (!ok) return false;
    }
    if (numeric_rule && all_digits(text) && text.size() > 1 && text.front() == '0') return false;
    return true;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (;;) {
        const auto pos = text.find(separator, start);
        parts.push_back(text.substr(start, pos == std::string_view::npos ? text.size() - start : pos - start));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return parts;
}

int compare_identifier(std::string_view a, std::string_view b) {
    const bool an = all_digits(a);
    const bool bn = all_digits(b);
    if (an && bn) {
        // Numeric identifiers have no leading zeroes after validation; compare
        // by length first so arbitrarily large values stay well-defined.
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        const int cmp = a.compare(b);
        return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
    }
    if (an != bn) return an ? -1 : 1; // numeric identifiers sort before alphanumeric
    const int cmp = a.compare(b);
    return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
}

} // namespace

bool SemVer::parse(std::string_view text, SemVer& out, std::string& error) {
    out = {};
    if (text.empty()) {
        error = "version must not be empty";
        return false;
    }
    if (text.size() > 256) {
        error = "version is too long";
        return false;
    }
    if (text.front() == 'v' || text.front() == 'V') {
        error = "version must not have a 'v' prefix";
        return false;
    }
    std::string_view core = text;
    const auto plus = text.find('+');
    if (plus != std::string_view::npos) {
        core = text.substr(0, plus);
        const auto build = text.substr(plus + 1);
        if (build.empty()) {
            error = "empty build metadata";
            return false;
        }
        for (const auto part : split(build, '.')) {
            if (!valid_identifier(part, false)) {
                error = "invalid build metadata";
                return false;
            }
        }
        out.build = std::string(build);
    }
    const auto dash = core.find('-');
    const std::string_view numeric = dash == std::string_view::npos ? core : core.substr(0, dash);
    if (dash != std::string_view::npos) {
        const auto prerelease = core.substr(dash + 1);
        if (prerelease.empty()) {
            error = "empty pre-release";
            return false;
        }
        for (const auto part : split(prerelease, '.')) {
            if (!valid_identifier(part, true)) {
                error = "invalid pre-release identifier";
                return false;
            }
            out.prerelease.emplace_back(part);
        }
    }
    const auto numbers = split(numeric, '.');
    if (numbers.size() != 3) {
        error = "version must have major.minor.patch";
        return false;
    }
    if (!parse_number(numbers[0], out.major) || !parse_number(numbers[1], out.minor) ||
        !parse_number(numbers[2], out.patch)) {
        error = "invalid numeric version component";
        return false;
    }
    error.clear();
    return true;
}

std::string SemVer::to_string() const {
    std::string out = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (!prerelease.empty()) {
        out += "-";
        for (std::size_t i = 0; i < prerelease.size(); ++i) {
            if (i > 0) out += ".";
            out += prerelease[i];
        }
    }
    if (!build.empty()) out += "+" + build;
    return out;
}

int compare(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.prerelease.empty() && b.prerelease.empty()) return 0;
    if (a.prerelease.empty()) return 1;  // release > pre-release
    if (b.prerelease.empty()) return -1;
    const std::size_t common = std::min(a.prerelease.size(), b.prerelease.size());
    for (std::size_t i = 0; i < common; ++i) {
        const int cmp = compare_identifier(a.prerelease[i], b.prerelease[i]);
        if (cmp != 0) return cmp;
    }
    if (a.prerelease.size() == b.prerelease.size()) return 0;
    return a.prerelease.size() < b.prerelease.size() ? -1 : 1;
}

bool host_api_compatible(const SemVer& host, const SemVer& min, const SemVer& max) {
    return compare(min, host) <= 0 && compare(host, max) <= 0;
}

} // namespace edgedesk::extensions
