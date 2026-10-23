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
    const std::string policy = manifest.csp_policy(origin_strings(manifest.origins));
    CHECK(policy.find("https://example.com") != std::string::npos);
    CHECK(policy.find("https://*.example.com") != std::string::npos);
    CHECK(policy.find("object-src 'none'") != std::string::npos);
}

void test_csp_requires_approved_origins() {
    auto document = base_manifest();
    document["permissions"].push_back("network");
    document["network"] = {{"origins", nlohmann::json::array(
                                          {"https://*.example.com", "http://192.168.1.10:8080"})}};
    Manifest manifest;
    std::string error;
    CHECK(accepts(document, manifest, error));

    const auto declared = origin_strings(manifest.origins);
    CHECK_EQ(declared.size(), std::size_t{2});
    CHECK_STR_EQ(declared[0], "https://*.example.com");
    CHECK_STR_EQ(declared[1], "http://192.168.1.10:8080");

    // No network grant/approval: no external source appears anywhere.
    const std::string denied = manifest.csp_policy();
    CHECK(denied.find("https://example.com") == std::string::npos);
    CHECK(denied.find("http://192.168.1.10:8080") == std::string::npos);
    CHECK(denied.find("default-src 'self'") != std::string::npos);

    // Revoked: the same empty policy is used even if origins are declared.
    const std::string revoked = manifest.csp_policy({});
    CHECK(revoked.find("https://example.com") == std::string::npos);
    CHECK(revoked.find("http://192.168.1.10:8080") == std::string::npos);

    // Only approved origins may appear.
    const std::vector<std::string> approved{"https://*.example.com"};
    std::vector<std::string> effective = intersect_origins(declared, approved);
    CHECK_EQ(effective.size(), std::size_t{1});
    CHECK_STR_EQ(effective[0], "https://*.example.com");
    CHECK(origins_subset(effective, approved));
    CHECK(!origins_subset(declared, approved));
    const std::string restricted = manifest.csp_policy(effective);
    CHECK(restricted.find("https://example.com") != std::string::npos);
    CHECK(restricted.find("https://*.example.com") != std::string::npos);
    CHECK(restricted.find("http://192.168.1.10:8080") == std::string::npos);

    // Both declared origins are allowed only when both are approved.
    const std::vector<std::string> all_approved{declared[0], declared[1]};
    CHECK(origins_subset(declared, all_approved));
    const std::string expanded = manifest.csp_policy(intersect_origins(declared, all_approved));
    CHECK(expanded.find("http://192.168.1.10:8080") != std::string::npos);
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

void test_backend_manifest() {
    auto document = base_manifest();
    document["backend"] = {{"executable", "bin/backend"}, {"args", {"--serve", "", "0"}}};
    Manifest manifest;
    std::string error;
    std::vector<std::string> warnings;
    CHECK(parse_manifest(document, manifest, error, warnings));
    if (!error.empty()) std::fprintf(stderr, "backend manifest error: %s\n", error.c_str());
    CHECK(manifest.backend.present);
    CHECK(manifest.has_backend());
    CHECK_STR_EQ(manifest.backend.executable, "bin/backend");
    CHECK_EQ(manifest.backend.args.size(), std::size_t{3});
    CHECK_STR_EQ(manifest.backend.args[0], "--serve");
    CHECK_STR_EQ(manifest.backend.args[1], "");
    // A supported backend is no longer reported as a reserved runtime field.
    CHECK(warnings.empty());

    // scripts/runtime keep their existing warning.
    auto reserved = document;
    reserved["scripts"] = {{"postinstall", "ignored"}};
    Manifest reserved_manifest;
    std::string reserved_error;
    std::vector<std::string> reserved_warnings;
    CHECK(parse_manifest(reserved, reserved_manifest, reserved_error, reserved_warnings));
    CHECK_EQ(reserved_warnings.size(), std::size_t{1});

    // Optional args default to an empty argv array.
    auto minimal = base_manifest();
    minimal["backend"] = {{"executable", "backend"}};
    Manifest minimal_manifest;
    std::string minimal_error;
    std::vector<std::string> minimal_warnings;
    CHECK(parse_manifest(minimal, minimal_manifest, minimal_error, minimal_warnings));
    CHECK(minimal_manifest.backend.present);
    CHECK(minimal_manifest.backend.args.empty());
    CHECK(minimal_warnings.empty());

    // Web-only packages keep the previous behavior.
    Manifest web_only;
    std::string web_error;
    std::vector<std::string> web_warnings;
    CHECK(parse_manifest(base_manifest(), web_only, web_error, web_warnings));
    CHECK(!web_only.backend.present);
    CHECK(!web_only.has_backend());
    CHECK(web_warnings.empty());
}

void test_invalid_backend_manifests() {
    struct Case {
        const char* label;
        void (*mutate)(nlohmann::json&);
    };
    const Case cases[] = {
        {"backend string", [](nlohmann::json& d) { d["backend"] = "backend"; }},
        {"backend null", [](nlohmann::json& d) { d["backend"] = nullptr; }},
        {"backend array", [](nlohmann::json& d) {
             d["backend"] = nlohmann::json::array(
                 {{{"executable", "one"}}, {{"executable", "two"}}});
         }},
        {"missing executable", [](nlohmann::json& d) { d["backend"] = {{"args", nlohmann::json::array()}}; }},
        {"executable type", [](nlohmann::json& d) { d["backend"] = {{"executable", 1}}; }},
        {"empty executable", [](nlohmann::json& d) { d["backend"] = {{"executable", ""}}; }},
        {"absolute executable", [](nlohmann::json& d) { d["backend"] = {{"executable", "/bin/backend"}}; }},
        {"traversal executable", [](nlohmann::json& d) { d["backend"] = {{"executable", "../backend"}}; }},
        {"inner traversal", [](nlohmann::json& d) { d["backend"] = {{"executable", "bin/../backend"}}; }},
        {"dot executable", [](nlohmann::json& d) { d["backend"] = {{"executable", "."}}; }},
        {"dot relative", [](nlohmann::json& d) { d["backend"] = {{"executable", "./backend"}}; }},
        {"empty component", [](nlohmann::json& d) { d["backend"] = {{"executable", "bin//backend"}}; }},
        {"trailing slash", [](nlohmann::json& d) { d["backend"] = {{"executable", "bin/backend/"}}; }},
        {"backslash", [](nlohmann::json& d) { d["backend"] = {{"executable", "bin\\backend"}}; }},
        {"control executable", [](nlohmann::json& d) {
             d["backend"] = {{"executable", std::string("bin/") + '\x01' + "backend"}};
         }},
        {"long executable", [](nlohmann::json& d) {
             d["backend"] = {{"executable", std::string(4097, 'a')}};
         }},
        {"args type", [](nlohmann::json& d) { d["backend"] = {{"executable", "backend"}, {"args", "--serve"}}; }},
        {"args entry type", [](nlohmann::json& d) {
             d["backend"] = {{"executable", "backend"}, {"args", nlohmann::json::array({1})}};
         }},
        {"too many args", [](nlohmann::json& d) {
             nlohmann::json args = nlohmann::json::array();
             for (int index = 0; index < 129; ++index) args.push_back("");
             d["backend"] = {{"executable", "backend"}, {"args", args}};
         }},
        {"long arg", [](nlohmann::json& d) {
             d["backend"] = {{"executable", "backend"},
                             {"args", nlohmann::json::array({std::string(8193, 'a')})}};
         }},
        {"total arg size", [](nlohmann::json& d) {
             nlohmann::json args = nlohmann::json::array();
             for (int index = 0; index < 17; ++index) args.push_back(std::string(8192, 'a'));
             d["backend"] = {{"executable", "backend"}, {"args", args}};
         }},
        {"control arg", [](nlohmann::json& d) {
             d["backend"] = {{"executable", "backend"},
                             {"args", nlohmann::json::array({std::string("a\nb")})}};
         }},
    };
    for (const auto& item : cases) {
        auto document = base_manifest();
        item.mutate(document);
        Manifest manifest;
        std::string error;
        const bool ok = accepts(document, manifest, error);
        if (ok) std::fprintf(stderr, "backend case unexpectedly accepted: %s\n", item.label);
        CHECK(!ok);
        CHECK(!error.empty());
    }

    // Identity/root/socket/host-path keys are rejected instead of silently
    // ignored, so a package can never suggest host privileges.
    const char* forbidden[] = {"run_user", "run_group", "root", "autostart", "socket", "data_dir", "url",
                               "port"};
    for (const char* key : forbidden) {
        auto document = base_manifest();
        document["backend"] = {{"executable", "backend"}, {key, "value"}};
        Manifest manifest;
        std::string error;
        const bool ok = accepts(document, manifest, error);
        if (ok) std::fprintf(stderr, "forbidden backend key accepted: %s\n", key);
        CHECK(!ok);
        CHECK(error.find(key) != std::string::npos);
    }
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
    test_csp_requires_approved_origins();
    test_origins_imply_network_request();
    test_invalid_manifests();
    test_host_incompatible();
    test_backend_manifest();
    test_invalid_backend_manifests();
    test_reserved_runtime_warning();
    test_origin_matching_rules();
    return edgedesk::test::finish("manifest_test");
}
