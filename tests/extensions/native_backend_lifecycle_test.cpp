#include "test_util.hpp"

#include "core/util.hpp"
#include "core/utf8.hpp"
#include "extensions/elf_info.hpp"
#include "extensions/extension_store.hpp"
#include "extensions/native_backend_installer.hpp"
#include "extensions/native_backend_lifecycle.hpp"
#include "extensions/package_manager.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using namespace edgedesk;
using namespace edgedesk::extensions;

namespace {

namespace fs = std::filesystem;

std::string current_user_name() {
    struct passwd* entry = ::getpwuid(::geteuid());
    if (entry != nullptr && entry->pw_name != nullptr) return entry->pw_name;
    return std::to_string(::geteuid());
}

std::string current_group_name() {
    struct group* entry = ::getgrgid(::getegid());
    if (entry != nullptr && entry->gr_name != nullptr) return entry->gr_name;
    return std::to_string(::getegid());
}

std::string host_arch() { return elf_arch_name(host_elf_arch()); }

std::string wrong_arch() { return host_arch() == "x86_64" ? "aarch64" : "x86_64"; }

struct TempRoot {
    fs::path path;
    bool created = false;

    TempRoot() {
        std::string pattern = (fs::temp_directory_path() / "edgedesk-lifecycle-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (::mkdtemp(buffer.data()) != nullptr) {
            path = buffer.data();
            created = true;
        }
    }
    ~TempRoot() {
        if (!created) return;
        std::string error;
        remove_tree_at(path.string(), error);
    }
};

struct Fixture {
    TempRoot root;
    std::unique_ptr<PackageManager> packages;
    std::unique_ptr<NativeBackendInstaller> installer;
    std::unique_ptr<NativeBackendLifecycle> lifecycle;
    bool ready = false;

    Fixture() {
        if (!root.created) return;
        Error error;
        packages = PackageManager::create(root.path.string(), error);
        if (!packages) {
            std::fprintf(stderr, "PackageManager::create failed: %s\n", error.message.c_str());
            return;
        }
        installer =
            std::make_unique<NativeBackendInstaller>(packages->store(), root.path.string());
        lifecycle = std::make_unique<NativeBackendLifecycle>(packages->store(), *packages,
                                                            *installer, root.path.string());
        ready = true;
    }

    ExtensionStore& store() { return packages->store(); }
    const std::string root_path() const { return root.path.string(); }
};

nlohmann::json manifest_json(const std::string& app_id, const std::string& name,
                             bool with_backend) {
    nlohmann::json manifest = {
        {"id", app_id},
        {"name", name},
        {"version", "1.1.0"},
        {"entry", "index.html"},
        {"minHostApiVersion", "1.0.0"},
        {"maxHostApiVersion", "2.0.0"},
    };
    if (with_backend) {
        manifest["backend"] = {{"executable", "backend"}, {"args", {"--serve", "--port=0"}}};
    }
    return manifest;
}

void create_app(Fixture& fixture, const std::string& app_id, const std::string& name,
                bool with_backend_manifest, bool with_backend_record) {
    AppRecord app;
    app.app_id = app_id;
    app.display_name = name;
    app.status = app_status::enabled;
    app.active_revision_id = "rev-1";
    app.active_version = "1.1.0";
    app.created_at = now_unix_seconds();
    CHECK(fixture.store().upsert_app(app).ok());

    RevisionRecord revision;
    revision.revision_id = "rev-1";
    revision.app_id = app_id;
    revision.version = "1.1.0";
    revision.manifest_json = manifest_json(app_id, name, with_backend_manifest).dump();
    revision.created_at = now_unix_seconds();
    revision.status = revision_status::active;
    CHECK(fixture.store().insert_revision(revision).ok());

    if (!with_backend_record) return;
    BackendRecord record;
    record.app_id = app_id;
    record.service_id = backend_service_id(app_id);
    record.executable_rel = "backend";
    record.args = {"--serve", "--port=0"};
    record.arch = host_arch();
    record.run_user = current_user_name();
    record.run_group = current_group_name();
    record.state = backend_state::stopped;
    record.created_at = now_unix_seconds();
    CHECK(fixture.store().upsert_backend(record).ok());
}

void write_executable(Fixture& fixture, const std::string& app_id) {
    const fs::path dir = backend_program_dir(fixture.root_path(), app_id);
    fs::create_directories(dir);
    const fs::path executable = backend_executable_path(fixture.root_path(), app_id);
    {
        std::ofstream out(executable, std::ios::binary | std::ios::trunc);
        const char payload[] = "not a real ELF; lifecycle only checks runnability";
        out.write(payload, static_cast<std::streamsize>(sizeof(payload) - 1));
    }
    CHECK(::chmod(executable.c_str(), 0755) == 0);
}

std::string error_slug(const Error& error) {
    return backend_error_slug(error, error_code_name(error.code));
}

void test_describe_web_only() {
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    create_app(fixture, "io.example.webonly", "Web Only", false, false);

    nlohmann::json backend;
    const Error error = fixture.lifecycle->describe("io.example.webonly", backend);
    CHECK(error.ok());
    CHECK_EQ(backend.value("present", true), false);
    CHECK(!backend.contains("executable"));
    CHECK(!backend.value("reason", std::string()).empty());
}

void test_describe_backend_manifest_without_record() {
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    create_app(fixture, "io.example.missing", "Missing Record", true, false);

    nlohmann::json backend;
    CHECK(fixture.lifecycle->describe("io.example.missing", backend).ok());
    CHECK_EQ(backend.value("present", true), false);
    CHECK(backend.value("reason", std::string()).find("declares a native backend") !=
          std::string::npos);
}

void test_describe_backend() {
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    create_app(fixture, "io.example.native", "Native App", true, true);
    write_executable(fixture, "io.example.native");

    nlohmann::json backend;
    const Error error = fixture.lifecycle->describe("io.example.native", backend);
    CHECK(error.ok());
    CHECK_EQ(backend.value("present", false), true);
    CHECK_STR_EQ(backend.value("service_id", std::string()),
                 backend_service_id("io.example.native"));
    CHECK_STR_EQ(backend.value("arch", std::string()), host_arch());
    CHECK_STR_EQ(backend.value("host_arch", std::string()), host_arch());
    CHECK_STR_EQ(backend.value("executable", std::string()),
                 backend_executable_path(fixture.root_path(), "io.example.native"));
    CHECK_STR_EQ(backend.value("program_dir", std::string()),
                 backend_program_dir(fixture.root_path(), "io.example.native"));
    CHECK_STR_EQ(backend.value("data_dir", std::string()),
                 backend_data_dir(fixture.root_path(), "io.example.native"));
    CHECK_STR_EQ(backend.value("socket_path", std::string()),
                 backend_socket_path(fixture.root_path(), "io.example.native"));
    CHECK_STR_EQ(backend.value("run_user", std::string()), current_user_name());
    CHECK_STR_EQ(backend.value("run_group", std::string()), current_group_name());
    CHECK_EQ(backend.value("allow_root", true), false);
    CHECK_EQ(backend.value("autostart", true), false);
    CHECK_STR_EQ(backend.value("state", std::string()), backend_state::stopped);
    CHECK_STR_EQ(backend.value("error", std::string()), "");
}

void test_service_params() {
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    create_app(fixture, "io.example.params", "Params App", true, true);
    write_executable(fixture, "io.example.params");

    nlohmann::json params;
    const Error error = fixture.lifecycle->service_params("io.example.params", params);
    CHECK(error.ok());
    if (!error.ok()) std::fprintf(stderr, "service_params: %s\n", error.message.c_str());
    CHECK_STR_EQ(params.value("id", std::string()), backend_service_id("io.example.params"));
    const std::string expected_name =
        "Params App (" + backend_service_id("io.example.params").substr(0, 16) + ") backend";
    CHECK_STR_EQ(params.value("name", std::string()), expected_name);
    CHECK_STR_EQ(params.value("executable", std::string()),
                 backend_executable_path(fixture.root_path(), "io.example.params"));
    CHECK_STR_EQ(params.value("cwd", std::string()),
                 backend_program_dir(fixture.root_path(), "io.example.params"));
    CHECK_STR_EQ(params.value("run_user", std::string()), current_user_name());
    CHECK_STR_EQ(params.value("run_group", std::string()), current_group_name());
    CHECK_EQ(params.value("allow_root", true), false);
    CHECK_EQ(params.value("autostart", true), false);
    CHECK_STR_EQ(params.value("restart_policy", std::string()), "on-failure");

    CHECK(params.contains("args"));
    CHECK(params["args"].is_array());
    CHECK_EQ(params["args"].size(), std::size_t{2});
    CHECK_STR_EQ(params["args"][0].get<std::string>(), "--serve");
    CHECK_STR_EQ(params["args"][1].get<std::string>(), "--port=0");
    // There is never a shell string or a client-supplied environment.
    CHECK(!params.contains("shell"));
    CHECK(!params.contains("command"));
    CHECK_EQ(params["env"].value(kBackendSocketEnv, std::string()),
             backend_socket_path(fixture.root_path(), "io.example.params"));
    CHECK_EQ(params["env"].value(kBackendDataEnv, std::string()),
             backend_data_dir(fixture.root_path(), "io.example.params"));
}

void test_service_name_is_sanitized() {
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    const std::string app_id = "io.example.longname";
    // 200 bytes plus a control character and a multi-byte character.
    std::string display(200, 'A');
    display += "\x01";
    display += "后端";
    create_app(fixture, app_id, display, true, true);
    write_executable(fixture, app_id);

    nlohmann::json params;
    const Error error = fixture.lifecycle->service_params(app_id, params);
    CHECK(error.ok());
    const std::string name = params.value("name", std::string());
    CHECK(name.size() <= 128);
    CHECK(edgedesk::is_valid_utf8(name));
    CHECK(name.find('\x01') == std::string::npos);
    CHECK(name.rfind(" backend") == name.size() - std::strlen(" backend"));
}

void test_identity_required() {
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    create_app(fixture, "io.example.noid", "No Identity", true, true);
    write_executable(fixture, "io.example.noid");

    BackendRecord record;
    bool found = false;
    CHECK(fixture.store().get_backend("io.example.noid", record, found).ok());
    CHECK(found);
    record.run_user.clear();
    record.run_group.clear();
    CHECK(fixture.store().upsert_backend(record).ok());

    nlohmann::json params;
    const Error error = fixture.lifecycle->service_params("io.example.noid", params);
    CHECK(!error.ok());
    CHECK_STR_EQ(error_slug(error), "backend_identity_required");

    std::string socket_path;
    const Error prepared = fixture.lifecycle->prepare_runtime("io.example.noid", socket_path);
    CHECK(!prepared.ok());
    CHECK_STR_EQ(error_slug(prepared), "backend_identity_required");
}

void test_arch_mismatch_and_missing_executable() {
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    create_app(fixture, "io.example.arch", "Arch App", true, true);
    write_executable(fixture, "io.example.arch");

    BackendRecord record;
    bool found = false;
    CHECK(fixture.store().get_backend("io.example.arch", record, found).ok());
    CHECK(found);
    const std::string original_arch = record.arch;
    record.arch = wrong_arch();
    CHECK(fixture.store().upsert_backend(record).ok());

    nlohmann::json params;
    Error error = fixture.lifecycle->service_params("io.example.arch", params);
    CHECK(!error.ok());
    CHECK_STR_EQ(error_slug(error), "backend_arch_mismatch");

    record.arch = original_arch;
    CHECK(fixture.store().upsert_backend(record).ok());
    CHECK(::unlink(backend_executable_path(fixture.root_path(), "io.example.arch").c_str()) == 0);
    error = fixture.lifecycle->service_params("io.example.arch", params);
    CHECK(!error.ok());
    CHECK_STR_EQ(error_slug(error), "backend_unavailable");
}

void test_root_requires_allow_root() {
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    create_app(fixture, "io.example.root", "Root App", true, true);
    write_executable(fixture, "io.example.root");

    Error error = fixture.lifecycle->set_identity("io.example.root", "0", "0", false);
    CHECK(!error.ok());
    CHECK_STR_EQ(error_slug(error), "backend_root_not_allowed");

    BackendRecord record;
    bool found = false;
    CHECK(fixture.store().get_backend("io.example.root", record, found).ok());
    CHECK(found);
    CHECK_STR_EQ(record.run_user, current_user_name());

    // Root is allowed only with the explicit flag. This does not assert that
    // the host can switch uid; it only records administrator intent.
    error = fixture.lifecycle->set_identity("io.example.root", "0", "0", true);
    CHECK(error.ok());
    CHECK(fixture.store().get_backend("io.example.root", record, found).ok());
    CHECK(found);
    CHECK_EQ(record.allow_root, true);
    CHECK_STR_EQ(record.run_user, "0");
}

void test_autostart_is_independent_and_defaults_off() {
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    create_app(fixture, "io.example.auto", "Autostart App", true, true);
    write_executable(fixture, "io.example.auto");

    BackendRecord record;
    bool found = false;
    CHECK(fixture.store().get_backend("io.example.auto", record, found).ok());
    CHECK(found);
    CHECK_EQ(record.autostart, false);

    CHECK(fixture.lifecycle->set_autostart("io.example.auto", true).ok());
    CHECK(fixture.store().get_backend("io.example.auto", record, found).ok());
    CHECK(found);
    CHECK_EQ(record.autostart, true);
    CHECK_STR_EQ(record.state, backend_state::stopped);
}

void test_prepare_runtime_and_stale_socket() {
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    const std::string app_id = "io.example.runtime";
    create_app(fixture, app_id, "Runtime App", true, true);
    write_executable(fixture, app_id);

    std::string socket_path;
    Error error = fixture.lifecycle->prepare_runtime(app_id, socket_path);
    CHECK(error.ok());
    if (!error.ok()) std::fprintf(stderr, "prepare_runtime: %s\n", error.message.c_str());
    CHECK_STR_EQ(socket_path, backend_socket_path(fixture.root_path(), app_id));
    CHECK(socket_path.size() < 108);

    struct stat info {};
    CHECK(::stat(backend_run_dir(fixture.root_path(), app_id).c_str(), &info) == 0);
    CHECK(S_ISDIR(info.st_mode));
    CHECK_EQ(static_cast<unsigned>(info.st_mode & 0777), 0770u);

    // A stale socket inside the run directory is removed before a new start.
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    if (fd >= 0) {
        struct sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
        CHECK(::bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(::close(fd) == 0);
        CHECK(::lstat(socket_path.c_str(), &info) == 0);
        CHECK(S_ISSOCK(info.st_mode));

        error = fixture.lifecycle->prepare_runtime(app_id, socket_path);
        CHECK(error.ok());
        CHECK(::lstat(socket_path.c_str(), &info) != 0 && errno == ENOENT);
    }

    // clean_runtime removes only the socket, never the directory tree.
    const int second = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(second >= 0);
    if (second >= 0) {
        struct sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
        CHECK(::bind(second, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(::close(second) == 0);
        CHECK(fixture.lifecycle->clean_runtime(app_id).ok());
        CHECK(::lstat(socket_path.c_str(), &info) != 0 && errno == ENOENT);
        CHECK(::stat(backend_run_dir(fixture.root_path(), app_id).c_str(), &info) == 0);
    }

    // A socket owned by a live listener is never treated as stale (this is the
    // concurrent-start / Supervisor-state-stale race guard).
    const int live = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(live >= 0);
    if (live >= 0) {
        struct sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
        CHECK(::bind(live, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(::listen(live, 1) == 0);
        error = fixture.lifecycle->prepare_runtime(app_id, socket_path);
        CHECK(!error.ok());
        CHECK_STR_EQ(error_slug(error), "backend_running");
        CHECK(::lstat(socket_path.c_str(), &info) == 0);
        CHECK(::close(live) == 0);
        CHECK(::unlink(socket_path.c_str()) == 0);
    }

    // A non-socket file at the socket path is never silently unlinked.
    {
        std::ofstream out(socket_path, std::ios::binary | std::ios::trunc);
        out << "not a socket";
    }
    error = fixture.lifecycle->prepare_runtime(app_id, socket_path);
    CHECK(!error.ok());
    CHECK_STR_EQ(error_slug(error), "backend_unavailable");
    CHECK(::unlink(socket_path.c_str()) == 0);
}

void test_permission_denied_for_other_identity() {
    if (::geteuid() == 0) {
        std::printf("permission-denied path skipped: tests are running as root\n");
        return;
    }
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    const std::string app_id = "io.example.denied";
    create_app(fixture, app_id, "Denied App", true, true);
    write_executable(fixture, app_id);

    struct passwd* other = ::getpwnam("nobody");
    if (other == nullptr) other = ::getpwnam("daemon");
    if (other == nullptr || other->pw_uid == ::geteuid()) {
        std::printf("permission-denied path skipped: no distinct unprivileged account\n");
        return;
    }
    struct group* other_group = ::getgrgid(other->pw_gid);
    const std::string group_name =
        other_group != nullptr && other_group->gr_name != nullptr
            ? other_group->gr_name
            : std::to_string(other->pw_gid);

    // Identity configuration is persisted, but unprivileged EdgeDesk records
    // that it cannot hand over the data directory.
    const Error identity =
        fixture.lifecycle->set_identity(app_id, other->pw_name, group_name, false);
    CHECK(identity.ok());
    BackendRecord record;
    bool found = false;
    CHECK(fixture.store().get_backend(app_id, record, found).ok());
    CHECK(found);
    CHECK_STR_EQ(record.run_user, other->pw_name);
    CHECK(record.error.rfind("backend_permission_denied:", 0) == 0);

    std::string socket_path;
    const Error prepared = fixture.lifecycle->prepare_runtime(app_id, socket_path);
    CHECK(!prepared.ok());
    CHECK_STR_EQ(error_slug(prepared), "backend_permission_denied");
}

void test_service_id_helper() {
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    create_app(fixture, "io.example.sid", "Sid App", true, true);

    std::string id;
    bool found = false;
    CHECK(fixture.lifecycle->service_id("io.example.sid", id, found).ok());
    CHECK(found);
    CHECK_STR_EQ(id, backend_service_id("io.example.sid"));

    id.clear();
    found = true;
    CHECK(fixture.lifecycle->service_id("io.example.noapp", id, found).ok());
    CHECK_EQ(found, false);
    CHECK_STR_EQ(id, "");
}

void test_unknown_app() {
    Fixture fixture;
    CHECK(fixture.ready);
    if (!fixture.ready) return;
    nlohmann::json backend;
    const Error error = fixture.lifecycle->describe("io.example.unknown", backend);
    CHECK(!error.ok());
    CHECK_STR_EQ(error_slug(error), "app_not_found");
}

} // namespace

int main() {
    test_describe_web_only();
    test_describe_backend_manifest_without_record();
    test_describe_backend();
    test_service_params();
    test_service_name_is_sanitized();
    test_identity_required();
    test_arch_mismatch_and_missing_executable();
    test_root_requires_allow_root();
    test_autostart_is_independent_and_defaults_off();
    test_prepare_runtime_and_stale_socket();
    test_permission_denied_for_other_identity();
    test_service_id_helper();
    test_unknown_app();
    return edgedesk::test::finish("native_backend_lifecycle_test");
}
