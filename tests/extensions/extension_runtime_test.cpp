#include "test_util.hpp"
#include "extensions/zip_builder.hpp"

#include "core/util.hpp"
#include "extensions/extension_runtime.hpp"
#include "extensions/package_manager.hpp"
#include "file/file_lock.hpp"
#include "file/file_service.hpp"
#include "file/path_sandbox.hpp"
#include "terminal/terminal_manager.hpp"
#include "terminal/terminal_session.hpp"

#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace edgedesk;
using namespace edgedesk::extensions;
using edgedesk::test::ZipItem;

namespace edgedesk::extensions {

// Narrow deterministic-time seam (friend of ExtensionRuntime). It never exists
// in production call paths; tests use it to age sessions and transfers without
// sleeping or introducing a production clock abstraction.
struct RuntimeTestAccess {
    static void age_session(ExtensionRuntime& runtime, const std::string& token, std::int64_t seconds) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        const auto it = runtime.sessions_.find(token);
        if (it != runtime.sessions_.end()) it->second.created_at = now_unix_seconds() - seconds;
    }

    static void age_upload(ExtensionRuntime& runtime, const std::string& transfer_id, std::int64_t idle_ms) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        const auto it = runtime.uploads_.find(transfer_id);
        if (it != runtime.uploads_.end()) it->second.last_activity_ms = now_unix_millis() - idle_ms;
    }

    static void age_download(ExtensionRuntime& runtime, const std::string& transfer_id, std::int64_t idle_ms) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        const auto it = runtime.downloads_.find(transfer_id);
        if (it != runtime.downloads_.end()) it->second.last_activity_ms = now_unix_millis() - idle_ms;
    }

    static std::size_t session_count(ExtensionRuntime& runtime) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        return runtime.sessions_.size();
    }

    static std::size_t upload_count(ExtensionRuntime& runtime) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        return runtime.uploads_.size();
    }

    static std::size_t download_count(ExtensionRuntime& runtime) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        return runtime.downloads_.size();
    }

    static std::size_t terminal_set_count(ExtensionRuntime& runtime) {
        std::lock_guard<std::mutex> lock(runtime.mutex_);
        return runtime.terminal_sessions_.size();
    }
};

} // namespace edgedesk::extensions

namespace {

namespace fs = std::filesystem;

constexpr const char* kAppId = "io.example.runtime";
constexpr const char* kOtherAppId = "io.example.other";
constexpr const char* kInstanceId = "instance-0001";

struct Fixture {
    fs::path base;
    fs::path sandbox_root;
    std::unique_ptr<PathSandbox> sandbox;
    FileLockManager locks;
    std::unique_ptr<FileService> files;
    std::unique_ptr<PackageManager> packages;
    std::unique_ptr<TerminalManager> terminals;
    std::unique_ptr<ExtensionRuntime> runtime;
    bool ok = false;

    Fixture(const fs::path& parent, const std::string& name) {
        base = parent / name;
        std::error_code ignored;
        fs::remove_all(base, ignored);
        sandbox_root = base / "sandbox";
        fs::create_directories(sandbox_root);
        Error error;
        sandbox = PathSandbox::create(sandbox_root.string(), false, error);
        if (!sandbox) {
            std::fprintf(stderr, "sandbox error: %s\n", error.message.c_str());
            return;
        }
        files = std::make_unique<FileService>(*sandbox);
        files->set_lock_manager(&locks);
        packages = PackageManager::create((base / "apps").string(), error);
        if (!packages) {
            std::fprintf(stderr, "package manager error: %s\n", error.message.c_str());
            return;
        }
        TerminalManagerOptions options;
        options.bash_path = "/bin/bash";
        terminals = std::make_unique<TerminalManager>(options);
        runtime = std::make_unique<ExtensionRuntime>(packages->store(), *packages, *files, terminals.get());
        ok = true;
    }

    fs::path journal_directory() const {
        return fs::path(packages->temporary_directory()) / "transfers";
    }
};

nlohmann::json manifest_json(const std::string& app_id, const std::string& version) {
    return {
        {"id", app_id},
        {"name", "Runtime Test App"},
        {"version", version},
        {"entry", "index.html"},
        {"minHostApiVersion", "1.0.0"},
        {"maxHostApiVersion", "1.9.9"},
        {"permissions", nlohmann::json::array({"files.manage"})},
        {"window", {{"defaultWidth", 800}, {"defaultHeight", 600}, {"minWidth", 320}, {"minHeight", 240},
                    {"instances", "single"}}},
    };
}

bool install_app(Fixture& fixture, const std::string& app_id, const std::string& version = "1.0.0",
                 bool approve = true) {
    std::vector<ZipItem> items = {
        {"manifest.json", manifest_json(app_id, version).dump(), 0, false, ""},
        {"index.html", "<h1>runtime</h1>", 0, false, ""},
    };
    const auto zip = (fixture.base / (app_id + "-" + version + ".zip")).string();
    if (!edgedesk::test::write_zip(zip, items)) return false;
    InstallOptions options;
    InstallResult result;
    Error error = fixture.packages->install(zip, options, result);
    if (!error.ok()) {
        std::fprintf(stderr, "install error: %s\n", error.message.c_str());
        return false;
    }
    if (!approve) return true;
    AppRecord app;
    error = fixture.packages->approve(app_id, {"files.manage"}, true, app);
    if (!error.ok()) {
        std::fprintf(stderr, "approve error: %s\n", error.message.c_str());
        return false;
    }
    return true;
}

bool create_session(Fixture& fixture, const std::string& app_id, RuntimeSession& out) {
    Error error = fixture.runtime->create_session(app_id, kInstanceId, out);
    if (!error.ok()) std::fprintf(stderr, "create_session error: %s\n", error.message.c_str());
    return error.ok();
}

std::size_t count_files(const fs::path& directory, const std::string& prefix, const std::string& suffix) {
    std::size_t count = 0;
    std::error_code error;
    if (!fs::exists(directory, error)) return 0;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.size() < prefix.size() + suffix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        ++count;
    }
    return count;
}

std::shared_ptr<TerminalSession> create_tracked_terminal(Fixture& fixture, const std::string& token,
                                                         std::string& session_id) {
    if (!fixture.terminals->shell_available()) return nullptr;
    TerminalManager::CreateRequest request;
    request.title = "runtime-test";
    request.cwd = fixture.sandbox_root.string();
    std::shared_ptr<TerminalSession> terminal;
    Error error = fixture.terminals->create(request, terminal);
    if (!error.ok()) {
        std::fprintf(stderr, "terminal error: %s\n", error.message.c_str());
        return nullptr;
    }
    session_id = terminal->id();
    fixture.runtime->track_terminal(token, session_id);
    return terminal;
}

void write_file(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
}

// Verifies that a teardown reclaimed every resource: no session, transfers,
// temporary upload files, journals, file locks or registered terminals remain.
void check_all_resources_released(Fixture& fixture, const std::string& token,
                                  const std::shared_ptr<TerminalSession>& terminal,
                                  const std::string& terminal_id) {
    RuntimeSession session;
    CHECK(!fixture.runtime->validate(token, session).ok());
    CHECK_EQ(RuntimeTestAccess::session_count(*fixture.runtime), std::size_t{0});
    CHECK_EQ(RuntimeTestAccess::upload_count(*fixture.runtime), std::size_t{0});
    CHECK_EQ(RuntimeTestAccess::download_count(*fixture.runtime), std::size_t{0});
    CHECK_EQ(RuntimeTestAccess::terminal_set_count(*fixture.runtime), std::size_t{0});
    CHECK_EQ(fixture.locks.active_locks(), std::size_t{0});
    CHECK_EQ(count_files(fixture.sandbox_root, ".edgedesk-upload-", ".tmp"), std::size_t{0});
    CHECK_EQ(count_files(fixture.journal_directory(), "", ".journal"), std::size_t{0});
    if (!terminal_id.empty()) {
        CHECK(fixture.terminals->find(terminal_id) == nullptr);
    }
    if (terminal) {
        terminal->join();
        CHECK(!terminal->running());
    }
}

// Proves that the lock taken by the reclaimed upload/download is really free.
void check_locks_reusable(Fixture& fixture) {
    FileService::ReadHandle handle;
    CHECK(fixture.files->open_for_read("/existing.txt", handle).ok());
    handle = FileService::ReadHandle();
    FileService::Upload upload;
    CHECK(fixture.files->begin_upload("/existing.txt", upload).ok());
    upload.abort();
}

void test_invalidate_app_reclaims_everything(const fs::path& parent) {
    Fixture fixture(parent, "invalidate-app");
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    CHECK(install_app(fixture, kAppId));
    write_file(fixture.sandbox_root / "existing.txt", "download source");
    RuntimeSession session;
    CHECK(create_session(fixture, kAppId, session));
    CHECK_EQ(session.app_id, std::string(kAppId));

    std::string terminal_id;
    auto terminal = create_tracked_terminal(fixture, session.token, terminal_id);

    TransferInfo upload;
    CHECK(fixture.runtime->begin_upload(session, "/upload.bin", 8, upload).ok());
    CHECK_EQ(count_files(fixture.sandbox_root, ".edgedesk-upload-", ".tmp"), std::size_t{1});
    CHECK_EQ(count_files(fixture.journal_directory(), "", ".journal"), std::size_t{1});
    TransferInfo download;
    CHECK(fixture.runtime->begin_download(session, "/existing.txt", download).ok());
    CHECK_EQ(fixture.locks.active_locks(), std::size_t{2});

    fixture.runtime->invalidate_app(kAppId);
    check_all_resources_released(fixture, session.token, terminal, terminal_id);
    check_locks_reusable(fixture);

    // Repeated invalidation and removal are idempotent.
    fixture.runtime->invalidate_app(kAppId);
    fixture.runtime->remove_session(session.token);
    CHECK_EQ(RuntimeTestAccess::session_count(*fixture.runtime), std::size_t{0});

    // A stale session object cannot resurrect state.
    std::uint64_t received = 0;
    CHECK(!fixture.runtime->append_upload(session, upload.transfer_id, 0, "12345678", 8, received).ok());
    std::string data;
    bool eof = false;
    CHECK(!fixture.runtime->read_download(session, download.transfer_id, 0, data, eof).ok());
}

void test_validate_reclaims_disabled_app(const fs::path& parent) {
    Fixture fixture(parent, "validate-disabled");
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    CHECK(install_app(fixture, kAppId));
    write_file(fixture.sandbox_root / "existing.txt", "download source");
    RuntimeSession session;
    CHECK(create_session(fixture, kAppId, session));
    std::string terminal_id;
    auto terminal = create_tracked_terminal(fixture, session.token, terminal_id);
    TransferInfo upload;
    CHECK(fixture.runtime->begin_upload(session, "/disabled.bin", 4, upload).ok());
    TransferInfo download;
    CHECK(fixture.runtime->begin_download(session, "/existing.txt", download).ok());

    AppRecord app;
    CHECK(fixture.packages->set_enabled(kAppId, false, app).ok());
    // validate() itself must tear down the invalid session and its resources.
    check_all_resources_released(fixture, session.token, terminal, terminal_id);
    check_locks_reusable(fixture);
}

void test_validate_reclaims_uninstalled_app(const fs::path& parent) {
    Fixture fixture(parent, "validate-uninstalled");
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    CHECK(install_app(fixture, kAppId));
    RuntimeSession session;
    CHECK(create_session(fixture, kAppId, session));
    std::string terminal_id;
    auto terminal = create_tracked_terminal(fixture, session.token, terminal_id);
    TransferInfo upload;
    CHECK(fixture.runtime->begin_upload(session, "/uninstalled.bin", 4, upload).ok());

    CHECK(fixture.packages->uninstall(kAppId, true).ok());
    check_all_resources_released(fixture, session.token, terminal, terminal_id);
    CHECK_EQ(fixture.locks.active_locks(), std::size_t{0});
}

void test_validate_reclaims_upgraded_app(const fs::path& parent) {
    Fixture fixture(parent, "validate-upgraded");
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    CHECK(install_app(fixture, kAppId, "1.0.0"));
    RuntimeSession session;
    CHECK(create_session(fixture, kAppId, session));
    TransferInfo upload;
    CHECK(fixture.runtime->begin_upload(session, "/upgraded.bin", 4, upload).ok());
    CHECK_EQ(count_files(fixture.journal_directory(), "", ".journal"), std::size_t{1});

    CHECK(install_app(fixture, kAppId, "1.1.0", false));
    RuntimeSession out;
    Error error = fixture.runtime->validate(session.token, out);
    CHECK(!error.ok());
    CHECK(error.code == ErrorCode::permission_denied);
    check_all_resources_released(fixture, session.token, nullptr, std::string());
}

void test_invalidate_revision_is_isolated(const fs::path& parent) {
    Fixture fixture(parent, "revision-isolation");
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    CHECK(install_app(fixture, kAppId));
    CHECK(install_app(fixture, kOtherAppId));
    RuntimeSession first;
    RuntimeSession second;
    CHECK(create_session(fixture, kAppId, first));
    CHECK(create_session(fixture, kOtherAppId, second));

    TransferInfo first_upload;
    TransferInfo second_upload;
    CHECK(fixture.runtime->begin_upload(first, "/first.bin", 4, first_upload).ok());
    CHECK(fixture.runtime->begin_upload(second, "/second.bin", 4, second_upload).ok());

    fixture.runtime->invalidate_revision(first.revision_id);

    RuntimeSession out;
    CHECK(!fixture.runtime->validate(first.token, out).ok());
    CHECK(fixture.runtime->validate(second.token, out).ok());
    CHECK_STR_EQ(out.app_id, std::string(kOtherAppId));
    CHECK_EQ(RuntimeTestAccess::upload_count(*fixture.runtime), std::size_t{1});
    CHECK_EQ(RuntimeTestAccess::session_count(*fixture.runtime), std::size_t{1});
    std::uint64_t received = 0;
    CHECK(!fixture.runtime->append_upload(first, first_upload.transfer_id, 0, "abcd", 4, received).ok());
    CHECK(fixture.runtime->append_upload(second, second_upload.transfer_id, 0, "wxyz", 4, received).ok());
    CHECK_EQ(received, std::uint64_t{4});
    CHECK_EQ(count_files(fixture.journal_directory(), "", ".journal"), std::size_t{1});

    fixture.runtime->remove_session(second.token);
    CHECK_EQ(fixture.locks.active_locks(), std::size_t{0});
    CHECK_EQ(count_files(fixture.sandbox_root, ".edgedesk-upload-", ".tmp"), std::size_t{0});
    CHECK_EQ(count_files(fixture.journal_directory(), "", ".journal"), std::size_t{0});
}

void test_expired_session_validate(const fs::path& parent) {
    Fixture fixture(parent, "expired-validate");
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    CHECK(install_app(fixture, kAppId));
    write_file(fixture.sandbox_root / "existing.txt", "download source");
    RuntimeSession session;
    CHECK(create_session(fixture, kAppId, session));
    std::string terminal_id;
    auto terminal = create_tracked_terminal(fixture, session.token, terminal_id);
    TransferInfo upload;
    CHECK(fixture.runtime->begin_upload(session, "/expired.bin", 4, upload).ok());
    TransferInfo download;
    CHECK(fixture.runtime->begin_download(session, "/existing.txt", download).ok());

    RuntimeTestAccess::age_session(*fixture.runtime, session.token, kRuntimeSessionTtlSeconds + 30);
    RuntimeSession out;
    Error error = fixture.runtime->validate(session.token, out);
    CHECK(!error.ok());
    CHECK(error.code == ErrorCode::permission_denied);
    CHECK(error.message.find("expired") != std::string::npos);

    check_all_resources_released(fixture, session.token, terminal, terminal_id);
    check_locks_reusable(fixture);
}

void test_sweep_expired_sessions_and_idle_transfers(const fs::path& parent) {
    Fixture fixture(parent, "sweep");
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    CHECK(install_app(fixture, kAppId));
    write_file(fixture.sandbox_root / "existing.txt", "download source");
    RuntimeSession expired;
    RuntimeSession active;
    CHECK(create_session(fixture, kAppId, expired));
    CHECK(create_session(fixture, kAppId, active));

    std::string terminal_id;
    auto terminal = create_tracked_terminal(fixture, expired.token, terminal_id);
    std::string active_terminal_id;
    auto active_terminal = create_tracked_terminal(fixture, active.token, active_terminal_id);
    TransferInfo expired_upload;
    TransferInfo expired_download;
    CHECK(fixture.runtime->begin_upload(expired, "/expired.bin", 4, expired_upload).ok());
    CHECK(fixture.runtime->begin_download(expired, "/existing.txt", expired_download).ok());
    TransferInfo active_upload;
    CHECK(fixture.runtime->begin_upload(active, "/active.bin", 4, active_upload).ok());

    RuntimeTestAccess::age_session(*fixture.runtime, expired.token, kRuntimeSessionTtlSeconds + 30);
    const auto now_ms = now_unix_millis();
    fixture.runtime->sweep(now_ms);

    RuntimeSession out;
    CHECK(!fixture.runtime->validate(expired.token, out).ok());
    CHECK(fixture.runtime->validate(active.token, out).ok());
    CHECK_EQ(RuntimeTestAccess::upload_count(*fixture.runtime), std::size_t{1});
    CHECK_EQ(RuntimeTestAccess::download_count(*fixture.runtime), std::size_t{0});
    CHECK_EQ(RuntimeTestAccess::terminal_set_count(*fixture.runtime), std::size_t{1});
    CHECK_EQ(count_files(fixture.sandbox_root, ".edgedesk-upload-", ".tmp"), std::size_t{1});
    CHECK_EQ(count_files(fixture.journal_directory(), "", ".journal"), std::size_t{1});
    CHECK_EQ(fixture.locks.active_locks(), std::size_t{1});
    if (!terminal_id.empty()) {
        CHECK(fixture.terminals->find(terminal_id) == nullptr);
        if (terminal) {
            terminal->join();
            CHECK(!terminal->running());
        }
    }
    // The healthy session's terminal was not touched.
    if (!active_terminal_id.empty()) {
        CHECK(fixture.terminals->find(active_terminal_id) != nullptr);
        if (active_terminal) CHECK(active_terminal->running());
    }
    // The active transfer was not touched.
    std::uint64_t received = 0;
    CHECK(fixture.runtime->append_upload(active, active_upload.transfer_id, 0, "abcd", 4, received).ok());

    // Idle timeout uses the same sweep clock and reclaims the remaining task.
    RuntimeTestAccess::age_upload(*fixture.runtime, active_upload.transfer_id, kTransferIdleTimeoutMs + 1000);
    fixture.runtime->sweep(now_unix_millis());
    CHECK_EQ(RuntimeTestAccess::upload_count(*fixture.runtime), std::size_t{0});
    CHECK_EQ(fixture.locks.active_locks(), std::size_t{0});
    CHECK_EQ(count_files(fixture.sandbox_root, ".edgedesk-upload-", ".tmp"), std::size_t{0});
    CHECK_EQ(count_files(fixture.journal_directory(), "", ".journal"), std::size_t{0});
    // The session itself is still valid; only its idle transfer was reclaimed.
    CHECK(fixture.runtime->validate(active.token, out).ok());

    fixture.runtime->remove_session(active.token);
    if (active_terminal) {
        active_terminal->join();
        CHECK(!active_terminal->running());
    }
}

void test_concurrent_and_repeated_teardown(const fs::path& parent) {
    Fixture fixture(parent, "concurrent");
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    CHECK(install_app(fixture, kAppId));
    RuntimeSession session;
    CHECK(create_session(fixture, kAppId, session));
    std::string terminal_id;
    auto terminal = create_tracked_terminal(fixture, session.token, terminal_id);
    TransferInfo upload;
    CHECK(fixture.runtime->begin_upload(session, "/concurrent.bin", 4, upload).ok());

    std::promise<void> gate;
    std::shared_future<void> start = gate.get_future().share();
    std::thread remover1([&] {
        start.wait();
        fixture.runtime->remove_session(session.token);
    });
    std::thread remover2([&] {
        start.wait();
        fixture.runtime->remove_session(session.token);
    });
    std::thread validator([&] {
        start.wait();
        for (int i = 0; i < 200; ++i) {
            RuntimeSession out;
            (void)fixture.runtime->validate(session.token, out);
        }
    });
    std::thread sweeper([&] {
        start.wait();
        for (int i = 0; i < 200; ++i) fixture.runtime->sweep(now_unix_millis());
    });
    gate.set_value();
    remover1.join();
    remover2.join();
    validator.join();
    sweeper.join();

    check_all_resources_released(fixture, session.token, terminal, terminal_id);
    // A third teardown after everyone else finished stays a no-op.
    fixture.runtime->remove_session(session.token);
    fixture.runtime->invalidate_app(kAppId);
    CHECK_EQ(RuntimeTestAccess::session_count(*fixture.runtime), std::size_t{0});
    CHECK_EQ(fixture.locks.active_locks(), std::size_t{0});
}

} // namespace

int main() {
    const auto parent = fs::temp_directory_path() /
                        ("edgedesk-runtime-test-" + std::to_string(::getpid()));
    std::error_code ignored;
    fs::remove_all(parent, ignored);
    fs::create_directories(parent);
    test_invalidate_app_reclaims_everything(parent);
    test_validate_reclaims_disabled_app(parent);
    test_validate_reclaims_uninstalled_app(parent);
    test_validate_reclaims_upgraded_app(parent);
    test_invalidate_revision_is_isolated(parent);
    test_expired_session_validate(parent);
    test_sweep_expired_sessions_and_idle_transfers(parent);
    test_concurrent_and_repeated_teardown(parent);
    fs::remove_all(parent, ignored);
    return edgedesk::test::finish("extension_runtime_test");
}
