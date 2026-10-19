#include "supervisor/supervisor_types.hpp"

#include "core/types.hpp"
#include "core/utf8.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <limits>
#include <pwd.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace edgedesk::supervisor {
namespace {

bool no_nul(std::string_view text) { return text.find('\0') == std::string_view::npos; }

bool valid_env_key(std::string_view key) {
    if (key.empty() || key.size() > 128) return false;
    const unsigned char first = static_cast<unsigned char>(key[0]);
    if (!(first == '_' || (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z'))) return false;
    for (const char ch : key) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (!(value == '_' || (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9'))) {
            return false;
        }
    }
    return true;
}

bool parse_numeric_id(std::string_view text, unsigned long& out) {
    if (text.empty()) return false;
    unsigned long value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const unsigned long digit = static_cast<unsigned long>(ch - '0');
        if (value > (std::numeric_limits<unsigned long>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

Error read_bool(const nlohmann::json& body, const char* key, bool& value) {
    if (!body.contains(key)) return Error::none();
    if (!body[key].is_boolean()) {
        return Error::make(ErrorCode::invalid_argument,
                           std::string("field '") + key + "' must be a boolean");
    }
    value = body[key].get<bool>();
    return Error::none();
}

Error read_string(const nlohmann::json& body, const char* key, std::string& value) {
    if (!body.contains(key)) return Error::none();
    if (!body[key].is_string()) {
        return Error::make(ErrorCode::invalid_argument,
                           std::string("field '") + key + "' must be a string");
    }
    value = body[key].get<std::string>();
    return Error::none();
}

Error read_int(const nlohmann::json& body, const char* key, int& value) {
    if (!body.contains(key)) return Error::none();
    if (!body[key].is_number_integer()) {
        return Error::make(ErrorCode::invalid_argument,
                           std::string("field '") + key + "' must be an integer");
    }
    const auto parsed = body[key].get<long long>();
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        return Error::make(ErrorCode::invalid_argument,
                           std::string("field '") + key + "' is out of range");
    }
    value = static_cast<int>(parsed);
    return Error::none();
}

Error read_u64(const nlohmann::json& body, const char* key, std::uint64_t& value) {
    if (!body.contains(key)) return Error::none();
    if (body[key].is_number_unsigned()) {
        value = body[key].get<std::uint64_t>();
        return Error::none();
    }
    if (!body[key].is_number_integer()) {
        return Error::make(ErrorCode::invalid_argument,
                           std::string("field '") + key + "' must be an integer");
    }
    const auto parsed = body[key].get<std::int64_t>();
    if (parsed < 0) {
        return Error::make(ErrorCode::invalid_argument,
                           std::string("field '") + key + "' must not be negative");
    }
    value = static_cast<std::uint64_t>(parsed);
    return Error::none();
}

bool stat_regular_executable(const std::string& path, std::string& error) {
    // O_PATH pins the inode for fexecve() and never blocks on a FIFO/device.
    const int fd = ::open(path.c_str(), O_PATH | O_CLOEXEC);
    if (fd < 0) {
        error = "executable is not accessible: " + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat info {};
    const bool ok = ::fstat(fd, &info) == 0;
    const int saved = errno;
    ::close(fd);
    if (!ok) {
        error = "cannot stat executable: " + path + ": " + std::strerror(saved);
        return false;
    }
    if (!S_ISREG(info.st_mode)) {
        error = "executable must be a regular file: " + path;
        return false;
    }
    if ((info.st_mode & 0111) == 0) {
        error = "executable is not executable: " + path;
        return false;
    }
    return true;
}

bool stat_directory(const std::string& path, std::string& error) {
    const int fd = ::open(path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        error = "working directory is not accessible: " + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat info {};
    const bool ok = ::fstat(fd, &info) == 0;
    const int saved = errno;
    ::close(fd);
    if (!ok || !S_ISDIR(info.st_mode)) {
        error = "working directory is not a directory: " + path +
                (ok ? std::string() : std::string(": ") + std::strerror(saved));
        return false;
    }
    return true;
}

} // namespace

const char* restart_policy_name(RestartPolicy policy) {
    switch (policy) {
        case RestartPolicy::always: return "always";
        case RestartPolicy::on_failure: return "on-failure";
        case RestartPolicy::never: return "never";
    }
    return "on-failure";
}

bool parse_restart_policy(std::string_view text, RestartPolicy& out) {
    if (text == "always") {
        out = RestartPolicy::always;
        return true;
    }
    if (text == "on-failure") {
        out = RestartPolicy::on_failure;
        return true;
    }
    if (text == "never") {
        out = RestartPolicy::never;
        return true;
    }
    return false;
}

const char* service_state_name(ServiceState state) {
    switch (state) {
        case ServiceState::stopped: return "STOPPED";
        case ServiceState::starting: return "STARTING";
        case ServiceState::running: return "RUNNING";
        case ServiceState::stopping: return "STOPPING";
        case ServiceState::backoff: return "BACKOFF";
        case ServiceState::failed: return "FAILED";
        case ServiceState::unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool parse_service_state(std::string_view text, ServiceState& out) {
    if (text == "STOPPED") {
        out = ServiceState::stopped;
        return true;
    }
    if (text == "STARTING") {
        out = ServiceState::starting;
        return true;
    }
    if (text == "RUNNING") {
        out = ServiceState::running;
        return true;
    }
    if (text == "STOPPING") {
        out = ServiceState::stopping;
        return true;
    }
    if (text == "BACKOFF") {
        out = ServiceState::backoff;
        return true;
    }
    if (text == "FAILED") {
        out = ServiceState::failed;
        return true;
    }
    if (text == "UNKNOWN") {
        out = ServiceState::unknown;
        return true;
    }
    return false;
}

Error service_config_from_json(const nlohmann::json& body, ServiceConfig& config,
                               bool update_existing) {
    if (!body.is_object()) {
        return Error::make(ErrorCode::invalid_argument, "service body must be a JSON object");
    }
    static const std::set<std::string> kAllowed = {
        "name",          "executable",          "args",           "cwd",
        "env",           "run_user",            "run_group",      "allow_root",
        "autostart",     "restart_policy",      "max_restarts",   "restart_window_sec",
        "restart_delay_ms", "max_restart_delay_ms", "stable_after_sec", "stop_timeout_ms",
        "log_max_bytes",
    };
    for (const auto& item : body.items()) {
        if (kAllowed.find(item.key()) == kAllowed.end()) {
            return Error::make(ErrorCode::invalid_argument, "unknown service field: " + item.key());
        }
    }

    Error error = read_string(body, "name", config.name);
    if (!error.ok()) return error;
    error = read_string(body, "executable", config.executable);
    if (!error.ok()) return error;
    error = read_string(body, "cwd", config.cwd);
    if (!error.ok()) return error;
    error = read_string(body, "run_user", config.run_user);
    if (!error.ok()) return error;
    error = read_string(body, "run_group", config.run_group);
    if (!error.ok()) return error;
    error = read_bool(body, "allow_root", config.allow_root);
    if (!error.ok()) return error;
    error = read_bool(body, "autostart", config.autostart);
    if (!error.ok()) return error;
    error = read_int(body, "max_restarts", config.max_restarts);
    if (!error.ok()) return error;
    error = read_int(body, "restart_window_sec", config.restart_window_sec);
    if (!error.ok()) return error;
    error = read_int(body, "restart_delay_ms", config.restart_delay_ms);
    if (!error.ok()) return error;
    error = read_int(body, "max_restart_delay_ms", config.max_restart_delay_ms);
    if (!error.ok()) return error;
    error = read_int(body, "stable_after_sec", config.stable_after_sec);
    if (!error.ok()) return error;
    error = read_int(body, "stop_timeout_ms", config.stop_timeout_ms);
    if (!error.ok()) return error;
    error = read_u64(body, "log_max_bytes", config.log_max_bytes);
    if (!error.ok()) return error;

    if (body.contains("restart_policy")) {
        if (!body["restart_policy"].is_string() ||
            !parse_restart_policy(body["restart_policy"].get<std::string>(), config.restart_policy)) {
            return Error::make(ErrorCode::invalid_argument,
                               "field 'restart_policy' must be always, on-failure or never");
        }
    }
    if (body.contains("args")) {
        if (!body["args"].is_array()) {
            return Error::make(ErrorCode::invalid_argument, "field 'args' must be an array of strings");
        }
        config.args.clear();
        for (const auto& item : body["args"]) {
            if (!item.is_string()) {
                return Error::make(ErrorCode::invalid_argument,
                                   "field 'args' must contain only strings");
            }
            config.args.push_back(item.get<std::string>());
        }
    }
    if (body.contains("env")) {
        if (!body["env"].is_object()) {
            return Error::make(ErrorCode::invalid_argument, "field 'env' must be an object");
        }
        config.env.clear();
        for (const auto& item : body["env"].items()) {
            if (!item.value().is_string()) {
                return Error::make(ErrorCode::invalid_argument,
                                   "field 'env' values must be strings");
            }
            config.env[item.key()] = item.value().get<std::string>();
        }
    }
    // On create these are mandatory rather than defaulted, so an accidentally
    // incomplete form cannot silently run as the Supervisor's own identity.
    if (!update_existing &&
        (config.name.empty() || config.executable.empty() || config.run_user.empty() ||
         config.run_group.empty())) {
        return Error::make(ErrorCode::invalid_argument,
                           "name, executable, run_user and run_group are required");
    }
    return Error::none();
}

Error validate_service_config(const ServiceConfig& config) {
    if (config.name.empty()) {
        return Error::make(ErrorCode::invalid_argument, "service name must not be empty");
    }
    if (config.name.size() > 128) {
        return Error::make(ErrorCode::invalid_argument, "service name exceeds 128 bytes");
    }
    if (!is_valid_utf8(config.name) || !no_nul(config.name)) {
        return Error::make(ErrorCode::invalid_argument, "service name must be valid UTF-8 without NUL");
    }
    for (const char ch : config.name) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (value < 0x20 || value == 0x7f) {
            return Error::make(ErrorCode::invalid_argument,
                               "service name must not contain control characters");
        }
    }
    if (config.executable.empty() || config.executable.front() != '/' ||
        config.executable.size() > 4096 || !no_nul(config.executable)) {
        return Error::make(ErrorCode::invalid_argument,
                           "field 'executable' must be an absolute path without NUL");
    }
    if (config.cwd.empty() || config.cwd.front() != '/' || config.cwd.size() > 4096 ||
        !no_nul(config.cwd)) {
        return Error::make(ErrorCode::invalid_argument,
                           "field 'cwd' must be an absolute path without NUL");
    }
    if (config.args.size() > 128) {
        return Error::make(ErrorCode::invalid_argument, "field 'args' exceeds 128 entries");
    }
    std::size_t argv_bytes = 0;
    for (const auto& arg : config.args) {
        if (!no_nul(arg)) {
            return Error::make(ErrorCode::invalid_argument, "args must not contain NUL");
        }
        if (arg.size() > 8192) {
            return Error::make(ErrorCode::invalid_argument, "a single argument exceeds 8192 bytes");
        }
        argv_bytes += arg.size();
    }
    if (argv_bytes > 128 * 1024) {
        return Error::make(ErrorCode::invalid_argument, "combined arguments exceed 128 KiB");
    }
    if (config.env.size() > 128) {
        return Error::make(ErrorCode::invalid_argument, "field 'env' exceeds 128 entries");
    }
    std::size_t env_bytes = 0;
    for (const auto& item : config.env) {
        if (!valid_env_key(item.first)) {
            return Error::make(ErrorCode::invalid_argument,
                               "invalid environment variable name: " + item.first);
        }
        if (!no_nul(item.second) || item.second.size() > 16384) {
            return Error::make(ErrorCode::invalid_argument,
                               "environment variable " + item.first + " is invalid or too long");
        }
        env_bytes += item.first.size() + item.second.size() + 1;
    }
    if (env_bytes > 128 * 1024) {
        return Error::make(ErrorCode::invalid_argument, "combined environment exceeds 128 KiB");
    }
    if (config.run_user.empty() || config.run_user.size() > 128 || !no_nul(config.run_user) ||
        config.run_group.empty() || config.run_group.size() > 128 || !no_nul(config.run_group)) {
        return Error::make(ErrorCode::invalid_argument,
                           "run_user and run_group are required and must be valid names or IDs");
    }
    if (config.max_restarts < 0 || config.max_restarts > 1000) {
        return Error::make(ErrorCode::invalid_argument, "max_restarts must be between 0 and 1000");
    }
    if (config.restart_window_sec < 1 || config.restart_window_sec > 30 * 24 * 3600) {
        return Error::make(ErrorCode::invalid_argument,
                           "restart_window_sec must be between 1 and 2592000");
    }
    if (config.restart_delay_ms < 10 || config.restart_delay_ms > 3600 * 1000) {
        return Error::make(ErrorCode::invalid_argument,
                           "restart_delay_ms must be between 10 and 3600000");
    }
    if (config.max_restart_delay_ms < config.restart_delay_ms ||
        config.max_restart_delay_ms > 3600 * 1000) {
        return Error::make(ErrorCode::invalid_argument,
                           "max_restart_delay_ms must be between restart_delay_ms and 3600000");
    }
    if (config.stable_after_sec < 1 || config.stable_after_sec > 30 * 24 * 3600) {
        return Error::make(ErrorCode::invalid_argument,
                           "stable_after_sec must be between 1 and 2592000");
    }
    if (config.stop_timeout_ms < 100 || config.stop_timeout_ms > 10 * 60 * 1000) {
        return Error::make(ErrorCode::invalid_argument,
                           "stop_timeout_ms must be between 100 and 600000");
    }
    if (config.log_max_bytes < 4096 || config.log_max_bytes > 1024ull * 1024 * 1024) {
        return Error::make(ErrorCode::invalid_argument,
                           "log_max_bytes must be between 4096 and 1073741824");
    }

    ResolvedIdentity identity;
    const Error identity_error = resolve_identity(config.run_user, config.run_group, identity);
    if (!identity_error.ok()) return identity_error;
    if (identity.is_root && !config.allow_root) {
        return Error::make(ErrorCode::permission_denied,
                           "running as root requires explicit confirmation (allow_root=true)");
    }

    std::string path_error;
    if (!stat_regular_executable(config.executable, path_error)) {
        return Error::make(ErrorCode::invalid_argument, path_error);
    }
    if (!stat_directory(config.cwd, path_error)) {
        return Error::make(ErrorCode::invalid_argument, path_error);
    }
    return Error::none();
}

Error resolve_identity(const std::string& user, const std::string& group, ResolvedIdentity& out) {
    out = {};
    unsigned long numeric = 0;
    if (parse_numeric_id(user, numeric)) {
        if (numeric > std::numeric_limits<uid_t>::max()) {
            return Error::make(ErrorCode::invalid_argument, "run_user UID is out of range");
        }
        out.uid = static_cast<uid_t>(numeric);
        struct passwd* entry = ::getpwuid(out.uid);
        if (entry != nullptr) {
            out.user_name = entry->pw_name != nullptr ? entry->pw_name : "";
            out.home = entry->pw_dir != nullptr ? entry->pw_dir : "/";
        }
    } else {
        struct passwd* entry = ::getpwnam(user.c_str());
        if (entry == nullptr) {
            return Error::make(ErrorCode::invalid_argument, "unknown run_user: " + user);
        }
        out.uid = entry->pw_uid;
        out.user_name = entry->pw_name != nullptr ? entry->pw_name : user;
        out.home = entry->pw_dir != nullptr ? entry->pw_dir : "/";
    }

    if (parse_numeric_id(group, numeric)) {
        if (numeric > std::numeric_limits<gid_t>::max()) {
            return Error::make(ErrorCode::invalid_argument, "run_group GID is out of range");
        }
        out.gid = static_cast<gid_t>(numeric);
        struct group* entry = ::getgrgid(out.gid);
        out.group_name = entry != nullptr && entry->gr_name != nullptr ? entry->gr_name : "";
    } else {
        struct group* entry = ::getgrnam(group.c_str());
        if (entry == nullptr) {
            return Error::make(ErrorCode::invalid_argument, "unknown run_group: " + group);
        }
        out.gid = entry->gr_gid;
        out.group_name = entry->gr_name != nullptr ? entry->gr_name : group;
    }
    if (out.home.empty()) out.home = "/";
    out.is_root = out.uid == 0;

    if (!out.user_name.empty()) {
        int count = 16;
        std::vector<gid_t> groups(static_cast<std::size_t>(count));
        if (::getgrouplist(out.user_name.c_str(), out.gid, groups.data(), &count) < 0) {
            if (count > 0) {
                groups.resize(static_cast<std::size_t>(count));
                if (::getgrouplist(out.user_name.c_str(), out.gid, groups.data(), &count) < 0) {
                    groups.clear();
                }
            }
        }
        if (!groups.empty()) {
            groups.resize(static_cast<std::size_t>(count));
            out.supplementary_groups = std::move(groups);
        }
    }
    if (out.supplementary_groups.empty()) out.supplementary_groups.push_back(out.gid);
    bool has_primary = false;
    for (const auto gid : out.supplementary_groups) {
        if (gid == out.gid) has_primary = true;
    }
    if (!has_primary) out.supplementary_groups.push_back(out.gid);
    return Error::none();
}

nlohmann::json service_config_json(const ServiceConfig& config) {
    nlohmann::json body;
    body["id"] = config.id;
    body["name"] = config.name;
    body["executable"] = config.executable;
    body["args"] = config.args;
    body["cwd"] = config.cwd;
    body["env"] = config.env;
    body["run_user"] = config.run_user;
    body["run_group"] = config.run_group;
    body["allow_root"] = config.allow_root;
    body["autostart"] = config.autostart;
    body["restart_policy"] = restart_policy_name(config.restart_policy);
    body["max_restarts"] = config.max_restarts;
    body["restart_window_sec"] = config.restart_window_sec;
    body["restart_delay_ms"] = config.restart_delay_ms;
    body["max_restart_delay_ms"] = config.max_restart_delay_ms;
    body["stable_after_sec"] = config.stable_after_sec;
    body["stop_timeout_ms"] = config.stop_timeout_ms;
    body["log_max_bytes"] = config.log_max_bytes;
    body["created_at"] = config.created_at;
    body["updated_at"] = config.updated_at;
    return body;
}

nlohmann::json service_state_json(const ServiceRecord& record) {
    const auto& runtime = record.runtime;
    nlohmann::json body;
    body["state"] = service_state_name(runtime.state);
    body["desired_state"] = runtime.desired_state;
    body["pid"] = runtime.pid > 0 ? nlohmann::json(runtime.pid) : nlohmann::json(nullptr);
    body["pgid"] = runtime.pgid > 0 ? nlohmann::json(runtime.pgid) : nlohmann::json(nullptr);
    body["boot_id"] = runtime.boot_id;
    body["started_at"] = runtime.started_at_ms > 0 ? nlohmann::json(runtime.started_at_ms) : nlohmann::json(nullptr);
    body["last_exit_code"] = runtime.last_exit_code >= 0 ? nlohmann::json(runtime.last_exit_code)
                                                         : nlohmann::json(nullptr);
    body["last_signal"] = runtime.last_signal > 0 ? nlohmann::json(runtime.last_signal)
                                                  : nlohmann::json(nullptr);
    body["last_exit_at"] = runtime.last_exit_at > 0 ? nlohmann::json(runtime.last_exit_at)
                                                    : nlohmann::json(nullptr);
    body["last_error"] = runtime.last_error;
    body["restart_count"] = runtime.restart_count;
    body["failed_locked"] = runtime.failed_locked;
    return body;
}

nlohmann::json service_record_json(const ServiceRecord& record) {
    nlohmann::json body = service_config_json(record.config);
    const nlohmann::json state = service_state_json(record);
    for (auto& item : state.items()) body[item.key()] = item.value();
    return body;
}

} // namespace edgedesk::supervisor
