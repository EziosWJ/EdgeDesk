#include "extensions/manifest.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>

namespace edgedesk::extensions {
namespace {

constexpr std::size_t kMaxNameBytes = 120;
constexpr std::size_t kMaxDescriptionBytes = 500;
constexpr std::size_t kMaxPublisherBytes = 200;
constexpr std::size_t kMaxIconBytes = 256;
constexpr std::int32_t kMaxWindowDimension = 10000;

bool has_control_characters(std::string_view text) {
    for (const char ch : text) {
        const auto value = static_cast<unsigned char>(ch);
        if (value < 0x20 || value == 0x7f) return true;
    }
    return false;
}

std::string trim_ascii(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return std::string(text.substr(begin, end - begin));
}

bool valid_dns_host(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    std::size_t start = 0;
    for (;;) {
        const auto dot = host.find('.', start);
        const auto label = host.substr(start, dot == std::string_view::npos ? host.size() - start : dot - start);
        if (label.empty() || label.size() > 63) return false;
        if (label.front() == '-' || label.back() == '-') return false;
        for (const char ch : label) {
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
            if (!ok) return false;
        }
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return true;
}

bool valid_ipv4(std::string_view host) {
    unsigned char buffer[4] = {};
    const std::string text(host);
    return ::inet_pton(AF_INET, text.c_str(), buffer) == 1;
}

bool valid_ipv6(std::string_view host) {
    unsigned char buffer[16] = {};
    const std::string text(host);
    return ::inet_pton(AF_INET6, text.c_str(), buffer) == 1;
}

bool parse_port(std::string_view text, std::uint16_t& out) {
    if (text.empty() || text.size() > 5) return false;
    std::uint32_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + static_cast<std::uint32_t>(ch - '0');
    }
    if (value == 0 || value > 65535) return false;
    out = static_cast<std::uint16_t>(value);
    return true;
}

bool safe_relative_asset_path(std::string_view path) {
    if (path.empty() || path.size() > kMaxIconBytes || path.front() == '/') return false;
    if (has_control_characters(path) || path.find('\\') != std::string_view::npos) return false;
    std::size_t start = 0;
    for (;;) {
        const auto slash = path.find('/', start);
        const auto part = path.substr(start, slash == std::string_view::npos ? path.size() - start : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

bool read_dimension(const nlohmann::json& object, const char* key, std::int32_t fallback, std::int32_t& out,
                    std::string& error) {
    if (!object.contains(key)) {
        out = fallback;
        return true;
    }
    const auto& value = object[key];
    if (!value.is_number_integer()) {
        error = std::string("window.") + key + " must be an integer";
        return false;
    }
    const auto number = value.get<std::int64_t>();
    if (number < 1 || number > kMaxWindowDimension) {
        error = std::string("window.") + key + " must be between 1 and " + std::to_string(kMaxWindowDimension);
        return false;
    }
    out = static_cast<std::int32_t>(number);
    return true;
}

} // namespace

const char* permission_name(Permission permission) {
    switch (permission) {
        case Permission::storage: return "storage";
        case Permission::files_manage: return "files.manage";
        case Permission::terminal_access: return "terminal.access";
        case Permission::network: return "network";
    }
    return "unknown";
}

bool parse_permission(std::string_view name, Permission& out) {
    if (name == "storage") {
        out = Permission::storage;
        return true;
    }
    if (name == "files.manage") {
        out = Permission::files_manage;
        return true;
    }
    if (name == "terminal.access") {
        out = Permission::terminal_access;
        return true;
    }
    if (name == "network") {
        out = Permission::network;
        return true;
    }
    return false;
}

bool permission_from_json(const nlohmann::json& value, Permission& out) {
    return value.is_string() && parse_permission(value.get<std::string>(), out);
}

bool valid_app_id(std::string_view id, std::string& error) {
    if (id.size() < 3 || id.size() > 255) {
        error = "application id length is invalid";
        return false;
    }
    std::size_t labels = 0;
    std::size_t start = 0;
    for (;;) {
        const auto dot = id.find('.', start);
        const auto label = id.substr(start, dot == std::string_view::npos ? id.size() - start : dot - start);
        if (label.empty() || label.size() > 63) {
            error = "application id labels must be 1..63 characters";
            return false;
        }
        if (!((label.front() >= 'a' && label.front() <= 'z') || (label.front() >= '0' && label.front() <= '9')) ||
            !((label.back() >= 'a' && label.back() <= 'z') || (label.back() >= '0' && label.back() <= '9'))) {
            error = "application id labels must start and end with a lowercase letter or digit";
            return false;
        }
        for (const char ch : label) {
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
            if (!ok) {
                error = "application id must use lowercase reverse-domain syntax";
                return false;
            }
        }
        ++labels;
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    if (labels < 2) {
        error = "application id must contain at least two labels (reverse-domain style)";
        return false;
    }
    error.clear();
    return true;
}

bool NetworkOrigin::parse(std::string_view text, NetworkOrigin& out, std::string& error) {
    out = {};
    const auto scheme_end = text.find("://");
    if (scheme_end == std::string_view::npos) {
        error = "origin must include http:// or https://";
        return false;
    }
    std::string scheme(text.substr(0, scheme_end));
    for (char& ch : scheme) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (scheme != "http" && scheme != "https") {
        error = "origin scheme must be http or https";
        return false;
    }
    std::string_view rest = text.substr(scheme_end + 3);
    if (rest.empty()) {
        error = "origin host is empty";
        return false;
    }
    if (rest.find('/') != std::string_view::npos || rest.find('?') != std::string_view::npos ||
        rest.find('#') != std::string_view::npos) {
        error = "origin must not contain a path, query or fragment";
        return false;
    }
    if (rest.find('@') != std::string_view::npos) {
        error = "origin must not contain user information";
        return false;
    }
    std::string_view host;
    std::string_view port_text;
    if (rest.front() == '[') {
        const auto close = rest.find(']');
        if (close == std::string_view::npos) {
            error = "invalid IPv6 origin";
            return false;
        }
        host = rest.substr(1, close - 1);
        if (close + 1 < rest.size()) {
            if (rest[close + 1] != ':') {
                error = "invalid origin port";
                return false;
            }
            port_text = rest.substr(close + 2);
        }
        if (!valid_ipv6(host)) {
            error = "invalid IPv6 address";
            return false;
        }
        out.ip_literal = true;
    } else {
        const auto colon = rest.rfind(':');
        if (colon != std::string_view::npos && rest.find(':') == colon) {
            host = rest.substr(0, colon);
            port_text = rest.substr(colon + 1);
        } else {
            host = rest;
        }
        std::string lowered(host);
        for (char& ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lowered.rfind("*.", 0) == 0) {
            out.wildcard = true;
            lowered.erase(0, 2);
        }
        if (valid_ipv4(lowered)) {
            if (out.wildcard) {
                error = "wildcards are only allowed for DNS names";
                return false;
            }
            out.ip_literal = true;
        } else if (!valid_dns_host(lowered)) {
            error = "invalid origin host";
            return false;
        }
        host = lowered;
    }
    std::uint16_t port = scheme == "https" ? 443 : 80;
    if (!port_text.empty() && !parse_port(port_text, port)) {
        error = "invalid origin port";
        return false;
    }
    out.scheme = std::move(scheme);
    out.host = std::string(host);
    out.port = port;
    error.clear();
    return true;
}

std::string NetworkOrigin::to_string() const {
    std::string out = scheme + "://";
    if (wildcard) out += "*.";
    if (ip_literal && host.find(':') != std::string::npos) {
        out += "[" + host + "]";
    } else {
        out += host;
    }
    const std::uint16_t default_port = scheme == "https" ? 443 : 80;
    if (port != default_port) out += ":" + std::to_string(port);
    return out;
}

std::vector<std::string> NetworkOrigin::csp_sources() const {
    std::vector<std::string> sources;
    const std::uint16_t default_port = scheme == "https" ? 443 : 80;
    auto build = [&](bool with_wildcard) {
        std::string source = scheme + "://";
        if (with_wildcard) source += "*.";
        if (ip_literal && host.find(':') != std::string::npos) {
            source += "[" + host + "]";
        } else {
            source += host;
        }
        if (port != default_port) source += ":" + std::to_string(port);
        return source;
    };
    if (wildcard) {
        // CSP's *.example.com does not match example.com, but SPEC-0007
        // requires wildcard origins to cover both root and subdomains.
        sources.push_back(build(false));
        sources.push_back(build(true));
    } else {
        sources.push_back(build(false));
    }
    return sources;
}

bool Manifest::has_permission(Permission permission) const {
    return std::find(permissions.begin(), permissions.end(), permission) != permissions.end();
}

bool Manifest::host_api_compatible() const {
    SemVer host;
    std::string error;
    if (!SemVer::parse(kHostApiVersion, host, error)) return false;
    return extensions::host_api_compatible(host, min_host_api, max_host_api);
}

std::vector<std::string> origin_strings(const std::vector<NetworkOrigin>& origins) {
    std::vector<std::string> out;
    out.reserve(origins.size());
    for (const auto& origin : origins) out.push_back(origin.to_string());
    return out;
}

std::vector<std::string> intersect_origins(const std::vector<std::string>& declared,
                                           const std::vector<std::string>& approved) {
    std::vector<std::string> out;
    for (const auto& origin : declared) {
        if (std::find(approved.begin(), approved.end(), origin) != approved.end()) out.push_back(origin);
    }
    return out;
}

bool origins_subset(const std::vector<std::string>& declared,
                    const std::vector<std::string>& approved) {
    for (const auto& origin : declared) {
        if (std::find(approved.begin(), approved.end(), origin) == approved.end()) return false;
    }
    return true;
}

std::string Manifest::csp_policy() const { return csp_policy({}); }

std::string Manifest::csp_policy(const std::vector<std::string>& origins) const {
    std::vector<std::string> external;
    for (const auto& text : origins) {
        NetworkOrigin origin;
        std::string error;
        if (!NetworkOrigin::parse(text, origin, error)) continue;
        const auto sources = origin.csp_sources();
        external.insert(external.end(), sources.begin(), sources.end());
    }
    std::string external_list;
    for (const auto& source : external) external_list += " " + source;
    const std::string style_sources = "'self' 'unsafe-inline'" + external_list;
    std::string policy = "default-src 'self'";
    policy += "; script-src 'self'" + external_list;
    policy += "; style-src " + style_sources;
    policy += "; img-src 'self' data:" + external_list;
    policy += "; font-src 'self' data:" + external_list;
    policy += "; connect-src 'self'" + external_list;
    policy += "; media-src 'self' data:" + external_list;
    policy += "; frame-src 'self'" + external_list;
    policy += "; child-src 'self'" + external_list;
    policy += "; form-action 'self'" + external_list;
    policy += "; object-src 'none'; base-uri 'none'; frame-ancestors 'self'";
    return policy;
}

bool parse_manifest(const nlohmann::json& document, Manifest& out, std::string& error,
                    std::vector<std::string>& warnings) {
    out = {};
    warnings.clear();
    if (!document.is_object()) {
        error = "manifest.json must contain a JSON object";
        return false;
    }

    const auto string_field = [&](const char* key, std::string& target, std::size_t max_bytes, bool required) {
        if (!document.contains(key)) {
            if (required) {
                error = std::string("manifest field '") + key + "' is required";
                return false;
            }
            return true;
        }
        const auto& value = document[key];
        if (!value.is_string()) {
            error = std::string("manifest field '") + key + "' must be a string";
            return false;
        }
        target = value.get<std::string>();
        if (required && target.empty()) {
            error = std::string("manifest field '") + key + "' must not be empty";
            return false;
        }
        if (target.size() > max_bytes) {
            error = std::string("manifest field '") + key + "' is too long";
            return false;
        }
        if (has_control_characters(target)) {
            error = std::string("manifest field '") + key + "' contains control characters";
            return false;
        }
        return true;
    };

    if (!string_field("id", out.id, 255, true) || !valid_app_id(out.id, error)) return false;
    if (!string_field("name", out.name, kMaxNameBytes, true)) return false;
    out.name = trim_ascii(out.name);
    if (out.name.empty()) {
        error = "manifest field 'name' must not be blank";
        return false;
    }
    if (!string_field("version", out.version, 256, true)) return false;
    if (!SemVer::parse(out.version, out.version_semver, error)) return false;

    if (!document.contains("entry") || !document["entry"].is_string() ||
        document["entry"].get<std::string>() != "index.html") {
        error = "manifest entry must be 'index.html' in the first runtime";
        return false;
    }
    out.entry = "index.html";

    if (!string_field("description", out.description, kMaxDescriptionBytes, false)) return false;
    if (!string_field("publisher", out.publisher, kMaxPublisherBytes, false)) return false;
    if (!string_field("icon", out.icon, kMaxIconBytes, false)) return false;
    if (!out.icon.empty() && !safe_relative_asset_path(out.icon)) {
        error = "manifest icon must be a normalized relative path";
        return false;
    }

    std::string min_text;
    std::string max_text;
    if (!string_field("minHostApiVersion", min_text, 256, true)) return false;
    if (!string_field("maxHostApiVersion", max_text, 256, true)) return false;
    if (!SemVer::parse(min_text, out.min_host_api, error)) {
        error = "minHostApiVersion: " + error;
        return false;
    }
    if (!SemVer::parse(max_text, out.max_host_api, error)) {
        error = "maxHostApiVersion: " + error;
        return false;
    }
    if (out.max_host_api < out.min_host_api) {
        error = "maxHostApiVersion must not be lower than minHostApiVersion";
        return false;
    }

    std::set<Permission> requested;
    if (document.contains("permissions")) {
        const auto& permissions = document["permissions"];
        if (!permissions.is_array()) {
            error = "manifest field 'permissions' must be an array";
            return false;
        }
        for (const auto& item : permissions) {
            Permission permission;
            if (!item.is_string()) {
                error = "permission entries must be strings";
                return false;
            }
            const auto name = item.get<std::string>();
            if (!parse_permission(name, permission)) {
                error = "unknown permission module: " + name;
                return false;
            }
            requested.insert(permission);
        }
    }

    if (document.contains("network")) {
        const auto& network = document["network"];
        if (!network.is_object()) {
            error = "manifest field 'network' must be an object";
            return false;
        }
        if (network.contains("origins")) {
            const auto& origins = network["origins"];
            if (!origins.is_array()) {
                error = "network.origins must be an array";
                return false;
            }
            std::set<std::string> seen;
            for (const auto& item : origins) {
                if (!item.is_string()) {
                    error = "network.origins entries must be strings";
                    return false;
                }
                NetworkOrigin origin;
                std::string origin_error;
                if (!NetworkOrigin::parse(item.get<std::string>(), origin, origin_error)) {
                    error = "invalid network origin: " + origin_error;
                    return false;
                }
                const auto canonical = origin.to_string();
                if (seen.insert(canonical).second) out.origins.push_back(std::move(origin));
            }
        }
        if (!out.origins.empty()) requested.insert(Permission::network);
    }

    out.permissions.assign(requested.begin(), requested.end());

    out.window = {};
    if (document.contains("window")) {
        const auto& window = document["window"];
        if (!window.is_object()) {
            error = "manifest field 'window' must be an object";
            return false;
        }
        if (!read_dimension(window, "defaultWidth", 960, out.window.default_width, error) ||
            !read_dimension(window, "defaultHeight", 640, out.window.default_height, error) ||
            !read_dimension(window, "minWidth", 320, out.window.min_width, error) ||
            !read_dimension(window, "minHeight", 240, out.window.min_height, error)) {
            return false;
        }
        if (out.window.min_width > out.window.default_width ||
            out.window.min_height > out.window.default_height) {
            error = "window minimum size exceeds the default size";
            return false;
        }
        if (window.contains("instances")) {
            if (!window["instances"].is_string()) {
                error = "window.instances must be 'single' or 'multi'";
                return false;
            }
            const auto instances = window["instances"].get<std::string>();
            if (instances != "single" && instances != "multi") {
                error = "window.instances must be 'single' or 'multi'";
                return false;
            }
            out.window.single_instance = instances == "single";
        }
    }

    // Execution-oriented reserved fields are accepted syntactically but never
    // executed. Their presence is surfaced to the administrator.
    if (document.contains("backend")) {
        warnings.push_back("backend runtime fields are reserved and are not executed in this release");
    }
    if (document.contains("scripts") || document.contains("runtime")) {
        warnings.push_back("script/runtime fields are reserved and are not executed in this release");
    }

    out.raw = document;
    error.clear();
    return true;
}

} // namespace edgedesk::extensions
