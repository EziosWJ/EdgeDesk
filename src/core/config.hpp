#pragma once

#include <cstdint>
#include <string>
#include <vector>

// CLI / config-file / environment configuration.
// This header intentionally contains no libhv or HTTP types.
// Precedence: defaults < config file < environment < command line.

namespace edgedesk {

struct Config {
    // network
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    int http_threads = 1;

    // static assets (disk mode only; embed mode ignores web_root)
    std::string web_root;
    bool web_root_explicit = false; // set by --web-root; the compile-time
                                    // default may legitimately be absent at
                                    // deployment time (warning, not fatal)

    // file service
    std::string file_root = "/";
    bool follow_symlinks = false;
    std::uint64_t max_request_body = 1ull * 1024 * 1024;
    std::uint64_t max_upload_size = 256ull * 1024 * 1024;
    std::uint64_t max_text_edit_size = 1ull * 1024 * 1024;
    int max_transfers = 4;

    // authentication
    std::string auth_token;          // optional Bearer token (empty = disabled)
    std::string auth_token_file;
    int session_ttl_seconds = 12 * 3600;
    int login_rate_limit_per_minute = 5;
    bool cookie_secure = false;
    std::vector<std::string> trusted_proxies;
    bool reset_admin_password = false;

    // general rate limiting
    int rate_limit_per_second = 30;
    int rate_limit_burst = 60;

    // terminals
    int max_terminal_sessions = 8;
    int terminal_idle_timeout_seconds = 1800;
    std::size_t terminal_backlog_bytes = 256ull * 1024;
    std::string bash_path = "/bin/bash";

    // metrics
    int metrics_interval_ms = 1000;
    int metrics_history = 300;
    int disk_refresh_cycles = 10;
    std::vector<std::string> disk_mounts;

    // audit
    bool audit_enabled = true;
    std::string audit_log_path;
    std::uint64_t audit_max_bytes = 1ull * 1024 * 1024;
    int audit_max_files = 2;

    // persistent application data (auth.json, settings.json)
    std::string data_dir;

    // runtime state directory (audit log default location)
    std::string state_dir;

    // meta
    bool help_requested = false;
    bool version_requested = false;
    std::string config_file;
};

// Throws std::invalid_argument with a user-facing message on invalid input.
Config parse_args(int argc, const char* const argv[]);

std::string help_text();

// Computes the default state directory (does not create it).
std::string default_state_dir();

// Creates the state directory with 0700 when missing. Returns false + reason.
bool ensure_state_dir(const std::string& path, std::string& error);

// Computes the persistent data directory: $XDG_DATA_HOME/edgedesk, then
// $HOME/.local/share/edgedesk. Returns an empty string when neither base is
// usable; callers must fail startup rather than fall back to /tmp.
std::string default_data_dir();

} // namespace edgedesk
