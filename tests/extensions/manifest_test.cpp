#include "test_util.hpp"

#include "extensions/manifest.hpp"

#include <nlohmann/json.hpp>

using namespace edgedesk::extensions;

namespace {

nlohmann::json base_manifest() {
    nlohmann::json manifest = {
        {"id", "io.example.tools"},
        {"name", "Example Tools"},
        {"version", "1.2.3"},
        {"entry", "index.html"},
        {"minHostApiVersion", "1.0.0"},
        {"maxHostApiVersion", "2.0.0"},
        {"permissions", nlohmann::json::array({"storage", "files.manage"})},
        {"window", {{"defaultWidth", 800}, {"defaultHeight", 600}, {"minWidth", 320}, {"minHeight", 240},
                     {"instances", "multi"}}},
    };
    return manifest;
}

bool accepts(const nlohmann::json& document, Manifest& manifest, std::string& error) {
    std::vector<std::string> warnings;
    return parse_manifest(document, manifest, error, warnings);
}

void test_valid() {
    Manifest manifest;
    std::string error;
    const bool ok = accepts(base_manifest(), manifest, error);
    CHECK(ok);
    if (!ok) std::fprintf(stderr, "manifest error: %s\n", error.c_str());
    CHECK_STR_EQ(manifest.id, "io.example.tools");
    CHECK_STR_EQ(manifest.version_semver.to_string(), "1.2.3");
    CHECK(manifest.has_permission(Permission::storage));
    CHECK(manifest.has_permission(Permission::files_manage));
    CHECK(!manifest.has_permission(Permission::terminal_access));
    CHECK(!manifest.window.single_instance);
    CHECK(manifest.host_api_compatible());
}

void test_network_and_csp() {
    auto document = base_manifest();
    document["permissions"].push_back("network");
    document["network"] = {{"origins", nlohmann::json::array(
                                          {"https://*.example.com", "http://192.168.1.10:8080",
                                           "https://[::1]:8443"})}};
    Manifest manifest;
    std::string error;
    CHECK(accepts(document, manifest, error));
    CHECK_EQ(manifest.origins.size(), std::size_t{3});
    CHECK(manifest.has_permission(Permission::network));
    const auto wildcard = manifest.origins[0].csp_sources();
    CHECK_EQ(wildcard.size(), std::size_t{2});
    CHECK_STR_EQ(wildcard[0], "https://example.com");
    CHECK_STR_EQ(wildcard[1], "https://*.example.com");
    CHECK_STR_EQ(manifest.origins[1].to_string(), "http://192.168.1.10:8080");
    CHECK_STR_EQ(manifest.origins[2].to_string(), "https://[::1]:8443");
    const std::string policy = manifest.csp_policy();
    CHECK(policy.find("https://example.com") != std::string::npos);
    CHECK(policy.find("https://*.example.com") != std::string::npos);
    CHECK(policy.find("object-src 'none'") != std::string::npos);
}

void test_origins_imply_network_request() {
    auto document = base_manifest();
    document["network"] = {{"origins", nlohmann::json::array({"https://example.com"})}};
    Manifest manifest;
    std::string error;
    CHECK(accepts(document, manifest, error));
    CHECK(manifest.has_permission(Permission::network));
}

void test_invalid_manifests() {
    struct Case {
        const char* label;
        void (*mutate)(nlohmann::json&);
    };
    const Case cases[] = {
        {"bad id", [](nlohmann::json& d) { d["id"] = "Example"; }},
        {"short id", [](nlohmann::json& d) { d["id"] = "example"; }},
        {"blank name", [](nlohmann::json& d) { d["name"] = "   "; }},
        {"bad version", [](nlohmann::json& d) { d["version"] = "1.0"; }},
        {"entry", [](nlohmann::json& d) { d["entry"] = "app.html"; }},
        {"missing min api", [](nlohmann::json& d) { d.erase("minHostApiVersion"); }},
        {"bad max api", [](nlohmann::json& d) { d["maxHostApiVersion"] = "x"; }},
        {"reversed range", [](nlohmann::json& d) {
             d["minHostApiVersion"] = "2.0.0";
             d["maxHostApiVersion"] = "1.0.0";
         }},
        {"unknown permission", [](nlohmann::json& d) { d["permissions"].push_back("root.everything"); }},
        {"permissions type", [](nlohmann::json& d) { d["permissions"] = "storage"; }},
        {"origin path", [](nlohmann::json& d) {
             d["network"] = {{"origins", nlohmann::json::array({"https://example.com/path"})}};
         }},
        {"origin scheme", [](nlohmann::json& d) {
             d["network"] = {{"origins", nlohmann::json::array({"ftp://example.com"})}};
         }},
        {"origin userinfo", [](nlohmann::json& d) {
             d["network"] = {{"origins", nlohmann::json::array({"https://user@example.com"})}};
         }},
        {"origin port", [](nlohmann::json& d) {
             d["network"] = {{"origins", nlohmann::json::array({"https://example.com:70000"})}};
         }},
        {"window min", [](nlohmann::json& d) {
             d["window"]["minWidth"] = 900;
             d["window"]["defaultWidth"] = 800;
         }},
        {"window instances", [](nlohmann::json& d) { d["window"]["instances"] = "many"; }},
        {"icon traversal", [](nlohmann::json& d) { d["icon"] = "../icon.png"; }},
    };
    for (const auto& item : cases) {
        auto document = base_manifest();
        item.mutate(document);
        Manifest manifest;
        std::string error;
        const bool ok = accepts(document, manifest, error);
        if (ok) std::fprintf(stderr, "case unexpectedly accepted: %s\n", item.label);
        CHECK(!ok);
        CHECK(!error.empty());
    }
}

void test_host_incompatible() {
    auto document = base_manifest();
    document["minHostApiVersion"] = "2.0.0";
    document["maxHostApiVersion"] = "3.0.0";
    Manifest manifest;
    std::string error;
    CHECK(accepts(document, manifest, error));
    CHECK(!manifest.host_api_compatible());
}

void test_reserved_runtime_warning() {
    auto document = base_manifest();
    document["runtime"] = {{"backend", true}};
    Manifest manifest;
    std::string error;
    std::vector<std::string> warnings;
    CHECK(parse_manifest(document, manifest, error, warnings));
    CHECK_EQ(warnings.size(), std::size_t{1});
}

void test_origin_matching_rules() {
    NetworkOrigin origin;
    std::string error;
    CHECK(NetworkOrigin::parse("https://*.example.com", origin, error));
    CHECK(origin.wildcard);
    CHECK_STR_EQ(origin.host, "example.com");
    CHECK_EQ(origin.port, 443);
    CHECK(!NetworkOrigin::parse("https://*", origin, error));
    CHECK(!NetworkOrigin::parse("https://example.com:abc", origin, error));
    CHECK(!NetworkOrigin::parse("https://exa mple.com", origin, error));
    CHECK(!NetworkOrigin::parse("https://example.com/", origin, error));
}

} // namespace

int main() {
    test_valid();
    test_network_and_csp();
    test_origins_imply_network_request();
    test_invalid_manifests();
    test_host_incompatible();
    test_reserved_runtime_warning();
    test_origin_matching_rules();
    return edgedesk::test::finish("manifest_test");
}
