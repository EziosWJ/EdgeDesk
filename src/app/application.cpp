#include "app/application.hpp"

#include "core/util.hpp"
#include "file/file_service.hpp"
#include "file/path_sandbox.hpp"
#include "http/middleware.hpp"
#include "http/server.hpp"
#include "security/audit.hpp"
#include "security/auth.hpp"
#include "security/rate_limiter.hpp"
#include "system/system_metrics.hpp"
#include "terminal/terminal_manager.hpp"
#include "web/web_root.hpp"

#include "hlog.h"

#include <csignal>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <stdexcept>
#include <unistd.h>

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
    stopping_.store(true);
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
    files_ = std::make_unique<FileService>(*sandbox_, file_options);

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

    AuthOptions auth_options;
    auth_options.token = config_.auth_token;
    auth_options.session_ttl_seconds = config_.session_ttl_seconds;
    auth_ = std::make_unique<Auth>(auth_options);

    api_limiter_ = std::make_unique<RateLimiter>(config_.rate_limit_per_second, config_.rate_limit_burst);
    login_limiter_ =
        std::make_unique<RateLimiter>(static_cast<double>(config_.login_rate_limit_per_minute) / 60.0,
                                      static_cast<double>(config_.login_rate_limit_per_minute));
    transfer_gate_ = std::make_unique<ConcurrencyGate>(config_.max_transfers);
    web_root_ = std::make_unique<WebRoot>(config_.web_root);
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
    if (config_.auth_token_ephemeral) {
        std::cout << "EdgeDesk access token (generated for this run): " << config_.auth_token << "\n"
                  << "  Use it on the login screen, or set --auth-token-file / EDGEDESK_AUTH_TOKEN to fix it.\n";
    } else if (!config_.auth_token_file.empty()) {
        std::cout << "EdgeDesk access token: loaded from " << config_.auth_token_file << " (not printed)\n";
    } else {
        std::cout << "EdgeDesk access token: provided via command line / environment (not printed)\n";
    }
    std::cout << "  file root: " << config_.file_root
              << " (symlinks: " << (config_.follow_symlinks ? "followed inside root" : "blocked") << ")\n"
              << "  state dir: " << config_.state_dir << "\n"
              << "  audit log: " << (config_.audit_enabled ? config_.audit_log_path : std::string("disabled"))
              << "\n"
              << "  terminals: max " << config_.max_terminal_sessions << ", shell " << config_.bash_path
              << (terminals_->shell_available() ? "" : " (NOT FOUND - terminal creation will fail)") << "\n"
              << "  metrics: every " << config_.metrics_interval_ms << " ms, "
              << config_.metrics_history << " samples in memory\n"
              << std::flush;
}

void Application::start_maintenance() {
    maintenance_ = std::thread([this] {
        std::mutex mutex;
        std::condition_variable wakeup;
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping_.load()) {
            wakeup.wait_for(lock, std::chrono::milliseconds(kMaintenanceIntervalMs),
                            [this] { return stopping_.load(); });
            if (stopping_.load()) break;
            const auto now_ms = now_unix_millis();
            terminals_->sweep(now_ms);
            api_limiter_->sweep(now_ms);
            login_limiter_->sweep(now_ms);
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
    context.audit = audit_.get();
    context.api_limiter = api_limiter_.get();
    context.login_limiter = login_limiter_.get();
    context.transfer_gate = transfer_gate_.get();

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

    stopping_.store(true);
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
