#include "app/application.hpp"
#include "app/supervisor_service.hpp"

#include "core/util.hpp"
#include "extensions/backend_proxy.hpp"
#include "extensions/extension_runtime.hpp"
#include "extensions/native_backend.hpp"
#include "extensions/native_backend_installer.hpp"
#include "extensions/native_backend_lifecycle.hpp"
#include "extensions/package_manager.hpp"
#include "file/file_lock.hpp"
#include "file/file_service.hpp"
#include "file/path_sandbox.hpp"
#include "file/protected_paths.hpp"
#include "http/middleware.hpp"
#include "http/server.hpp"
#include "security/audit.hpp"
#include "security/auth.hpp"
#include "security/auth_store.hpp"
#include "security/rate_limiter.hpp"
#include "security/settings_store.hpp"
#include "system/system_metrics.hpp"
#include "terminal/terminal_manager.hpp"
#include "web/web_root.hpp"

#include "hlog.h"

#include <csignal>
#include <condition_variable>
#include <chrono>
#include <iostream>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <stdexcept>
#include <thread>

namespace edgedesk {
namespace {

constexpr int kMaintenanceIntervalMs = 30 * 1000;

std::string getenv_or(const char* name, const std::string& fallback) {
    const char* value = ::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : fallback;
}

} // namespace

Application::Application(Config config) : config_(std::move(config)) {}

Application::~Application() {
    request_stop();
    if (maintenance_.joinable()) maintenance_.join();
    if (metrics_) metrics_->stop();
    // TerminalManager is destroyed after the server stops; its destructor
    // terminates and joins every PTY reader thread.
}

void Application::initialize() {
    std::string error;
    if (!ensure_state_dir(config_.state_dir, error)) {
        throw std::runtime_error(error);
    }

    AuthOptions auth_options;
    auth_options.token = config_.auth_token;
    auth_options.session_ttl_seconds = config_.session_ttl_seconds;
    auth_options.cookie_secure = config_.cookie_secure;
    auth_options.data_dir = config_.data_dir;
    auth_ = std::make_unique<Auth>(std::move(auth_options));
    settings_ = SettingsStore::open(config_.data_dir, error);
    if (!settings_) {
        throw std::runtime_error(error);
    }

    AuditOptions audit_options;
    audit_options.path = config_.audit_log_path;
    audit_options.max_bytes = config_.audit_max_bytes;
    audit_options.max_files = config_.audit_max_files;
    audit_options.enabled = config_.audit_enabled;
    Error audit_error;
    audit_ = AuditLog::create(audit_options, audit_error);
    if (!audit_) {
        throw std::runtime_error(audit_error.message);
    }

    Error sandbox_error;
    sandbox_ = PathSandbox::create(config_.file_root, config_.follow_symlinks, sandbox_error);
    if (!sandbox_) {
        throw std::runtime_error(sandbox_error.message);
    }

    FileServiceOptions file_options;
    file_options.max_upload_size = config_.max_upload_size;
    file_options.max_text_edit_size = config_.max_text_edit_size;
    file_locks_ = std::make_unique<FileLockManager>();
    files_ = std::make_unique<FileService>(*sandbox_, file_options);
    files_->set_lock_manager(file_locks_.get());

    TerminalManagerOptions terminal_options;
    terminal_options.max_sessions = config_.max_terminal_sessions;
    terminal_options.idle_timeout_seconds = config_.terminal_idle_timeout_seconds;
    terminal_options.backlog_bytes = config_.terminal_backlog_bytes;
    terminal_options.bash_path = config_.bash_path;
    terminal_options.default_cwd = getenv_or("HOME", "/");
    terminals_ = std::make_unique<TerminalManager>(terminal_options);

    SystemMetrics::Options metrics_options;
    metrics_options.interval_ms = config_.metrics_interval_ms;
    metrics_options.history = config_.metrics_history;
    metrics_options.disk_refresh_cycles = config_.disk_refresh_cycles;
    metrics_options.disk_mounts = config_.disk_mounts;
    metrics_ = std::make_unique<SystemMetrics>(metrics_options);

    api_limiter_ = std::make_unique<RateLimiter>(config_.rate_limit_per_second, config_.rate_limit_burst);
    login_limiter_ =
        std::make_unique<RateLimiter>(static_cast<double>(config_.login_rate_limit_per_minute) / 60.0,
                                      static_cast<double>(config_.login_rate_limit_per_minute));
    setup_limiter_ =
        std::make_unique<RateLimiter>(static_cast<double>(config_.login_rate_limit_per_minute) / 60.0,
                                      static_cast<double>(config_.login_rate_limit_per_minute));
    transfer_gate_ = std::make_unique<ConcurrencyGate>(config_.max_transfers);
    web_root_ = std::make_unique<WebRoot>(config_.web_root);

    if (config_.supervisor_enabled) {
        supervisor::SupervisorClientOptions client_options;
        client_options.socket_path = config_.supervisor_socket;
        Error client_error;
        auto client = supervisor::SupervisorClient::create(client_options, client_error);
        if (!client) {
            throw std::runtime_error("cannot configure process supervisor client: " +
                                     client_error.message);
        }
        // Two workers keep the libhv loop responsive while preserving a strict
        // upper bound on concurrent privileged requests.
        supervisor_service_ = std::make_unique<SupervisorService>(std::move(client), 2, 32);
        // Upgrade needs to prove the backend stopped before replacing the
        // stable executable. This call is bounded and only used by install,
        // which already performs synchronous package work.
        supervisor::SupervisorClientOptions stop_options;
        stop_options.socket_path = config_.supervisor_socket;
        stop_options.connect_timeout_ms = 1500;
        stop_options.request_timeout_ms = 15000;
        Error stop_client_error;
        backend_stop_client_ = supervisor::SupervisorClient::create(stop_options, stop_client_error);
        if (!backend_stop_client_) {
            throw std::runtime_error("cannot configure supervisor stop client: " +
                                     stop_client_error.message);
        }
    }

    Error extension_error;
    packages_ = extensions::PackageManager::create(config_.apps_root, extension_error);
    if (!packages_) {
        std::cerr << "WARNING: extension platform unavailable (apps root " << config_.apps_root
                  << "): " << extension_error.message << "\n"
                  << "         Core APIs remain available; pass --apps-root to a writable directory.\n";
    } else {
        // Extension calls get their own sandbox over the same root. The
        // protected set covers platform state that must never be reachable
        // through files.manage even when --file-root is "/". It is compiled
        // after the package manager has created apps_root so that existing
        // descendants (including SQLite files and revisions) are indexed for
        // hard-link alias detection.
        PathSandbox::Options extension_options;
        extension_options.protected_paths = platform_protected_paths(config_);
        extension_sandbox_ =
            PathSandbox::create(config_.file_root, config_.follow_symlinks, extension_options, sandbox_error);
        if (!extension_sandbox_) {
            throw std::runtime_error(sandbox_error.message);
        }
        extension_files_ = std::make_unique<FileService>(*extension_sandbox_, file_options);
        extension_files_->set_lock_manager(file_locks_.get());
        extension_runtime_ = std::make_unique<extensions::ExtensionRuntime>(
            packages_->store(), *packages_, *extension_files_, terminals_.get());
        // SPEC-0011: one optional native backend per application. Installing an
        // installer is what turns an otherwise Web-only package manager into
        // one that accepts `backend` packages; it still never executes them.
        native_installer_ =
            std::make_unique<extensions::NativeBackendInstaller>(packages_->store(), config_.apps_root);
        packages_->set_backend_installer(native_installer_.get());
        packages_->set_backend_stop_hook(
            [this](std::string_view app_id) { return stop_backend_for_upgrade(app_id); });
        packages_->set_backend_service_remove_hook(
            [this](std::string_view service_id) { return remove_backend_service(service_id); });
        native_lifecycle_ = std::make_unique<extensions::NativeBackendLifecycle>(
            packages_->store(), *packages_, *native_installer_, config_.apps_root);
        backend_proxy_ =
            std::make_unique<extensions::BackendProxy>(packages_->store(), config_.apps_root);
    }
}

Error Application::stop_backend_for_upgrade(std::string_view app_id) {
    if (!packages_) return Error::none();
    extensions::BackendRecord record;
    bool found = false;
    Error error = packages_->store().get_backend(app_id, record, found);
    if (!error.ok()) return error;
    if (!found) return Error::none();
    if (!backend_stop_client_) {
        return Error::make(ErrorCode::unavailable,
                           "process supervisor is not configured; cannot confirm the native "
                           "backend is stopped before upgrading");
    }

    using supervisor::SupervisorRpcError;
    const auto query_state = [&](std::string& state, bool& exists) -> Error {
        nlohmann::json result;
        SupervisorRpcError rpc;
        Error call_error =
            backend_stop_client_->call("service.get", {{"id", record.service_id}}, result, rpc);
        if (!call_error.ok()) return call_error;
        if (rpc.code != 0) {
            if (rpc.slug == "service_not_found") {
                exists = false;
                state.clear();
                return Error::none();
            }
            return Error::make(ErrorCode::unavailable,
                               "supervisor rejected service.get: " + rpc.message);
        }
        exists = true;
        state = result.value("state", "");
        return Error::none();
    };

    std::string state;
    bool exists = false;
    error = query_state(state, exists);
    if (!error.ok()) return error;
    if (!exists || state == "STOPPED" || state == "FAILED" || state == "BACKOFF") {
        return Error::none();
    }

    nlohmann::json operation;
    SupervisorRpcError rpc;
    error = backend_stop_client_->call("service.stop", {{"id", record.service_id}}, operation, rpc);
    if (!error.ok()) return error;
    if (rpc.code != 0 && rpc.slug != "service_not_found") {
        return Error::make(ErrorCode::unavailable,
                           "supervisor rejected service.stop: " + rpc.message);
    }

    // service.stop is asynchronous in the Supervisor. Poll the authoritative
    // state for a short bounded period; never overwrite a possibly running ELF.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        error = query_state(state, exists);
        if (!error.ok()) return error;
        if (!exists || state == "STOPPED" || state == "FAILED" || state == "BACKOFF") {
            return Error::none();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return Error::make(ErrorCode::unavailable,
                               "native backend did not stop within 20 seconds; operation aborted");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

Error Application::remove_backend_service(std::string_view service_id) {
    if (!backend_stop_client_) {
        return Error::make(ErrorCode::unavailable,
                           "process supervisor is not configured; cannot remove the native "
                           "backend service");
    }
    nlohmann::json result;
    supervisor::SupervisorRpcError rpc;
    Error error = backend_stop_client_->call(
        "service.delete", {{"id", std::string(service_id)}}, result, rpc);
    if (!error.ok()) return error;
    if (rpc.code != 0 && rpc.slug != "service_not_found") {
        return Error::make(ErrorCode::unavailable,
                           "supervisor rejected service.delete: " + rpc.message);
    }
    return Error::none();
}

void Application::print_banner() const {
#if !EDGEDESK_EMBED_ASSETS
    // Disk mode without assets still serves the API and terminals; the warning
    // makes the missing UI explicit instead of failing startup.
    struct stat info {};
    if (config_.web_root.empty() || ::stat(config_.web_root.c_str(), &info) != 0) {
        std::cerr << "WARNING: frontend directory not found (" << config_.web_root
                  << "); API and terminal remain available, the web UI will return 404.\\n"
                  << "         Point --web-root at a built web/dist directory to serve the UI.\\n";
    }
#endif
    const auto auth_state = auth_->state();
    std::cout << "  auth: " << auth_state_name(auth_state);
    if (config_.auth_token.empty()) {
        std::cout << " (Bearer API token disabled)\n";
    } else if (!config_.auth_token_file.empty()) {
        std::cout << " (Bearer API token loaded from " << config_.auth_token_file << ")\n";
    } else {
        std::cout << " (Bearer API token configured; not printed)\n";
    }
    if (auth_state == AuthState::uninitialized) {
        std::cout << "  Open the web UI and set the administrator password.\n";
    } else if (auth_state == AuthState::corrupt) {
        std::cout << "  WARNING: authentication state is corrupt (" << auth_->last_error() << ")\n"
                  << "  Business APIs are disabled. Stop the service and run:\n"
                  << "    edgedesk --reset-admin-password --data-dir " << config_.data_dir << "\n";
    }
    std::cout << "  file root: " << config_.file_root
              << " (symlinks: " << (config_.follow_symlinks ? "followed inside root" : "blocked") << ")\n"
              << "  data dir: " << config_.data_dir << "\n"
              << "  state dir: " << config_.state_dir << "\n"
              << "  audit log: " << (config_.audit_enabled ? config_.audit_log_path : std::string("disabled"))
              << "\n"
              << "  terminals: max " << config_.max_terminal_sessions << ", shell " << config_.bash_path
              << (terminals_->shell_available() ? "" : " (NOT FOUND - terminal creation will fail)") << "\n"
              << "  metrics: every " << config_.metrics_interval_ms << " ms, "
              << config_.metrics_history << " samples in memory\n"
              << "  supervisor: "
              << (config_.supervisor_enabled ? config_.supervisor_socket : std::string("disabled"))
              << "\n"
              << std::flush;
}

void Application::request_stop() {
    {
        std::lock_guard<std::mutex> lock(maintenance_mutex_);
        stopping_.store(true);
    }
    maintenance_wakeup_.notify_all();
}

void Application::start_maintenance() {
    maintenance_ = std::thread([this] {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(maintenance_mutex_);
                maintenance_wakeup_.wait_for(lock, std::chrono::milliseconds(kMaintenanceIntervalMs),
                                             [this] { return stopping_.load(); });
                if (stopping_.load()) return;
            }
            // Do the periodic work without holding the mutex: request_stop()
            // must not wait for a sweep or an audit prune to finish.
            const auto now_ms = now_unix_millis();
            terminals_->sweep(now_ms);
            api_limiter_->sweep(now_ms);
            login_limiter_->sweep(now_ms);
            setup_limiter_->sweep(now_ms);
            if (extension_runtime_) extension_runtime_->sweep(now_ms);
            const std::int64_t now_seconds = now_unix_seconds();
            if (audit_ && audit_->enabled() &&
                (last_audit_prune_ == 0 || now_seconds - last_audit_prune_ >= 24 * 3600)) {
                last_audit_prune_ = now_seconds;
                std::size_t removed = 0;
                std::string prune_error;
                const std::int64_t cutoff =
                    now_seconds - static_cast<std::int64_t>(config_.audit_retention_days) * 86400;
                if (audit_->prune_older_than(cutoff, removed, prune_error)) {
                    audit_->record("audit.prune", "ok", std::to_string(removed) + " records removed");
                } else {
                    audit_->record("audit.prune", "failed", prune_error);
                }
            }
        }
    });
}

int Application::run() {
    // libhv defaults to writing "<cwd>/libhv.<date>.log" without rotation,
    // which is unacceptable on a read-only or space-constrained device. Route
    // its logger to stderr so the service manager owns log retention.
    hlog_set_file("/dev/stderr");

    // Block SIGINT/SIGTERM before any worker thread exists, then wait for them
    // on a dedicated thread so the event loop can shut down cleanly.
    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    sigaddset(&signal_set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signal_set, nullptr) != 0) {
        throw std::runtime_error("failed to block termination signals");
    }

    initialize();
    print_banner();
    if (!terminals_->shell_available()) {
        std::cerr << "WARNING: " << config_.bash_path
                  << " is not executable; EdgeDesk will refuse to create terminals instead of falling back to sh.\n";
    }
    metrics_->start();
    start_maintenance();

    RouteContext context;
    context.config = &config_;
    context.web_root = web_root_.get();
    context.files = files_.get();
    context.terminals = terminals_.get();
    context.metrics = metrics_.get();
    context.auth = auth_.get();
    context.settings = settings_.get();
    context.audit = audit_.get();
    context.api_limiter = api_limiter_.get();
    context.login_limiter = login_limiter_.get();
    context.setup_limiter = setup_limiter_.get();
    context.transfer_gate = transfer_gate_.get();
    context.packages = packages_.get();
    context.extension_runtime = extension_runtime_.get();
    context.native_backends = native_lifecycle_.get();
    context.backend_proxy = backend_proxy_.get();
    context.supervisor = supervisor_service_.get();

    Server server(context, config_);
    std::thread signal_thread([&signal_set, &server] {
        int signal_number = 0;
        for (;;) {
            if (sigwait(&signal_set, &signal_number) != 0) continue;
            std::cout << "\nReceived signal " << signal_number << ", shutting down..." << std::endl;
            server.stop();
            return;
        }
    });

    const int result = server.run();

    request_stop();
    if (signal_thread.joinable()) {
        ::pthread_kill(signal_thread.native_handle(), SIGTERM);
        signal_thread.join();
    }
    if (maintenance_.joinable()) maintenance_.join();
    metrics_->stop();
    terminals_->shutdown();
    audit_->record("service", "stop", "", "");
    std::cout << "EdgeDesk stopped" << std::endl;
    return result;
}

} // namespace edgedesk
