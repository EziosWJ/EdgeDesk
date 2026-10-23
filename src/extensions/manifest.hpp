#pragma once

#include "extensions/backend_spec.hpp"
#include "extensions/semver.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

// Core manifest parser plus the module fields required to enable an app.
// The core parser only validates declared fields; no backend/script runtime
// field is ever interpreted as executable capability.

namespace edgedesk::extensions {

constexpr const char* kHostApiVersion = "1.0.0";

enum class Permission {
    storage,
    files_manage,
    terminal_access,
    network,
};

const char* permission_name(Permission permission);
bool parse_permission(std::string_view name, Permission& out);
bool permission_from_json(const nlohmann::json& value, Permission& out);

struct NetworkOrigin {
    std::string scheme;  // http | https
    std::string host;    // lowercase DNS name, IPv4 or bracket-free IPv6
    std::uint16_t port = 0;
    bool wildcard = false; // *.example.com; matches root and all subdomains
    bool ip_literal = false;

    static bool parse(std::string_view text, NetworkOrigin& out, std::string& error);
    std::string to_string() const;
    // CSP source expressions needed to cover the declared origin. For a
    // wildcard origin this includes both the root domain and *.domain.
    std::vector<std::string> csp_sources() const;
};

struct WindowSpec {
    std::int32_t default_width = 960;
    std::int32_t default_height = 640;
    std::int32_t min_width = 320;
    std::int32_t min_height = 240;
    bool single_instance = true;
};

struct Manifest {
    std::string id;
    std::string name;
    std::string version;
    SemVer version_semver;
    std::string entry = "index.html";
    std::string description;
    std::string publisher;
    std::string icon;
    SemVer min_host_api;
    SemVer max_host_api;
    std::vector<Permission> permissions;       // sorted, unique, includes network when origins exist
    std::vector<NetworkOrigin> origins;
    WindowSpec window;
    // Optional native backend (SPEC-0011). `present == false` keeps the
    // package a pure Web application with the SPEC-0007 behavior.
    BackendSpec backend;
    nlohmann::json raw;

    bool has_permission(Permission permission) const;
    bool has_backend() const { return backend.present; }
    bool host_api_compatible() const;
    // Emits a same-origin policy with no external sources. Callers that serve
    // an application with an effective network grant must use the overload
    // below; this overload is the secure default.
    std::string csp_policy() const;
    // `origins` are canonical NetworkOrigin strings that are both declared by
    // the manifest and covered by the administrator's approval. Only these
    // become script/style/img/font/connect/media/frame/child/form sources.
    std::string csp_policy(const std::vector<std::string>& origins) const;
};

// Canonical `scheme://host[:port]` strings in manifest order.
std::vector<std::string> origin_strings(const std::vector<NetworkOrigin>& origins);
// Declared origins that are also present in `approved`, preserving declared
// order. Used for CSP and runtime session snapshots.
std::vector<std::string> intersect_origins(const std::vector<std::string>& declared,
                                           const std::vector<std::string>& approved);
// True when every declared origin is present in `approved`.
bool origins_subset(const std::vector<std::string>& declared,
                    const std::vector<std::string>& approved);

// Validates a complete manifest. Unknown top-level keys are preserved for
// forward compatibility; execution-oriented backend/script fields are ignored
// (never executable) and reported through `warnings`.
bool parse_manifest(const nlohmann::json& document, Manifest& out, std::string& error,
                    std::vector<std::string>& warnings);

// Validates a stable reverse-domain application ID.
bool valid_app_id(std::string_view id, std::string& error);

} // namespace edgedesk::extensions
