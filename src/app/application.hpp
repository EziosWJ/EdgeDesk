#pragma once

#include "core/config.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace edgedesk {

class Auth;
class AuditLog;
class ConcurrencyGate;
class FileService;
class PathSandbox;
class RateLimiter;
class SettingsStore;
class SystemMetrics;
class TerminalManager;
class WebRoot;
class FileLockManager;
class SupervisorService;

namespace extensions {
class ExtensionRuntime;
class PackageManager;
} // namespace extensions

// Application owns every service for the process lifetime and wires them into
// the HTTP server. Dependencies are explicit; no service singletons.
class Application {
public:
    explicit Application(Config config);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Returns a process exit code. Throws std::runtime_error on fatal setup
    // errors (message is user-facing).
    int run();

private:
    void initialize();
    void print_banner() const;
    void start_maintenance();

    Config config_;
    std::unique_ptr<Auth> auth_;
    std::unique_ptr<SettingsStore> settings_;
    std::unique_ptr<AuditLog> audit_;
    std::unique_ptr<PathSandbox> sandbox_;
    std::unique_ptr<FileService> files_;
    // Separate sandbox/service for extension files.* calls. It shares the
    // file root and symlink policy but adds the platform-internal protected
    // paths, so the administrator file manager keeps its existing semantics.
    std::unique_ptr<PathSandbox> extension_sandbox_;
    std::unique_ptr<FileService> extension_files_;
    std::unique_ptr<TerminalManager> terminals_;
    std::unique_ptr<SystemMetrics> metrics_;
    std::unique_ptr<RateLimiter> api_limiter_;
    std::unique_ptr<RateLimiter> login_limiter_;
    std::unique_ptr<RateLimiter> setup_limiter_;
    std::unique_ptr<ConcurrencyGate> transfer_gate_;
    std::unique_ptr<WebRoot> web_root_;
    std::unique_ptr<FileLockManager> file_locks_;
    std::unique_ptr<extensions::PackageManager> packages_;
    std::unique_ptr<extensions::ExtensionRuntime> extension_runtime_;
    std::unique_ptr<SupervisorService> supervisor_service_;

    std::thread maintenance_;
    std::atomic<bool> stopping_{false};
    std::int64_t last_audit_prune_ = 0;
};

} // namespace edgedesk
