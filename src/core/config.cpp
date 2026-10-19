#include "core/config.hpp"

#include "core/types.hpp"
#include "core/util.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace edgedesk {
namespace {

struct OptionSpec {
    const char* name;
    bool takes_value;
    bool repeatable;
};

const OptionSpec kOptions[] = {
    {"host", true, false},
    {"port", true, false},
    {"http-threads", true, false},
    {"web-root", true, false},
    {"file-root", true, false},
    {"follow-symlinks", false, false},
    {"max-request-body", true, false},
    {"max-upload-size", true, false},
    {"max-text-edit-size", true, false},
    {"max-transfers", true, false},
    {"auth-token", true, false},
    {"auth-token-file", true, false},
    {"session-ttl", true, false},
    {"login-rate-limit", true, false},
    {"cookie-secure", false, false},
    {"trusted-proxy", true, true},
    {"rate-limit", true, false},
    {"rate-limit-burst", true, false},
    {"max-terminal-sessions", true, false},
    {"terminal-idle-timeout", true, false},
    {"terminal-backlog", true, false},
    {"bash", true, false},
    {"metrics-interval", true, false},
    {"metrics-history", true, false},
    {"disk-refresh-cycles", true, false},
    {"disk-mount", true, true},
    {"audit-log", true, false},
    {"no-audit", false, false},
    {"audit-max-size", true, false},
    {"audit-max-files", true, false},
    {"audit-retention-days", true, false},
    {"apps-root", true, false},
    {"supervisor-socket", true, false},
    {"no-supervisor", false, false},
    {"state-dir", true, false},
    {"data-dir", true, false},
    {"reset-admin-password", false, false},
    {"config", true, false},
};

const OptionSpec* find_option(std::string_view name) {
    for (const auto& option : kOptions) {
        if (name == option.name) return &option;
    }
    return nullptr;
}

bool read_token_file(const std::string& path, std::string& token, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open auth token file: " + path;
        return false;
    }
    std::string first_line;
    std::getline(file, first_line);
    token = trim(first_line);
    if (token.empty()) {
        error = "auth token file is empty: " + path;
        return false;
    }
    if (token.size() > 4096) {
        error = "auth token file content too large: " + path;
        return false;
    }
    return true;
}

bool valid_network_spec(std::string_view text) {
    const auto slash = text.find('/');
    const std::string address(text.substr(0, slash));
    int prefix = -1;
    if (slash != std::string_view::npos) {
        std::uint64_t parsed = 0;
        if (!parse_uint64(text.substr(slash + 1), parsed) || parsed > 128) return false;
        prefix = static_cast<int>(parsed);
    }
    unsigned char buffer[16] = {};
    if (::inet_pton(AF_INET, address.c_str(), buffer) == 1) {
        return prefix < 0 || prefix <= 32;
    }
    if (::inet_pton(AF_INET6, address.c_str(), buffer) == 1) {
        return prefix < 0 || prefix <= 128;
    }
    return false;
}

bool parse_int(std::string_view text, long long min_value, long long max_value, long long& out,
               const char* name, std::string& error) {
    std::uint64_t parsed = 0;
    if (!parse_uint64(text, parsed) || parsed > static_cast<std::uint64_t>(max_value) ||
        static_cast<long long>(parsed) < min_value) {
        error = std::string("invalid value for --") + name;
        return false;
    }
    out = static_cast<long long>(parsed);
    return true;
}

bool apply_option(Config& config, const std::string& name, const std::string& value, bool has_value,
                  std::string& error) {
    using edgedesk::parse_byte_size;
    long long number = 0;
    std::uint64_t size = 0;

    if (name == "host") {
        if (value.empty()) {
            error = "--host must not be empty";
            return false;
        }
        config.host = value;
    } else if (name == "port") {
        if (!parse_int(value, 1, 65535, number, "port", error)) return false;
        config.port = static_cast<std::uint16_t>(number);
    } else if (name == "http-threads") {
        if (!parse_int(value, 1, 64, number, "http-threads", error)) return false;
        config.http_threads = static_cast<int>(number);
    } else if (name == "web-root") {
        config.web_root = value;
        config.web_root_explicit = true;
    } else if (name == "file-root") {
        if (value.empty() || value.front() != '/') {
            error = "--file-root must be an absolute path";
            return false;
        }
        config.file_root = value;
    } else if (name == "follow-symlinks") {
        config.follow_symlinks = true;
    } else if (name == "max-request-body" || name == "max-upload-size" || name == "max-text-edit-size" ||
               name == "terminal-backlog" || name == "audit-max-size") {
        if (!parse_byte_size(value, size) || size == 0) {
            error = "invalid byte size for --" + name;
            return false;
        }
        if (name == "max-request-body") {
            config.max_request_body = size;
        } else if (name == "max-upload-size") {
            config.max_upload_size = size;
        } else if (name == "max-text-edit-size") {
            config.max_text_edit_size = size;
        } else if (name == "terminal-backlog") {
            config.terminal_backlog_bytes = static_cast<std::size_t>(size);
        } else {
            config.audit_max_bytes = size;
        }
    } else if (name == "max-transfers") {
        if (!parse_int(value, 1, 128, number, "max-transfers", error)) return false;
        config.max_transfers = static_cast<int>(number);
    } else if (name == "auth-token") {
        config.auth_token = trim(value);
    } else if (name == "auth-token-file") {
        config.auth_token_file = value;
    } else if (name == "session-ttl") {
        if (!parse_int(value, 60, 30 * 24 * 3600, number, "session-ttl", error)) return false;
        config.session_ttl_seconds = static_cast<int>(number);
    } else if (name == "login-rate-limit") {
        if (!parse_int(value, 1, 1000, number, "login-rate-limit", error)) return false;
        config.login_rate_limit_per_minute = static_cast<int>(number);
    } else if (name == "cookie-secure") {
        config.cookie_secure = true;
    } else if (name == "trusted-proxy") {
        if (!valid_network_spec(value)) {
            error = "invalid --trusted-proxy address or CIDR: " + value;
            return false;
        }
        config.trusted_proxies.push_back(value);
    } else if (name == "rate-limit") {
        if (!parse_int(value, 1, 100000, number, "rate-limit", error)) return false;
        config.rate_limit_per_second = static_cast<int>(number);
    } else if (name == "rate-limit-burst") {
        if (!parse_int(value, 1, 1000000, number, "rate-limit-burst", error)) return false;
        config.rate_limit_burst = static_cast<int>(number);
    } else if (name == "max-terminal-sessions") {
        if (!parse_int(value, 1, 1024, number, "max-terminal-sessions", error)) return false;
        config.max_terminal_sessions = static_cast<int>(number);
    } else if (name == "terminal-idle-timeout") {
        if (!parse_int(value, 0, 30 * 24 * 3600, number, "terminal-idle-timeout", error)) return false;
        config.terminal_idle_timeout_seconds = static_cast<int>(number);
    } else if (name == "bash") {
        if (value.empty() || value.front() != '/') {
            error = "--bash must be an absolute path";
            return false;
        }
        config.bash_path = value;
    } else if (name == "metrics-interval") {
        if (!parse_int(value, 200, 60000, number, "metrics-interval", error)) return false;
        config.metrics_interval_ms = static_cast<int>(number);
    } else if (name == "metrics-history") {
        if (!parse_int(value, 10, 36000, number, "metrics-history", error)) return false;
        config.metrics_history = static_cast<int>(number);
    } else if (name == "disk-refresh-cycles") {
        if (!parse_int(value, 1, 3600, number, "disk-refresh-cycles", error)) return false;
        config.disk_refresh_cycles = static_cast<int>(number);
    } else if (name == "disk-mount") {
        if (value.empty() || value.front() != '/') {
            error = "--disk-mount must be an absolute mount path";
            return false;
        }
        config.disk_mounts.push_back(value);
    } else if (name == "audit-log") {
        config.audit_log_path = value;
    } else if (name == "no-audit") {
        config.audit_enabled = false;
    } else if (name == "audit-max-files") {
        if (!parse_int(value, 1, 32, number, "audit-max-files", error)) return false;
        config.audit_max_files = static_cast<int>(number);
    } else if (name == "audit-retention-days") {
        if (!parse_int(value, 1, 3650, number, "audit-retention-days", error)) return false;
        config.audit_retention_days = static_cast<int>(number);
    } else if (name == "apps-root") {
        if (value.empty() || value.front() != '/') {
            error = "--apps-root must be an absolute path";
            return false;
        }
        config.apps_root = value;
    } else if (name == "supervisor-socket") {
        if (value.empty() || value.front() != '/' ||
            value.size() >= sizeof(sockaddr_un::sun_path)) {
            error = "--supervisor-socket must be an absolute Unix socket path shorter than 108 bytes";
            return false;
        }
        config.supervisor_socket = value;
    } else if (name == "no-supervisor") {
        config.supervisor_enabled = false;
    } else if (name == "state-dir") {
        config.state_dir = value;
    } else if (name == "data-dir") {
        config.data_dir = value;
    } else if (name == "reset-admin-password") {
        config.reset_admin_password = true;
    } else if (name == "config") {
        config.config_file = value;
    } else {
        (void)has_value;
        error = "unknown option: --" + name;
        return false;
    }
    return true;
}

bool split_config_line(const std::string& line, std::string& key, std::string& value) {
    const auto comment = line.find('#');
    const auto trimmed = trim(line.substr(0, comment == std::string::npos ? line.size() : comment));
    if (trimmed.empty()) return false;
    const auto separator = trimmed.find('=');
    if (separator == std::string::npos) {
        key = trimmed;
        value.clear();
        return true;
    }
    key = trim(trimmed.substr(0, separator));
    value = trim(trimmed.substr(separator + 1));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return true;
}

bool load_config_file(Config& config, const std::string& path, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open config file: " + path;
        return false;
    }
    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        std::string key;
        std::string value;
        if (!split_config_line(line, key, value)) continue;
        if (key == "config" || key == "help" || key == "version") {
            error = "option not allowed in config file: " + key;
            return false;
        }
        const auto* spec = find_option(key);
        if (spec == nullptr) {
            error = "unknown option in config file line " + std::to_string(line_number) + ": " + key;
            return false;
        }
        if (spec->takes_value && value.empty()) {
            error = "missing value in config file line " + std::to_string(line_number) + ": " + key;
            return false;
        }
        if (!apply_option(config, key, value, spec->takes_value, error)) {
            error += " (config file line " + std::to_string(line_number) + ")";
            return false;
        }
    }
    return true;
}

bool file_exists(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

} // namespace

std::string default_state_dir() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg != nullptr && xdg[0] == '/') {
        return std::string(xdg) + "/edgedesk";
    }
    return "/tmp/edgedesk-" + std::to_string(static_cast<unsigned long>(::getuid()));
}

std::string default_data_dir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg != nullptr && xdg[0] == '/') {
        return std::string(xdg) + "/edgedesk";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] == '/') {
        return std::string(home) + "/.local/share/edgedesk";
    }
    return {};
}

bool ensure_state_dir(const std::string& path, std::string& error) {
    if (path.empty() || path.front() != '/') {
        error = "state directory must be an absolute path";
        return false;
    }
    struct stat info {};
    if (::stat(path.c_str(), &info) == 0) {
        if (!S_ISDIR(info.st_mode)) {
            error = "state path exists but is not a directory: " + path;
            return false;
        }
        return true;
    }
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        error = "cannot create state directory " + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

Config parse_args(int argc, const char* const argv[]) {
    Config config;
#ifdef EDGEDESK_DISK_DEFAULT_DIR
    config.web_root = EDGEDESK_DISK_DEFAULT_DIR;
#endif

    // Pass 1: locate --config so the file loads before other CLI options.
    std::string config_path;
    const char* env_config = std::getenv("EDGEDESK_CONFIG");
    if (env_config != nullptr && env_config[0] != '\0') config_path = env_config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg.rfind("--config=", 0) == 0) {
            config_path = arg.substr(9);
        }
    }
    if (!config_path.empty()) {
        std::string error;
        if (!load_config_file(config, config_path, error)) {
            throw std::invalid_argument(error);
        }
        config.config_file = config_path;
    }

    // Environment: the secret token, runtime directory and persistent data
    // directory. Precedence stays defaults < config file < environment < CLI.
    const char* env_token = std::getenv("EDGEDESK_AUTH_TOKEN");
    if (env_token != nullptr && env_token[0] != '\0') {
        config.auth_token = trim(env_token);
    }
    const char* env_state = std::getenv("EDGEDESK_STATE_DIR");
    if (env_state != nullptr && env_state[0] == '/') {
        config.state_dir = env_state;
    }
    const char* env_data = std::getenv("EDGEDESK_DATA_DIR");
    if (env_data != nullptr && env_data[0] == '/') {
        config.data_dir = env_data;
    }

    // Pass 2: command line overrides.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            config.help_requested = true;
            continue;
        }
        if (arg == "--version" || arg == "-v") {
            config.version_requested = true;
            continue;
        }
        if (arg.size() < 3 || arg.rfind("--", 0) != 0) {
            throw std::invalid_argument("Unknown argument: " + arg);
        }
        std::string name = arg.substr(2);
        std::string value;
        bool has_value = false;
        const auto equals = name.find('=');
        if (equals != std::string::npos) {
            value = name.substr(equals + 1);
            name = name.substr(0, equals);
            has_value = true;
        }
        const auto* spec = find_option(name);
        if (spec == nullptr) {
            throw std::invalid_argument("Unknown argument: " + arg);
        }
        if (spec->takes_value && !has_value) {
            if (i + 1 >= argc) {
                throw std::invalid_argument("Missing value for --" + name);
            }
            value = argv[++i];
            has_value = true;
        }
        if (!spec->takes_value && has_value) {
            throw std::invalid_argument("Option --" + name + " does not take a value");
        }
        std::string error;
        if (!apply_option(config, name, value, has_value, error)) {
            throw std::invalid_argument(error);
        }
    }

    if (config.help_requested || config.version_requested) return config;

    // Resolve the optional API token: token file wins over a literal CLI/env
    // token because it keeps the secret out of process listings and shell
    // history. An empty token disables Bearer authentication; it never disables
    // the password login flow.
    if (!config.auth_token_file.empty()) {
        std::string token;
        std::string error;
        if (!read_token_file(config.auth_token_file, token, error)) {
            throw std::invalid_argument(error);
        }
        config.auth_token = token;
    }

    if (config.data_dir.empty()) {
        config.data_dir = default_data_dir();
    }
    if (config.data_dir.empty() || config.data_dir.front() != '/') {
        throw std::invalid_argument(
            "cannot determine a persistent data directory; pass --data-dir or set EDGEDESK_DATA_DIR");
    }

    if (config.state_dir.empty()) {
        config.state_dir = default_state_dir();
    }
    if (config.audit_log_path.empty()) {
        config.audit_log_path = config.state_dir + "/audit.log";
    }
    if (config.audit_enabled && config.audit_max_bytes < 4096) {
        throw std::invalid_argument("--audit-max-size must be at least 4K");
    }

    if (config.max_text_edit_size > config.max_request_body) {
        throw std::invalid_argument("--max-text-edit-size must not exceed --max-request-body");
    }

    if (!file_exists(config.file_root)) {
        throw std::invalid_argument("--file-root does not exist: " + config.file_root);
    }

    if (config.web_root_explicit && !config.web_root.empty() && !file_exists(config.web_root)) {
        throw std::invalid_argument("--web-root does not exist: " + config.web_root);
    }

    return config;
}

std::string help_text() {
    return
        "EdgeDesk 1.0 - embedded Linux web operations panel\n"
        "\n"
        "Usage: edgedesk [options]\n"
        "\n"
        "Network:\n"
        "  --host ADDR                 Listen address (default 127.0.0.1)\n"
        "  --port PORT                 Listen port (default 8080)\n"
        "  --http-threads N            HTTP event-loop threads (default 1)\n"
        "\n"
        "Static assets:\n"
        "  --web-root PATH             Serve frontend from disk (disk build only)\n"
        "\n"
        "Files:\n"
        "  --file-root PATH            Sandbox root for the file API (default /)\n"
        "  --follow-symlinks           Allow symlinks that stay inside the root (default off)\n"
        "  --max-upload-size SIZE      Streaming upload limit (default 256M)\n"
        "  --max-text-edit-size SIZE   Text editor body limit (default 1M)\n"
        "  --max-request-body SIZE     JSON API body limit (default 1M)\n"
        "  --max-transfers N           Concurrent uploads/downloads (default 4)\n"
        "\n"
        "Authentication:\n"
        "  --auth-token TEXT           Optional Bearer API token (visible in ps)\n"
        "  --auth-token-file PATH      Read the optional Bearer token from a file\n"
        "  (env EDGEDESK_AUTH_TOKEN)   Alternative to the options above\n"
        "  --session-ttl SECONDS       Cookie session lifetime (default 43200)\n"
        "  --login-rate-limit N        Login attempts per minute per IP (default 5)\n"
        "  --cookie-secure             Add Secure to session cookies (HTTPS deployments)\n"
        "  --trusted-proxy IP/CIDR     Trust X-Forwarded-For from this proxy (repeatable)\n"
        "  --reset-admin-password      Reset local admin auth and exit (service stopped)\n"
        "  --rate-limit N              Requests per second per IP (default 30)\n"
        "  --rate-limit-burst N        Burst budget per IP (default 60)\n"
        "\n"
        "Terminal:\n"
        "  --max-terminal-sessions N   Concurrent PTY sessions (default 8)\n"
        "  --terminal-idle-timeout S   Retain idle detached sessions (default 1800, 0=off)\n"
        "  --terminal-backlog SIZE     Replay buffer per session (default 256K)\n"
        "  --bash PATH                 Login shell (default /bin/bash; never falls back to sh)\n"
        "\n"
        "Metrics:\n"
        "  --metrics-interval MS       Sample interval (default 1000)\n"
        "  --metrics-history N         In-memory samples kept (default 300)\n"
        "  --disk-refresh-cycles N     Disk capacity refresh every N samples (default 10)\n"
        "  --disk-mount PATH           Restrict disk metrics to this mount (repeatable)\n"
        "\n"
        "Audit / runtime:\n"
        "  --data-dir PATH             Persistent data directory (auth/settings; default XDG data)\n"
        "  --state-dir PATH            Writable runtime state directory\n"
        "  --audit-log PATH            Audit log path (default <state-dir>/audit.log)\n"
        "  --audit-max-size SIZE       Rotation size (default 1M)\n"
        "  --audit-max-files N         Rotated files kept (default 2)\n"
        "  --audit-retention-days N    Audit retention in days (default 90)\n"
        "  --no-audit                  Disable audit logging\n"
        "\n"
        "Extensions:\n"
        "  --apps-root PATH            Application packages/data root\n"
        "                              (default /var/lib/edgedesk/apps)\n"
        "\n"
        "Process supervisor:\n"
        "  --supervisor-socket PATH    Supervisor Unix socket\n"
        "                              (default /run/edgedesk-supervisor.sock)\n"
        "  --no-supervisor             Disable process management integration\n"
        "\n"
        "Other:\n"
        "  --config PATH               Load options from a key=value file\n"
        "  --help                      Show this help\n"
        "  --version                   Show version\n";
}

} // namespace edgedesk
