#include "supervisor/supervisor_engine.hpp"

#include "core/util.hpp"
#include "supervisor/process_control.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifndef EDGEDESK_VERSION
#define EDGEDESK_VERSION "dev"
#endif

namespace edgedesk::supervisor {
namespace {

constexpr std::size_t kLogChunk = 16 * 1024;
constexpr std::size_t kLogDrainBudget = 256 * 1024;
constexpr std::int64_t kStopKillGraceMs = 5000;
constexpr std::int64_t kMaxPollSleepMs = 1000;
constexpr std::size_t kMaxRetainedOperations = 256;

RpcResult failure(ErrorCode code, std::string slug, std::string message) {
    RpcResult result;
    result.ok = false;
    result.error = Error::make(code, std::move(message));
    result.slug = std::move(slug);
    return result;
}

RpcResult success(nlohmann::json body) {
    RpcResult result;
    result.ok = true;
    result.result = std::move(body);
    return result;
}

std::string config_error_slug(const Error& error) {
    if (error.code == ErrorCode::permission_denied) {
        if (error.message.find("root") != std::string::npos) return "root_confirmation_required";
        return "permission_denied";
    }
    if (error.code == ErrorCode::invalid_argument) return "invalid_service_config";
    return "supervisor_unavailable";
}

bool read_id(const nlohmann::json& params, std::string& id, RpcResult& error) {
    if (!params.is_object() || !params.contains("id") || !params["id"].is_string()) {
        error = failure(ErrorCode::invalid_argument, "invalid_params", "field 'id' is required");
        return false;
    }
    id = params["id"].get<std::string>();
    if (id.empty() || id.size() > 64) {
        error = failure(ErrorCode::invalid_argument, "invalid_params", "invalid service id");
        return false;
    }
    return true;
}

bool read_optional_u64(const nlohmann::json& params, const char* key, bool& present,
                       std::uint64_t& value, RpcResult& error) {
    present = false;
    if (!params.contains(key) || params[key].is_null()) return true;
    if (params[key].is_number_unsigned()) {
        value = params[key].get<std::uint64_t>();
    } else if (params[key].is_number_integer()) {
        const auto parsed = params[key].get<std::int64_t>();
        if (parsed < 0) {
            error = failure(ErrorCode::invalid_argument, "invalid_params",
                            std::string("field '") + key + "' must not be negative");
            return false;
        }
        value = static_cast<std::uint64_t>(parsed);
    } else {
        error = failure(ErrorCode::invalid_argument, "invalid_params",
                        std::string("field '") + key + "' must be an integer");
        return false;
    }
    present = true;
    return true;
}

bool read_optional_int(const nlohmann::json& params, const char* key, int64_t minimum,
                       int64_t maximum, std::int64_t default_value, std::int64_t& value,
                       RpcResult& error) {
    value = default_value;
    if (!params.contains(key) || params[key].is_null()) return true;
    std::int64_t parsed = 0;
    if (params[key].is_number_unsigned()) {
        const auto raw = params[key].get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            error = failure(ErrorCode::invalid_argument, "invalid_params",
                            std::string("field '") + key + "' is out of range");
            return false;
        }
        parsed = static_cast<std::int64_t>(raw);
    } else if (params[key].is_number_integer()) {
        parsed = params[key].get<std::int64_t>();
    } else {
        error = failure(ErrorCode::invalid_argument, "invalid_params",
                        std::string("field '") + key + "' must be an integer");
        return false;
    }
    if (parsed < minimum || parsed > maximum) {
        error = failure(ErrorCode::invalid_argument, "invalid_params",
                        std::string("field '") + key + "' is out of range");
        return false;
    }
    value = parsed;
    return true;
}

bool restart_policy_wants_restart(RestartPolicy policy, bool failed) {
    switch (policy) {
        case RestartPolicy::always: return true;
        case RestartPolicy::on_failure: return failed;
        case RestartPolicy::never: return false;
    }
    return false;
}

std::uint64_t jitter_for(std::string_view id, std::uint64_t sequence, std::int64_t delay_ms) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const char ch : id) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    hash ^= sequence * 0x9e3779b97f4a7c15ull;
    const std::int64_t span = std::max<std::int64_t>(1, std::min<std::int64_t>(1000, delay_ms / 5));
    return hash % static_cast<std::uint64_t>(span + 1);
}

std::string default_exec_staging_directory() {
    if (::geteuid() == 0) return "/run/edgedesk-supervisor-exec";
    return "/tmp/edgedesk-supervisor-exec-" +
           std::to_string(static_cast<unsigned>(::geteuid()));
}

// The staging base only holds executable symlinks. It must be owned by the
// supervisor and not writable by anyone else; root additionally needs x bits so
// a service running as another user can traverse to its own per-spawn symlink.
Error prepare_exec_staging_directory(const std::string& path) {
    if (path.empty() || path.front() != '/') {
        return Error::make(ErrorCode::invalid_argument,
                           "executable staging directory must be an absolute path");
    }
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        if (errno != ENOENT) {
            return Error::make(ErrorCode::io_error,
                               "cannot inspect executable staging directory " + path + ": " +
                                   std::strerror(errno), errno);
        }
        const mode_t mode = ::geteuid() == 0 ? 0711 : 0700;
        if (::mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
            return Error::make(ErrorCode::io_error,
                               "cannot create executable staging directory " + path + ": " +
                                   std::strerror(errno), errno);
        }
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return Error::make(ErrorCode::permission_denied,
                           "cannot open executable staging directory " + path + ": " +
                               std::strerror(errno), errno);
    }
    Error error = Error::none();
    if (::fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode)) {
        error = Error::make(ErrorCode::permission_denied,
                            "executable staging path is not a directory: " + path);
    } else if (info.st_uid != ::geteuid()) {
        error = Error::make(ErrorCode::permission_denied,
                            "executable staging directory is not owned by the supervisor: " + path);
    } else if ((info.st_mode & 0022) != 0) {
        error = Error::make(ErrorCode::permission_denied,
                            "executable staging directory is writable by group/other: " + path);
    } else {
        const mode_t wanted = ::geteuid() == 0
                                  ? static_cast<mode_t>((info.st_mode & 07777) | 0011)
                                  : static_cast<mode_t>(0700);
        if ((info.st_mode & 07777) != wanted && ::fchmod(fd, wanted) != 0) {
            error = Error::make(ErrorCode::permission_denied,
                                "cannot restrict executable staging directory " + path + ": " +
                                    std::strerror(errno), errno);
        }
    }
    ::close(fd);
    return error;
}


} // namespace

struct SupervisorEngine::ServiceHandle {
    ServiceRecord record;
    int log_fd = -1;
    int exec_fd = -1;
    bool exec_confirmed = false;
    bool exec_failed = false;
    bool script_entry = false;
    std::string exec_error;
    bool adopted = false;
    bool log_discarding = false;
    // Per-spawn executable staging symlink (not persisted; only needed while
    // this instance is being started/alive).
    std::string exec_staging_link;
    std::string exec_staging_directory;
    // A runtime write failed. Automatic starts/restarts are disabled until a
    // later manual start successfully persists its intent (PSH-04).
    bool persistence_degraded = false;

    std::int64_t backoff_deadline_ms = 0;
    std::int64_t stable_deadline_ms = 0;
    std::int64_t next_metrics_ms = 0;
    std::int64_t last_adopt_check_ms = 0;

    std::int64_t stop_term_deadline_ms = 0;
    std::int64_t stop_verify_deadline_ms = 0;
    bool stop_kill_sent = false;
    bool restart_after_stop = false;

    bool pending_auto_restart = false;
    bool cleanup_kill_sent = false;
    std::int64_t cleanup_deadline_ms = 0;

    bool has_cpu_prev = false;
    std::uint64_t last_cpu_ticks = 0;
    std::int64_t last_cpu_sample_ms = 0;
    bool cpu_available = false;
    double cpu_percent = 0.0;
    bool rss_available = false;
    std::int64_t rss_bytes = 0;
};

struct SupervisorEngine::PendingOperation {
    std::string operation_id;
    std::string service_id;
    bool restart_after = false;
    std::int64_t deadline_ms = 0;
};

struct SupervisorEngine::ServiceOperation {
    std::string id;
    std::string service_id;
    std::string kind;
    // pending | succeeded | failed
    std::string state = "pending";
    nlohmann::json result = nullptr;
    Error error;
    std::string slug;
    std::int64_t created_at = 0;
    std::int64_t finished_at = 0;
    // Monotonic deadline used only to fail an operation whose completion timer
    // never fired. Never exposed as a wall-clock time.
    std::int64_t deadline_ms = 0;
};

std::unique_ptr<SupervisorEngine> SupervisorEngine::create(const EngineOptions& options,
                                                          Error& error) {
    std::unique_ptr<SupervisorEngine> engine(new SupervisorEngine(options));
    error = engine->initialize();
    if (!error.ok()) return nullptr;
    return engine;
}

Error SupervisorEngine::initialize() {
    if (options_.database_path.empty()) {
        return Error::make(ErrorCode::invalid_argument, "supervisor database path is required");
    }
    if (options_.log_directory.empty()) {
        return Error::make(ErrorCode::invalid_argument, "supervisor log directory is required");
    }
    boot_id_ = options_.boot_id.empty() ? read_boot_id() : options_.boot_id;
    if (boot_id_.empty()) {
        return Error::make(ErrorCode::unavailable,
                           "cannot read kernel boot ID; refusing to manage processes without "
                           "reliable identity");
    }
    if (boot_id_.size() > 128) {
        return Error::make(ErrorCode::invalid_argument, "boot ID is implausibly long");
    }
    started_at_ms_ = now_unix_millis();
    engine_steady_ms_ = steady_millis();

    Error error;
    store_ = SupervisorStore::open(options_.database_path, error);
    if (!store_) return error;
    logs_ = LogStore::open(options_.log_directory, error);
    if (!logs_) return error;

    exec_staging_ = options_.exec_staging_directory.empty()
                        ? default_exec_staging_directory()
                        : options_.exec_staging_directory;
    error = prepare_exec_staging_directory(exec_staging_);
    if (!error.ok() && options_.exec_staging_directory.empty()) {
        // /run may be read-only in some containers; /tmp is the portable
        // fallback (systemd's PrivateTmp keeps it private to the daemon).
        exec_staging_ = "/tmp/edgedesk-supervisor-exec-" +
                        std::to_string(static_cast<unsigned>(::geteuid()));
        error = prepare_exec_staging_directory(exec_staging_);
    }
    if (!error.ok()) return error;

    std::vector<ServiceRecord> records;
    error = store_->load_all(records);
    if (!error.ok()) return error;

    // A previous supervisor observed a runtime write failure. Its database rows
    // may describe an intent that was never committed, so this start must not
    // turn stale desired_state into a new process (PSH-04). The marker is only
    // cleared after every loaded row has been normalized and persisted.
    const bool inconsistent = store_->inconsistent_marker_present();
    const std::int64_t now = steady_millis();
    for (auto& record : records) {
        ServiceHandle handle;
        handle.record = std::move(record);
        auto& runtime = handle.record.runtime;
        if (runtime.desired_state != "running" && runtime.desired_state != "stopped") {
            runtime.desired_state = "stopped";
        }
        auto& config = handle.record.config;

        // Persisted STOPPING/BACKOFF work is resumed before a new instance may
        // run (SPEC-0010 PSH-02/PSH-05). The runtime identity is kept until the
        // old group is confirmed gone.
        const auto begin_cleanup = [&](bool restart_after) {
            runtime.state = ServiceState::stopping;
            handle.restart_after_stop = restart_after;
            handle.stop_kill_sent = false;
            handle.stop_term_deadline_ms = now + config.stop_timeout_ms;
            handle.stop_verify_deadline_ms = handle.stop_term_deadline_ms + kStopKillGraceMs;
            handle.backoff_deadline_ms = 0;
            handle.pending_auto_restart = false;
            handle.cleanup_deadline_ms = 0;
            handle.stable_deadline_ms = 0;
            const GroupProbe probe =
                probe_service_group(runtime.pgid, runtime.pid, runtime.start_ticks,
                                    runtime.boot_id, boot_id_, runtime.anchor_pid,
                                    runtime.anchor_start_ticks);
            if (probe.state == GroupState::uncertain) {
                runtime.state = ServiceState::unknown;
                runtime.last_error = "cannot verify recovered process group: " + probe.detail;
                return;
            }
            if (probe.state == GroupState::absent) {
                clear_identity(handle);
                runtime.state = restart_after ? ServiceState::backoff : ServiceState::stopped;
                if (restart_after) handle.backoff_deadline_ms = now;
                return;
            }
            handle.adopted = true;
            if (probe.state == GroupState::anchor_only) {
                std::string signal_error;
                if (signal_service_group(probe, SIGKILL, signal_error)) {
                    handle.stop_kill_sent = true;
                } else {
                    const GroupProbe after = probe_service_group(
                        runtime.pgid, runtime.pid, runtime.start_ticks, runtime.boot_id,
                        boot_id_, runtime.anchor_pid, runtime.anchor_start_ticks);
                    if (after.state == GroupState::absent) {
                        clear_identity(handle);
                        runtime.state = restart_after ? ServiceState::backoff
                                                      : ServiceState::stopped;
                        if (restart_after) handle.backoff_deadline_ms = now;
                    } else {
                        runtime.state = ServiceState::unknown;
                        runtime.last_error = "cannot reap recovered identity anchor: " +
                                             signal_error;
                    }
                }
                return;
            }
            std::string signal_error;
            if (!signal_service_group(probe, SIGTERM, signal_error)) {
                const GroupProbe after = probe_service_group(
                    runtime.pgid, runtime.pid, runtime.start_ticks, runtime.boot_id, boot_id_,
                    runtime.anchor_pid, runtime.anchor_start_ticks);
                if (after.state == GroupState::absent) {
                    clear_identity(handle);
                    runtime.state = restart_after ? ServiceState::backoff : ServiceState::stopped;
                    if (restart_after) handle.backoff_deadline_ms = now;
                } else {
                    runtime.state = ServiceState::unknown;
                    runtime.last_error = "cannot signal recovered process group: " + signal_error;
                }
            }
        };

        const bool same_boot = !runtime.boot_id.empty() && runtime.boot_id == boot_id_;
        bool start_expected = false;
        if (!same_boot) {
            // A new system boot clears boot-specific overrides. autostart now
            // decides whether the service should be running, except after a
            // persistence failure where no automatic start is permitted.
            runtime.boot_id = boot_id_;
            runtime.state = ServiceState::stopped;
            clear_identity(handle);
            runtime.window_restarts = 0;
            runtime.window_started_at = 0;
            runtime.failed_locked = false;
            runtime.desired_state =
                (config.autostart && !inconsistent) ? "running" : "stopped";
            start_expected = config.autostart && !inconsistent;
            if (inconsistent && config.autostart) {
                runtime.last_error =
                    "previous Supervisor persistence failure; automatic start disabled";
            }
        } else {
            runtime.boot_id = boot_id_;
            const bool wants_running = runtime.desired_state == "running";
            const bool may_start = wants_running && !inconsistent;
            const bool interrupted_cleanup = runtime.state == ServiceState::stopping ||
                                             runtime.state == ServiceState::backoff;
            const bool interrupted_start =
                runtime.state == ServiceState::starting && runtime.pid <= 0;
            const bool leader_ok =
                runtime.pid > 0 && runtime.start_ticks > 0 &&
                process_identity_matches(runtime.pid, boot_id_, runtime.start_ticks);
            GroupProbe probe;
            if (runtime.pgid > 0 && runtime.start_ticks > 0) {
                probe = probe_service_group(runtime.pgid, runtime.pid, runtime.start_ticks,
                                            runtime.boot_id, boot_id_, runtime.anchor_pid,
                                            runtime.anchor_start_ticks);
            }

            if (interrupted_start) {
                // The start intent was persisted but no process identity ever
                // was. A write failure may be why; never reinterpret the stale
                // desired_state as a request to start now (PSH-04).
                clear_identity(handle);
                runtime.state = ServiceState::failed;
                runtime.desired_state = "stopped";
                runtime.last_error =
                    "interrupted service start: process identity was never persisted; "
                    "manual start required";
            } else if (runtime.pid > 0 && runtime.start_ticks == 0) {
                // A persisted PID without a verifiable start time could be a
                // reused PID. Never signal it or start a second copy.
                runtime.state = ServiceState::unknown;
                runtime.last_error =
                    "recovered PID has no verifiable start time; manual intervention required";
            } else if (probe.state == GroupState::uncertain) {
                runtime.state = ServiceState::unknown;
                runtime.last_error =
                    "cannot verify recovered process identity: " + probe.detail +
                    "; manual intervention required";
            } else if (probe.state == GroupState::absent && runtime.pgid > 0 &&
                       runtime.start_ticks > 0 &&
                       process_group_has_any_members(runtime.pgid)) {
                // The scan saw no verifiable member but a live process still
                // uses this PGID. Refuse to guess.
                runtime.state = ServiceState::unknown;
                runtime.last_error =
                    "recovered process group contains unverifiable members; manual intervention "
                    "required";
            } else if (leader_ok && !interrupted_cleanup && wants_running) {
                // The old Supervisor died but the service survived. Adopt it;
                // it is not our child, so status comes from /proc.
                handle.adopted = true;
                runtime.state = ServiceState::running;
                if (runtime.pgid <= 0) {
                    ProcStat stat;
                    if (read_proc_stat(runtime.pid, stat) && stat.pgrp > 1) {
                        runtime.pgid = stat.pgrp;
                    }
                }
                if (runtime.started_at_ms <= 0) runtime.started_at_ms = now_unix_millis();
            } else if (leader_ok || probe.state == GroupState::verified ||
                       probe.state == GroupState::anchor_only) {
                // The persisted desired state asks for stopped, or a restart
                // was interrupted. Verify identity, then stop/clean the old
                // group before anything else. A pending desired=running starts
                // only after that cleanup completes.
                begin_cleanup(may_start);
            } else {
                clear_identity(handle);
                runtime.state = ServiceState::stopped;
                if (runtime.failed_locked) {
                    runtime.state = ServiceState::failed;
                } else if (may_start) {
                    start_expected = true;
                } else if (wants_running) {
                    runtime.state = ServiceState::failed;
                    runtime.desired_state = "stopped";
                    runtime.last_error =
                        "previous Supervisor persistence failure; automatic start disabled";
                }
            }
        }

        if (start_expected) {
            runtime.state = ServiceState::backoff;
            handle.backoff_deadline_ms = now;
        }
        error = store_->save_runtime(handle.record.config.id, runtime);
        if (!error.ok()) return error;
        services_.emplace(handle.record.config.id, std::move(handle));
    }
    if (inconsistent) {
        // Normalization is durable only once every row above was written.
        const Error clear_error = store_->clear_inconsistent_marker();
        if (!clear_error.ok()) return clear_error;
    }
    return Error::none();
}

SupervisorEngine::SupervisorEngine(EngineOptions options) : options_(std::move(options)) {}

SupervisorEngine::~SupervisorEngine() {
    // Service processes deliberately survive an abrupt Supervisor exit so a
    // replacement can adopt them; only our pipe descriptors are released.
    for (auto& item : services_) {
        close_fd(item.second.log_fd);
        close_fd(item.second.exec_fd);
        remove_exec_staging(item.second);
    }
}

SupervisorEngine::ServiceHandle* SupervisorEngine::find(std::string_view id) {
    const auto found = services_.find(std::string(id));
    return found == services_.end() ? nullptr : &found->second;
}

const SupervisorEngine::ServiceHandle* SupervisorEngine::find(std::string_view id) const {
    const auto found = services_.find(std::string(id));
    return found == services_.end() ? nullptr : &found->second;
}

std::size_t SupervisorEngine::running_count() const {
    std::size_t count = 0;
    for (const auto& item : services_) {
        const ServiceState state = item.second.record.runtime.state;
        if (state == ServiceState::running || state == ServiceState::starting) ++count;
    }
    return count;
}

nlohmann::json SupervisorEngine::record_json(const ServiceHandle& handle,
                                             std::int64_t now_ms) const {
    nlohmann::json body = service_record_json(handle.record);
    const auto& runtime = handle.record.runtime;
    const bool active = runtime.pid > 0 &&
                        (runtime.state == ServiceState::running ||
                         runtime.state == ServiceState::starting);
    if (active && runtime.started_at_ms > 0) {
        const std::int64_t uptime_ms = now_unix_millis() - runtime.started_at_ms;
        body["uptime_seconds"] = uptime_ms > 0 ? uptime_ms / 1000 : 0;
    } else {
        body["uptime_seconds"] = nullptr;
    }
    if (handle.cpu_available) {
        body["cpu_percent"] = handle.cpu_percent;
    } else {
        body["cpu_percent"] = nullptr;
    }
    if (handle.rss_available) {
        body["rss_bytes"] = handle.rss_bytes;
    } else {
        body["rss_bytes"] = nullptr;
    }
    body["metrics_available"] = handle.cpu_available && handle.rss_available;
    (void)now_ms;
    return body;
}

nlohmann::json SupervisorEngine::health_json(std::int64_t now_ms) const {
    nlohmann::json body;
    body["status"] = "ok";
    body["service"] = "edgedesk-supervisor";
    body["version"] = EDGEDESK_VERSION;
    body["boot_id"] = boot_id_;
    body["uptime_seconds"] =
        now_ms > engine_steady_ms_ ? (now_ms - engine_steady_ms_) / 1000 : 0;
    body["services"] = services_.size();
    body["running"] = running_count();
    body["persistence_errors"] = persistence_errors_;
    body["log_errors"] = log_errors_;
    body["degraded"] = persistence_errors_ > 0;
    return body;
}

RpcResult SupervisorEngine::rpc(std::string_view method, const nlohmann::json& params,
                                std::uint64_t ticket) {
    (void)ticket;  // stop/restart are asynchronous operations identified by operation_id.
    const std::int64_t now = steady_millis();
    if (!params.is_null() && !params.is_object()) {
        return failure(ErrorCode::invalid_argument, "invalid_params",
                       "params must be a JSON object");
    }
    const nlohmann::json body = params.is_null() ? nlohmann::json::object() : params;
    if (method == "health") return success(health_json(now));
    if (method == "service.list") {
        std::size_t offset = 0;
        std::size_t limit = 200;
        RpcResult error;
        std::int64_t parsed = 0;
        if (!read_optional_int(body, "offset", 0, 1000000, 0, parsed, error)) return error;
        offset = static_cast<std::size_t>(parsed);
        if (!read_optional_int(body, "limit", 1, 1000, 200, parsed, error)) return error;
        limit = static_cast<std::size_t>(parsed);
        bool filter_state = false;
        ServiceState wanted = ServiceState::stopped;
        if (body.contains("status") && !body["status"].is_null()) {
            if (!body["status"].is_string() ||
                !parse_service_state(body["status"].get<std::string>(), wanted)) {
                return failure(ErrorCode::invalid_argument, "invalid_params",
                               "field 'status' must be a lifecycle state");
            }
            filter_state = true;
        }
        std::vector<const ServiceHandle*> matches;
        for (const auto& item : services_) {
            if (filter_state && item.second.record.runtime.state != wanted) continue;
            matches.push_back(&item.second);
        }
        std::sort(matches.begin(), matches.end(), [](const auto* left, const auto* right) {
            if (left->record.config.created_at != right->record.config.created_at) {
                return left->record.config.created_at < right->record.config.created_at;
            }
            return left->record.config.id < right->record.config.id;
        });
        const std::size_t total = matches.size();
        nlohmann::json list = nlohmann::json::array();
        for (std::size_t index = offset; index < total && index < offset + limit; ++index) {
            list.push_back(record_json(*matches[index], now));
        }
        nlohmann::json result;
        result["services"] = std::move(list);
        result["total"] = total;
        result["count"] = result["services"].size();
        result["offset"] = offset;
        result["limit"] = limit;
        return success(std::move(result));
    }
    if (method == "service.get") {
        std::string id;
        RpcResult error;
        if (!read_id(body, id, error)) return error;
        const ServiceHandle* handle = find(id);
        if (handle == nullptr) {
            return failure(ErrorCode::not_found, "service_not_found", "service not found: " + id);
        }
        return success(record_json(*handle, now));
    }
    if (method == "service.create") return handle_create(body, now);
    if (method == "service.update") return handle_update(body, now);
    if (method == "service.delete") return handle_delete(body);
    if (method == "service.start") {
        std::string id;
        RpcResult error;
        if (!read_id(body, id, error)) return error;
        return handle_start(id, now);
    }
    if (method == "service.stop") {
        std::string id;
        RpcResult error;
        if (!read_id(body, id, error)) return error;
        return handle_stop(id, now);
    }
    if (method == "service.restart") {
        std::string id;
        RpcResult error;
        if (!read_id(body, id, error)) return error;
        return handle_restart(id, now);
    }
    if (method == "service.operation.get") {
        if (!body.contains("operation_id") || !body["operation_id"].is_string()) {
            return failure(ErrorCode::invalid_argument, "invalid_params",
                           "field 'operation_id' is required");
        }
        return handle_operation(body["operation_id"].get<std::string>());
    }
    if (method == "service.logs") {
        return handle_logs(body);
    }
    return failure(ErrorCode::unsupported, "method_not_found",
                   "unknown supervisor method: " + std::string(method));
}

RpcResult SupervisorEngine::handle_create(const nlohmann::json& params, std::int64_t now_ms) {
    ServiceConfig config;
    Error error = service_config_from_json(params, config, false);
    if (!error.ok()) return failure(error.code, "invalid_service_config", error.message);
    error = validate_service_config(config);
    if (!error.ok()) return failure(error.code, config_error_slug(error), error.message);

    ServiceRecord record;
    record.config = std::move(config);
    record.runtime.state = ServiceState::stopped;
    record.runtime.desired_state = "stopped";
    record.runtime.boot_id = boot_id_;
    error = store_->insert(record);
    if (!error.ok()) {
        const std::string slug =
            error.code == ErrorCode::conflict ? "service_name_conflict" : "supervisor_unavailable";
        return failure(error.code, slug, error.message);
    }
    ServiceHandle handle;
    handle.record = std::move(record);
    auto inserted = services_.emplace(handle.record.config.id, std::move(handle));
    return success(record_json(inserted.first->second, now_ms));
}

RpcResult SupervisorEngine::handle_update(const nlohmann::json& params, std::int64_t now_ms) {
    std::string id;
    RpcResult error;
    if (!read_id(params, id, error)) return error;
    ServiceHandle* handle = find(id);
    if (handle == nullptr) {
        return failure(ErrorCode::not_found, "service_not_found", "service not found: " + id);
    }
    nlohmann::json body = params;
    body.erase("id");
    ServiceConfig candidate = handle->record.config;
    Error config_error = service_config_from_json(body, candidate, true);
    if (!config_error.ok()) {
        return failure(config_error.code, "invalid_service_config", config_error.message);
    }
    candidate.id = id;
    candidate.created_at = handle->record.config.created_at;
    config_error = validate_service_config(candidate);
    if (!config_error.ok()) {
        return failure(config_error.code, config_error_slug(config_error), config_error.message);
    }
    config_error = store_->update(candidate);
    if (!config_error.ok()) {
        const std::string slug = config_error.code == ErrorCode::conflict
                                     ? "service_name_conflict"
                                     : "supervisor_unavailable";
        return failure(config_error.code, slug, config_error.message);
    }
    candidate.updated_at = now_unix_seconds();
    handle->record.config = std::move(candidate);
    return success(record_json(*handle, now_ms));
}

RpcResult SupervisorEngine::handle_delete(const nlohmann::json& params) {
    std::string id;
    RpcResult error;
    if (!read_id(params, id, error)) return error;
    ServiceHandle* handle = find(id);
    if (handle == nullptr) {
        return failure(ErrorCode::not_found, "service_not_found", "service not found: " + id);
    }
    const ServiceState state = handle->record.runtime.state;
    if (state == ServiceState::starting || state == ServiceState::running ||
        state == ServiceState::stopping || state == ServiceState::backoff ||
        (state == ServiceState::unknown && unverified_process_may_exist(*handle)) ||
        group_alive(*handle)) {
        return failure(ErrorCode::conflict, "service_running",
                       "service must be stopped before it can be deleted");
    }
    Error log_error = logs_->remove(id);
    if (!log_error.ok()) {
        return failure(log_error.code, "supervisor_error", log_error.message);
    }
    Error remove_error = store_->remove(id);
    if (!remove_error.ok()) {
        return failure(remove_error.code, "supervisor_unavailable", remove_error.message);
    }
    close_fd(handle->log_fd);
    close_fd(handle->exec_fd);
    remove_exec_staging(*handle);
    services_.erase(id);
    nlohmann::json result;
    result["deleted"] = true;
    result["id"] = id;
    return success(std::move(result));
}

RpcResult SupervisorEngine::handle_start(std::string_view id, std::int64_t now_ms) {
    ServiceHandle* handle = find(id);
    if (handle == nullptr) {
        return failure(ErrorCode::not_found, "service_not_found",
                       "service not found: " + std::string(id));
    }
    const ServiceState state = handle->record.runtime.state;
    if (state == ServiceState::running || state == ServiceState::starting) {
        return failure(ErrorCode::conflict, "service_running", "service is already running");
    }
    if (state == ServiceState::stopping) {
        return failure(ErrorCode::conflict, "service_stopping",
                       "service is stopping; wait for it to stop before starting it");
    }
    if (state == ServiceState::unknown) {
        if (unverified_process_may_exist(*handle)) {
            return failure(ErrorCode::conflict, "service_unknown",
                           "service process identity is unknown; refusing to start a duplicate");
        }
        clear_identity(*handle);
    } else if (group_uncertain(*handle)) {
        return failure(ErrorCode::conflict, "service_unknown",
                       "cannot verify whether a previous process group still belongs to this "
                       "service; refusing to start a duplicate");
    } else if (group_alive(*handle)) {
        return failure(ErrorCode::conflict, "service_running",
                       "a process from the previous run is still alive");
    }

    close_fd(handle->log_fd);
    close_fd(handle->exec_fd);
    handle->adopted = false;
    handle->exec_confirmed = false;
    handle->exec_failed = false;
    handle->exec_error.clear();
    handle->log_discarding = false;
    handle->pending_auto_restart = false;
    handle->cleanup_kill_sent = false;
    handle->cleanup_deadline_ms = 0;
    handle->stop_kill_sent = false;
    handle->restart_after_stop = false;
    handle->backoff_deadline_ms = 0;
    handle->stable_deadline_ms = 0;
    handle->has_cpu_prev = false;
    handle->cpu_available = false;
    handle->rss_available = false;

    auto& runtime = handle->record.runtime;
    runtime.desired_state = "running";
    runtime.failed_locked = false;
    runtime.window_restarts = 0;
    runtime.window_started_at = now_ms;
    runtime.last_error.clear();

    Error error = spawn_locked(*handle, now_ms);
    if (!error.ok()) {
        // spawn_locked already persisted the failed state when it could.
        return failure(error.code, config_error_slug(error), error.message);
    }
    return success(record_json(*handle, now_ms));
}

RpcResult SupervisorEngine::handle_stop(std::string_view id, std::int64_t now_ms) {
    ServiceHandle* handle = find(id);
    if (handle == nullptr) {
        return failure(ErrorCode::not_found, "service_not_found",
                       "service not found: " + std::string(id));
    }
    // Repeated submissions are idempotent while the same stop is pending.
    if (ServiceOperation* pending = pending_operation(id, "stop")) {
        return success(operation_json(*pending));
    }
    const ServiceState state = handle->record.runtime.state;
    const auto stopped_result = [&](bool ok, std::string slug, std::string message) {
        if (!ok) {
            return failure(ErrorCode::unavailable, std::move(slug), std::move(message));
        }
        const std::string operation_id =
            record_completed_operation(*handle, "stop", now_ms, true, {}, {});
        return success(operation_json(operations_.at(operation_id)));
    };

    if (state == ServiceState::stopped || state == ServiceState::failed ||
        state == ServiceState::backoff) {
        auto& runtime = handle->record.runtime;
        runtime.desired_state = "stopped";
        runtime.failed_locked = false;
        runtime.last_error.clear();
        handle->pending_auto_restart = false;
        handle->backoff_deadline_ms = 0;
        handle->cleanup_deadline_ms = 0;
        // Persist the stop intent before touching the process (PSH-04).
        Error persist = save(*handle);
        if (!persist.ok()) {
            return failure(persist.code, "supervisor_unavailable",
                           "cannot persist stop intent: " + persist.message);
        }
        if (group_uncertain(*handle)) {
            runtime.state = ServiceState::unknown;
            runtime.last_error =
                "cannot verify whether the previous process group belongs to this service";
            (void)save(*handle);
            return failure(ErrorCode::conflict, "service_unknown", runtime.last_error);
        }
        if (group_alive(*handle)) {
            // A previous leader exited but descendants are still alive. Stop
            // them now instead of forgetting the process group (PSH-02).
            RpcResult error;
            const std::string operation_id =
                begin_operation(*handle, "stop", false, now_ms, error);
            if (!error.ok) return error;
            initiate_stop(*handle, false, now_ms);
            if (handle->record.runtime.state == ServiceState::unknown) {
                fail_pending(id, "service_unknown", handle->record.runtime.last_error);
            }
            (void)save(*handle);
            return success(operation_json(operations_.at(operation_id)));
        }
        runtime.state = ServiceState::stopped;
        clear_identity(*handle);
        persist = save(*handle);
        return stopped_result(persist.ok(), "supervisor_unavailable",
                              "cannot persist stopped state: " + persist.message);
    }
    if (state == ServiceState::unknown) {
        if (unverified_process_may_exist(*handle)) {
            return failure(ErrorCode::conflict, "service_unknown",
                           "service process identity is unknown; refusing to signal a reused PID");
        }
        auto& runtime = handle->record.runtime;
        runtime.state = ServiceState::stopped;
        runtime.desired_state = "stopped";
        runtime.failed_locked = false;
        clear_identity(*handle);
        runtime.last_error.clear();
        const Error persist = save(*handle);
        return stopped_result(persist.ok(), "supervisor_unavailable",
                              "cannot persist stopped state: " + persist.message);
    }
    if (state == ServiceState::running || state == ServiceState::starting ||
        state == ServiceState::stopping) {
        auto& runtime = handle->record.runtime;
        if (state == ServiceState::stopping) {
            // A manual stop wins over a pending restart.
            handle->restart_after_stop = false;
        }
        runtime.desired_state = "stopped";
        // Persist the intent first; if that fails nothing is signalled and the
        // caller must not believe the stop was accepted.
        Error persist = save(*handle);
        if (!persist.ok()) {
            runtime.desired_state = "running";
            return failure(persist.code, "supervisor_unavailable",
                           "cannot persist stop intent: " + persist.message);
        }
        RpcResult error;
        const std::string operation_id =
            begin_operation(*handle, "stop", false, now_ms, error);
        if (!error.ok) return error;
        if (state != ServiceState::stopping) {
            initiate_stop(*handle, false, now_ms);
            if (handle->record.runtime.state == ServiceState::unknown) {
                fail_pending(id, "service_unknown", handle->record.runtime.last_error);
            }
        }
        (void)save(*handle);
        return success(operation_json(operations_.at(operation_id)));
    }
    return failure(ErrorCode::conflict, "service_state",
                   "service cannot be stopped from its current state");
}

RpcResult SupervisorEngine::handle_restart(std::string_view id, std::int64_t now_ms) {
    ServiceHandle* handle = find(id);
    if (handle == nullptr) {
        return failure(ErrorCode::not_found, "service_not_found",
                       "service not found: " + std::string(id));
    }
    // A repeated restart request joins the operation that is already running.
    if (ServiceOperation* pending = pending_operation(id, "restart")) {
        return success(operation_json(*pending));
    }
    const ServiceState state = handle->record.runtime.state;
    if (state == ServiceState::stopping) {
        return failure(ErrorCode::conflict, "service_stopping",
                       "service is stopping; start it again after the stop completes");
    }
    if (state == ServiceState::unknown) {
        if (unverified_process_may_exist(*handle)) {
            return failure(ErrorCode::conflict, "service_unknown",
                           "service process identity is unknown; refusing to start a duplicate");
        }
        clear_identity(*handle);
    } else if (state != ServiceState::running && state != ServiceState::starting) {
        if (group_uncertain(*handle)) {
            return failure(ErrorCode::conflict, "service_unknown",
                           "cannot verify whether a previous process group still belongs to this "
                           "service; refusing to start a duplicate");
        }
        if (group_alive(*handle)) {
            return failure(ErrorCode::conflict, "service_running",
                           "a process from the previous run is still alive");
        }
    }
    if (state == ServiceState::running || state == ServiceState::starting) {
        handle->record.runtime.desired_state = "running";
        // Persist the restart intent before signalling anything (PSH-04).
        const Error persist = save(*handle);
        if (!persist.ok()) {
            return failure(persist.code, "supervisor_unavailable",
                           "cannot persist restart intent: " + persist.message);
        }
        RpcResult error;
        const std::string operation_id =
            begin_operation(*handle, "restart", true, now_ms, error);
        if (!error.ok) return error;
        initiate_stop(*handle, true, now_ms);
        if (handle->record.runtime.state == ServiceState::unknown) {
            fail_pending(id, "service_unknown", handle->record.runtime.last_error);
        } else if (handle->record.runtime.state != ServiceState::stopping) {
            // complete_stop() already retired the operation when the group had
            // already disappeared; this covers any residual bookkeeping.
            const ServiceState after = handle->record.runtime.state;
            complete_pending(id, now_ms, after == ServiceState::starting ||
                                             after == ServiceState::running);
        }
        (void)save(*handle);
        return success(operation_json(operations_.at(operation_id)));
    }

    close_fd(handle->log_fd);
    close_fd(handle->exec_fd);
    handle->adopted = false;
    handle->exec_confirmed = false;
    handle->exec_failed = false;
    handle->exec_error.clear();
    handle->log_discarding = false;
    handle->pending_auto_restart = false;
    handle->cleanup_kill_sent = false;
    handle->cleanup_deadline_ms = 0;
    handle->backoff_deadline_ms = 0;
    handle->has_cpu_prev = false;
    handle->cpu_available = false;
    handle->rss_available = false;

    auto& runtime = handle->record.runtime;
    runtime.desired_state = "running";
    runtime.failed_locked = false;
    runtime.window_restarts = 0;
    runtime.window_started_at = now_ms;
    runtime.last_error.clear();
    const Error error = spawn_locked(*handle, now_ms);
    const std::string operation_id =
        record_completed_operation(*handle, "restart", now_ms, error.ok(),
                                   error.ok() ? std::string() : config_error_slug(error),
                                   error.ok() ? std::string() : error.message);
    // The operation object carries the outcome; a failed restart is not an
    // HTTP/IPC error because the request itself was accepted and executed.
    return success(operation_json(operations_.at(operation_id)));
}

RpcResult SupervisorEngine::handle_logs(const nlohmann::json& params) const {
    if (!params.is_object()) {
        return failure(ErrorCode::invalid_argument, "invalid_params",
                       "params must be a JSON object");
    }
    std::string id;
    RpcResult error;
    if (!read_id(params, id, error)) return error;
    if (find(id) == nullptr) {
        return failure(ErrorCode::not_found, "service_not_found", "service not found: " + id);
    }
    bool has_tail = false;
    bool has_cursor = false;
    bool has_max = false;
    std::uint64_t tail = 0;
    std::uint64_t cursor = 0;
    std::uint64_t max_bytes = 0;
    if (!read_optional_u64(params, "tail_bytes", has_tail, tail, error)) return error;
    if (!read_optional_u64(params, "cursor", has_cursor, cursor, error)) return error;
    if (!read_optional_u64(params, "max_bytes", has_max, max_bytes, error)) return error;
    if (!has_cursor && !has_tail) tail = 16 * 1024;
    if (!has_max) max_bytes = 64 * 1024;
    LogReadResult read;
    const Error log_error = logs_->read(id, tail, has_cursor, cursor, max_bytes, read);
    if (!log_error.ok()) {
        return failure(log_error.code, "supervisor_error", log_error.message);
    }
    nlohmann::json result;
    result["service_id"] = id;
    result["content"] = read.content;
    result["cursor"] = read.cursor;
    result["next_cursor"] = read.next_cursor;
    result["size"] = read.size;
    result["reset"] = read.reset;
    result["truncated"] = read.truncated;
    return success(std::move(result));
}

namespace {
void reap_child(pid_t pid) {
    int status = 0;
    for (int attempt = 0; attempt < 500; ++attempt) {
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) return;
        ::usleep(2000);
    }
    (void)::kill(pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

std::string exec_error_message(int value) {
    return std::string(std::strerror(value));
}
} // namespace

Error SupervisorEngine::spawn_locked(ServiceHandle& handle, std::int64_t now_ms) {
    ResolvedIdentity identity;
    Error error = resolve_identity(handle.record.config.run_user,
                                   handle.record.config.run_group, identity);
    if (!error.ok()) return error;
    if (identity.is_root && !handle.record.config.allow_root) {
        return Error::make(ErrorCode::permission_denied,
                           "running as root requires explicit confirmation (allow_root=true)");
    }

    auto& runtime = handle.record.runtime;
    // Persist the start intent (and STARTING) before a child exists. A failure
    // here must not spawn anything: the caller reports the persistence error.
    runtime.state = ServiceState::starting;
    runtime.desired_state = "running";
    runtime.boot_id = boot_id_;
    clear_identity(handle);
    remove_exec_staging(handle);
    // Automatic restarts call spawn_locked directly, so a still-open log pipe
    // from the previous instance must be drained and released before the new
    // descriptors overwrite it (otherwise the FD and the tail of its output are
    // leaked on every restart).
    if (handle.log_fd >= 0) {
        drain_log(handle, kLogDrainBudget);
        close_fd(handle.log_fd);
    }
    close_fd(handle.exec_fd);
    runtime.last_error.clear();
    error = save(handle);
    if (!error.ok()) {
        runtime.state = ServiceState::failed;
        runtime.desired_state = "stopped";
        runtime.last_error = "cannot persist start intent: " + error.message;
        return error;
    }
    // The intent is durable again; a later failure re-enters degraded mode.
    handle.persistence_degraded = false;

    SpawnOutcome outcome = spawn_service(handle.record.config, identity, exec_staging_);
    if (!outcome.error.ok()) {
        runtime.state = ServiceState::failed;
        runtime.desired_state = "stopped";
        runtime.last_error = outcome.error.message;
        (void)save(handle);
        return outcome.error;
    }
    handle.exec_staging_link = outcome.exec_staging_link;
    handle.exec_staging_directory = outcome.exec_staging_directory;
    if (outcome.pid <= 0) {
        abort_service_child(outcome.handshake_fd);
        close_fd(outcome.log_read_fd);
        close_fd(outcome.exec_read_fd);
        runtime.state = ServiceState::failed;
        runtime.desired_state = "stopped";
        runtime.last_error = "spawn returned no child PID";
        remove_exec_staging(handle);
        (void)save(handle);
        return Error::make(ErrorCode::io_error, runtime.last_error);
    }
    if (outcome.start_ticks == 0 || outcome.anchor_pid <= 0 ||
        outcome.anchor_start_ticks == 0) {
        // Without a kernel start time (or a verified sentinel) the process
        // identity can never be verified after a Supervisor restart. The child
        // is blocked on the handshake; abort it and reap both processes.
        abort_service_child(outcome.handshake_fd);
        (void)::kill(outcome.pid, SIGKILL);
        reap_child(outcome.pid);
        if (outcome.anchor_pid > 1) reap_child(outcome.anchor_pid);
        close_fd(outcome.log_read_fd);
        close_fd(outcome.exec_read_fd);
        runtime.state = ServiceState::failed;
        runtime.desired_state = "stopped";
        runtime.last_error =
            "cannot determine a verifiable process identity; refusing to manage the process";
        remove_exec_staging(handle);
        (void)save(handle);
        return Error::make(ErrorCode::unavailable, runtime.last_error);
    }

    runtime.pid = outcome.pid;
    runtime.pgid = outcome.pgid;
    runtime.start_ticks = outcome.start_ticks;
    runtime.anchor_pid = outcome.anchor_pid;
    runtime.anchor_start_ticks = outcome.anchor_start_ticks;
    runtime.started_at_ms = now_unix_millis();
    runtime.last_error.clear();
    // Persist PID/PGID/boot ID/start time and the sentinel identity before the
    // child is allowed to exec.
    error = save(handle);
    if (!error.ok()) {
        abort_service_child(outcome.handshake_fd);
        reap_child(outcome.pid);
        if (outcome.anchor_pid > 1) reap_child(outcome.anchor_pid);
        runtime.anchor_pid = 0;
        runtime.anchor_start_ticks = 0;
        clear_identity(handle);
        close_fd(outcome.log_read_fd);
        close_fd(outcome.exec_read_fd);
        runtime.state = ServiceState::failed;
        runtime.desired_state = "stopped";
        runtime.last_error = "cannot persist process identity: " + error.message;
        remove_exec_staging(handle);
        (void)save(handle);
        return error;
    }

    std::string release_error;
    if (!release_service_child(outcome.handshake_fd, release_error)) {
        // The child exited before it could be released (typically a failed
        // setgroups/setgid/setuid). Surface the real errno instead of a
        // generic handshake error.
        std::string detail = release_error;
        unsigned char buffer[sizeof(int)] = {0};
        const ssize_t count = ::read(outcome.exec_read_fd, buffer, sizeof(int));
        if (count == static_cast<ssize_t>(sizeof(int))) {
            int value = 0;
            std::memcpy(&value, buffer, sizeof(int));
            detail = "cannot start service: " + exec_error_message(value);
        }
        (void)::kill(outcome.pid, SIGKILL);
        reap_child(outcome.pid);
        if (outcome.anchor_pid > 1) reap_child(outcome.anchor_pid);
        runtime.anchor_pid = 0;
        runtime.anchor_start_ticks = 0;
        clear_identity(handle);
        close_fd(outcome.log_read_fd);
        close_fd(outcome.exec_read_fd);
        runtime.state = ServiceState::failed;
        runtime.desired_state = "stopped";
        runtime.last_error = detail;
        remove_exec_staging(handle);
        (void)save(handle);
        return Error::make(ErrorCode::unavailable, detail);
    }

    handle.log_fd = outcome.log_read_fd;
    handle.exec_fd = outcome.exec_read_fd;
    handle.exec_confirmed = false;
    handle.exec_failed = false;
    handle.exec_error.clear();
    handle.script_entry = outcome.script_entry;
    handle.adopted = false;
    handle.log_discarding = false;
    handle.stop_kill_sent = false;
    handle.stop_term_deadline_ms = 0;
    handle.stop_verify_deadline_ms = 0;
    handle.restart_after_stop = false;
    handle.pending_auto_restart = false;
    handle.cleanup_kill_sent = false;
    handle.cleanup_deadline_ms = 0;
    handle.backoff_deadline_ms = 0;
    handle.stable_deadline_ms = 0;
    handle.next_metrics_ms = now_ms + options_.metrics_interval_ms;
    handle.has_cpu_prev = false;
    handle.cpu_available = false;
    handle.rss_available = false;
    (void)now_ms;
    return Error::none();
}

void SupervisorEngine::initiate_stop(ServiceHandle& handle, bool restart_after,
                                     std::int64_t now_ms) {
    auto& runtime = handle.record.runtime;
    runtime.state = ServiceState::stopping;
    handle.restart_after_stop = restart_after;
    handle.stop_kill_sent = false;
    handle.stop_term_deadline_ms = now_ms + handle.record.config.stop_timeout_ms;
    handle.stop_verify_deadline_ms = handle.stop_term_deadline_ms + kStopKillGraceMs;
    handle.backoff_deadline_ms = 0;
    handle.pending_auto_restart = false;
    handle.cleanup_deadline_ms = 0;
    handle.stable_deadline_ms = 0;

    const GroupProbe probe = probe_group(handle);
    if (probe.state == GroupState::uncertain) {
        // Never signal an unverified group (PSH-01).
        runtime.state = ServiceState::unknown;
        runtime.last_error = "cannot verify process group before stop: " + probe.detail;
        return;
    }
    if (probe.state == GroupState::absent) {
        complete_stop(handle, now_ms);
        return;
    }
    if (probe.state == GroupState::anchor_only) {
        // The workload is already gone; reap the sentinel immediately. SIGKILL
        // is safe because the sentinel still reserves the PGID.
        std::string signal_error;
        if (signal_service_group(probe, SIGKILL, signal_error)) {
            handle.stop_kill_sent = true;
            runtime.state = ServiceState::stopping;
            return;
        }
        const GroupProbe after = probe_group(handle);
        if (after.state == GroupState::absent) {
            complete_stop(handle, now_ms);
            return;
        }
        runtime.state = ServiceState::unknown;
        runtime.last_error = "cannot reap process group sentinel: " + signal_error;
        return;
    }
    std::string signal_error;
    if (signal_service_group(probe, SIGTERM, signal_error)) return;
    const GroupProbe after = probe_group(handle);
    if (after.state == GroupState::absent) {
        complete_stop(handle, now_ms);
        return;
    }
    runtime.state = ServiceState::unknown;
    runtime.last_error = "cannot signal process group: " + signal_error;
}

void SupervisorEngine::complete_stop(ServiceHandle& handle, std::int64_t now_ms) {
    auto& runtime = handle.record.runtime;
    const bool restart = handle.restart_after_stop;
    runtime.state = ServiceState::stopped;
    clear_identity(handle);
    remove_exec_staging(handle);
    runtime.last_error.clear();
    handle.restart_after_stop = false;
    handle.stop_kill_sent = false;
    handle.stop_term_deadline_ms = 0;
    handle.stop_verify_deadline_ms = 0;
    handle.pending_auto_restart = false;
    handle.cleanup_kill_sent = false;
    handle.cleanup_deadline_ms = 0;
    handle.adopted = false;
    close_fd(handle.exec_fd);
    handle.has_cpu_prev = false;
    handle.cpu_available = false;
    handle.rss_available = false;

    // Persist the completed cleanup (identity cleared) before any replacement
    // instance may start. If this write fails the service must not be restarted
    // and the operator must see the failure (PSH-04).
    const Error persist = save(handle);
    if (!persist.ok()) {
        runtime.state = ServiceState::unknown;
        runtime.last_error = "cannot persist completed stop: " + persist.message;
        (void)save(handle);
        fail_pending(handle.record.config.id, "supervisor_unavailable",
                     "cannot persist completed stop: " + persist.message);
        return;
    }

    bool restarted = false;
    if (restart && !shutting_down_ && runtime.desired_state == "running" &&
        !handle.persistence_degraded) {
        const Error error = spawn_locked(handle, now_ms);
        if (!error.ok()) {
            runtime.state = ServiceState::failed;
            runtime.desired_state = "stopped";
            runtime.last_error = error.message;
            (void)save(handle);
        } else {
            restarted = true;
        }
    }
    complete_pending(handle.record.config.id, now_ms, restarted);
}

void SupervisorEngine::schedule_restart(ServiceHandle& handle, std::int64_t now_ms) {
    auto& runtime = handle.record.runtime;
    const auto& config = handle.record.config;
    if (runtime.desired_state != "running") {
        // A stop request (including one that raced an UNKNOWN transition) wins
        // over the restart policy; never resurrect a service the operator
        // stopped.
        runtime.state = ServiceState::stopped;
        runtime.last_error.clear();
        handle.pending_auto_restart = false;
        handle.backoff_deadline_ms = 0;
        handle.cleanup_deadline_ms = 0;
        (void)save(handle);
        return;
    }
    if (runtime.state == ServiceState::unknown) {
        handle.pending_auto_restart = false;
        handle.backoff_deadline_ms = 0;
        return;
    }
    if (handle.persistence_degraded) {
        // A previous runtime write failed; do not launch a replacement from a
        // record whose durable state may not match memory (PSH-04).
        runtime.state = ServiceState::failed;
        runtime.desired_state = "stopped";
        runtime.last_error =
            "automatic restart disabled after a runtime persistence failure";
        handle.pending_auto_restart = false;
        handle.backoff_deadline_ms = 0;
        (void)save(handle);
        fail_pending(handle.record.config.id, "supervisor_unavailable", runtime.last_error);
        return;
    }
    if (runtime.window_started_at <= 0 ||
        now_ms - runtime.window_started_at > static_cast<std::int64_t>(config.restart_window_sec) * 1000) {
        runtime.window_restarts = 0;
        runtime.window_started_at = now_ms;
    }
    if (config.max_restarts == 0 || runtime.window_restarts >=
                                        static_cast<std::uint64_t>(config.max_restarts)) {
        runtime.state = ServiceState::failed;
        runtime.failed_locked = true;
        runtime.desired_state = "stopped";
        runtime.last_error = "restart limit reached; service is locked";
        (void)save(handle);
        fail_pending(handle.record.config.id, "service_failed", runtime.last_error);
        return;
    }
    std::uint64_t delay = static_cast<std::uint64_t>(config.restart_delay_ms);
    const unsigned shift = static_cast<unsigned>(std::min<std::uint64_t>(
        runtime.window_restarts, 20));
    delay = std::min<std::uint64_t>(delay << shift,
                                    static_cast<std::uint64_t>(config.max_restart_delay_ms));
    delay += jitter_for(config.id, runtime.window_restarts, static_cast<std::int64_t>(delay));
    runtime.window_restarts += 1;
    runtime.restart_count += 1;
    runtime.state = ServiceState::backoff;
    handle.backoff_deadline_ms = now_ms + static_cast<std::int64_t>(delay);
    const Error persist = save(handle);
    if (!persist.ok()) {
        // Without a durable backoff record a replacement instance must not be
        // started automatically (PSH-04).
        runtime.state = ServiceState::unknown;
        runtime.desired_state = "stopped";
        runtime.last_error = "cannot persist restart schedule: " + persist.message;
        handle.backoff_deadline_ms = 0;
        (void)save(handle);
        fail_pending(handle.record.config.id, "supervisor_unavailable",
                     "cannot persist restart schedule: " + persist.message);
    }
}

void SupervisorEngine::apply_exit(ServiceHandle& handle, bool failed, int exit_code, int signal,
                                  std::int64_t now_ms) {
    auto& runtime = handle.record.runtime;
    handle.cpu_available = false;
    handle.rss_available = false;
    handle.has_cpu_prev = false;
    runtime.last_exit_code = exit_code;
    runtime.last_signal = signal;
    runtime.last_exit_at = now_unix_millis();

    if (shutting_down_) {
        // Keep the process-group identity while descendants (or the sentinel)
        // still exist so shutdown() can terminate and persist them; never lose
        // the target.
        GroupProbe probe = probe_group(handle);
        if (probe.state == GroupState::anchor_only) {
            std::string signal_error;
            (void)signal_service_group(probe, SIGKILL, signal_error);
            probe = probe_group(handle);
        }
        if (probe.state == GroupState::uncertain) {
            runtime.state = ServiceState::unknown;
            runtime.last_error =
                "cannot verify the surviving process group during shutdown: " + probe.detail;
        } else if (probe.state != GroupState::absent) {
            runtime.state = ServiceState::stopping;
            handle.restart_after_stop = false;
            handle.stop_kill_sent = false;
            handle.stop_term_deadline_ms = now_ms;
            handle.stop_verify_deadline_ms = now_ms + kStopKillGraceMs;
            handle.pending_auto_restart = false;
            handle.cleanup_deadline_ms = 0;
        } else {
            runtime.state = ServiceState::stopped;
            clear_identity(handle);
            remove_exec_staging(handle);
        }
        (void)save(handle);
        return;
    }

    GroupProbe probe = probe_group(handle);
    // When only the sentinel is left there is nothing to terminate; reap it
    // immediately so the exit path can schedule/complete as if the group were
    // already gone (PSH-02).
    if (probe.state == GroupState::anchor_only) {
        std::string signal_error;
        if (!signal_service_group(probe, SIGKILL, signal_error)) {
            const GroupProbe after = probe_group(handle);
            if (after.state != GroupState::absent) {
                runtime.state = ServiceState::unknown;
                runtime.desired_state = "stopped";
                runtime.last_error = "cannot reap service identity anchor: " + signal_error;
                handle.pending_auto_restart = false;
                handle.backoff_deadline_ms = 0;
                handle.cleanup_deadline_ms = 0;
                (void)save(handle);
                fail_pending(handle.record.config.id, "service_unknown", runtime.last_error);
                return;
            }
            probe = after;
        } else {
            probe = probe_group(handle);
        }
    }

    // An unverifiable surviving group can neither be signalled nor replaced by
    // a second instance (PSH-01/PSH-02).
    if (probe.state == GroupState::uncertain) {
        runtime.state = ServiceState::unknown;
        runtime.desired_state = "stopped";
        runtime.last_error =
            "cannot verify the surviving process group after the leader exited; "
            "manual intervention required";
        handle.pending_auto_restart = false;
        handle.backoff_deadline_ms = 0;
        handle.cleanup_deadline_ms = 0;
        (void)save(handle);
        fail_pending(handle.record.config.id, "service_unknown", runtime.last_error);
        return;
    }

    const bool want_restart =
        runtime.desired_state == "running" &&
        runtime.state != ServiceState::unknown &&
        restart_policy_wants_restart(handle.record.config.restart_policy, failed) &&
        !handle.persistence_degraded;
    if (!want_restart) {
        runtime.desired_state = "stopped";
        if (probe.state == GroupState::absent) {
            runtime.state = ServiceState::stopped;
            clear_identity(handle);
            remove_exec_staging(handle);
            (void)save(handle);
            complete_pending(handle.record.config.id, now_ms, false);
            return;
        }
        // PSH-02: descendants survive the leader even when no restart follows.
        // Run the normal stop policy before clearing the identity metadata.
        runtime.state = ServiceState::stopping;
        handle.restart_after_stop = false;
        handle.stop_kill_sent = false;
        handle.stop_term_deadline_ms = now_ms + handle.record.config.stop_timeout_ms;
        handle.stop_verify_deadline_ms = handle.stop_term_deadline_ms + kStopKillGraceMs;
        handle.pending_auto_restart = false;
        handle.cleanup_deadline_ms = 0;
        handle.backoff_deadline_ms = 0;
        handle.stable_deadline_ms = 0;
        if (probe.state == GroupState::verified) {
            std::string signal_error;
            (void)signal_service_group(probe, SIGTERM, signal_error);
        }
        (void)save(handle);
        return;
    }

    if (probe.state == GroupState::verified) {
        // Descendants still exist. Terminate them before a new instance is
        // started so two copies cannot run concurrently.
        runtime.state = ServiceState::backoff;
        handle.pending_auto_restart = true;
        handle.cleanup_kill_sent = false;
        handle.cleanup_deadline_ms = now_ms + handle.record.config.stop_timeout_ms;
        handle.backoff_deadline_ms = 0;
        std::string signal_error;
        (void)signal_service_group(probe, SIGTERM, signal_error);
        (void)save(handle);
        return;
    }
    clear_identity(handle);
    remove_exec_staging(handle);
    schedule_restart(handle, now_ms);
}

void SupervisorEngine::handle_child_exit(ServiceHandle& handle, int status,
                                         std::int64_t now_ms) {
    auto& runtime = handle.record.runtime;
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    const int exit_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    // poll() drains the exec pipe before waitpid, but it can see EAGAIN if the
    // child fails execve after that drain and exits before the reap. The child
    // is gone here, so a blocking read only returns buffered errno or EOF.
    if (handle.exec_fd >= 0) {
        unsigned char buffer[sizeof(int)] = {0};
        ssize_t count = -1;
        do {
            count = ::read(handle.exec_fd, buffer, sizeof(buffer));
        } while (count < 0 && errno == EINTR);
        if (count == static_cast<ssize_t>(sizeof(int))) {
            int value = 0;
            std::memcpy(&value, buffer, sizeof(int));
            handle.exec_failed = true;
            handle.exec_error = "cannot start service: " + exec_error_message(value);
            runtime.last_error = handle.exec_error;
        }
        close_fd(handle.exec_fd);
    }
    runtime.pid = 0;
    handle.adopted = false;
    handle.cpu_available = false;
    handle.rss_available = false;
    handle.has_cpu_prev = false;
    runtime.last_exit_code = exit_code;
    runtime.last_signal = exit_signal;
    runtime.last_exit_at = now_unix_millis();
    if (handle.exec_failed && !handle.exec_error.empty()) {
        runtime.last_error = handle.exec_error;
    }

    if (runtime.state == ServiceState::stopping) {
        if (group_alive(handle)) {
            // The leader exited but descendants remain; the stop timer keeps
            // waiting and escalates to SIGKILL at the deadline.
            save(handle);
            return;
        }
        complete_stop(handle, now_ms);
        return;
    }
    const bool failed = exit_signal != 0 || exit_code != 0 || handle.exec_failed;
    if (handle.exec_failed && !handle.exec_error.empty()) {
        runtime.last_error = handle.exec_error;
    }
    apply_exit(handle, failed, exit_code, exit_signal, now_ms);
}

void SupervisorEngine::handle_adopted_gone(ServiceHandle& handle, std::int64_t now_ms) {
    auto& runtime = handle.record.runtime;
    runtime.pid = 0;
    handle.adopted = false;
    runtime.last_exit_code = -1;
    runtime.last_signal = 0;
    runtime.last_exit_at = now_unix_millis();
    runtime.last_error = "adopted process exited; exit status unavailable";
    apply_exit(handle, true, -1, 0, now_ms);
}

Error SupervisorEngine::save(ServiceHandle& handle) {
    if (options_.persistence_fault && options_.persistence_fault()) {
        ++persistence_errors_;
        mark_persistence_failure(handle, "injected runtime persistence failure for " +
                                             handle.record.config.id);
        return Error::make(ErrorCode::io_error,
                           "injected runtime persistence failure for " +
                               handle.record.config.id);
    }
    const Error error = store_->save_runtime(handle.record.config.id, handle.record.runtime);
    if (!error.ok()) {
        ++persistence_errors_;
        handle.record.runtime.last_error =
            "cannot persist service state: " + error.message;
        mark_persistence_failure(handle, "cannot persist service " + handle.record.config.id +
                                             ": " + error.message);
        std::fprintf(stderr, "edgedesk-supervisor: cannot persist service %s: %s\n",
                     handle.record.config.id.c_str(), error.message.c_str());
    }
    return error;
}

void SupervisorEngine::close_fd(int& fd) {
    if (fd >= 0) {
        (void)::close(fd);
        fd = -1;
    }
}

void SupervisorEngine::drain_log(ServiceHandle& handle, std::size_t budget) {
    if (handle.log_fd < 0) return;
    char buffer[kLogChunk];
    std::size_t drained = 0;
    while (drained < budget) {
        const std::size_t want = std::min<std::size_t>(sizeof(buffer), budget - drained);
        const ssize_t count = ::read(handle.log_fd, buffer, want);
        if (count > 0) {
            drained += static_cast<std::size_t>(count);
            if (!handle.log_discarding) {
                const Error error = logs_->append(
                    handle.record.config.id, std::string_view(buffer, static_cast<std::size_t>(count)),
                    handle.record.config.log_max_bytes);
                if (!error.ok()) {
                    ++log_errors_;
                    handle.log_discarding = true;
                    std::fprintf(stderr,
                                 "edgedesk-supervisor: cannot write log for %s: %s "
                                 "(discarding further output)\n",
                                 handle.record.config.id.c_str(), error.message.c_str());
                }
            }
            continue;
        }
        if (count == 0) {
            close_fd(handle.log_fd);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        ++log_errors_;
        std::fprintf(stderr, "edgedesk-supervisor: log read failed for %s: %s\n",
                     handle.record.config.id.c_str(), std::strerror(errno));
        close_fd(handle.log_fd);
        return;
    }
}

void SupervisorEngine::drain_exec(ServiceHandle& handle) {
    if (handle.exec_fd < 0) return;
    unsigned char buffer[4] = {0, 0, 0, 0};
    for (;;) {
        const ssize_t count = ::read(handle.exec_fd, buffer, sizeof(buffer));
        if (count == 0) {
            close_fd(handle.exec_fd);
            if (!handle.exec_failed && handle.record.runtime.state == ServiceState::starting) {
                handle.record.runtime.state = ServiceState::running;
                handle.exec_confirmed = true;
                handle.record.runtime.last_error.clear();
                handle.stable_deadline_ms =
                    steady_millis() +
                    static_cast<std::int64_t>(handle.record.config.stable_after_sec) * 1000;
                save(handle);
            }
            return;
        }
        if (count > 0) {
            int value = 0;
            std::memcpy(&value, buffer, static_cast<std::size_t>(count));
            handle.exec_failed = true;
            handle.exec_confirmed = false;
            handle.exec_error = "cannot start service: " + std::string(std::strerror(value));
            handle.record.runtime.last_error = handle.exec_error;
            std::fprintf(stderr, "edgedesk-supervisor: %s: %s\n",
                         handle.record.config.id.c_str(), handle.exec_error.c_str());
            save(handle);
            close_fd(handle.exec_fd);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        close_fd(handle.exec_fd);
        return;
    }
}

void SupervisorEngine::update_metrics(ServiceHandle& handle, std::int64_t now_ms) {
    handle.next_metrics_ms = now_ms + options_.metrics_interval_ms;
    const auto& runtime = handle.record.runtime;
    if (runtime.pid <= 0 || runtime.start_ticks == 0) {
        handle.cpu_available = false;
        handle.rss_available = false;
        handle.has_cpu_prev = false;
        return;
    }
    ProcStat stat;
    if (!read_proc_stat(runtime.pid, stat) || stat.start_ticks != runtime.start_ticks) {
        if (runtime.state != ServiceState::running && runtime.state != ServiceState::starting) {
            return;
        }
        // The process is gone or the PID was reused. Keep the state change to
        // the child/waitpid or adopted liveness path, not a metrics sample.
        return;
    }
    handle.rss_available = true;
    handle.rss_bytes = stat.rss_pages * page_size_bytes();
    const std::uint64_t ticks = stat.utime_ticks + stat.stime_ticks;
    if (handle.has_cpu_prev && now_ms > handle.last_cpu_sample_ms && ticks >= handle.last_cpu_ticks) {
        const double seconds =
            static_cast<double>(now_ms - handle.last_cpu_sample_ms) / 1000.0;
        const double cpu_seconds =
            static_cast<double>(ticks - handle.last_cpu_ticks) /
            static_cast<double>(std::max<long>(1, clock_ticks_per_second()));
        handle.cpu_percent = seconds > 0.0 ? (cpu_seconds / seconds) * 100.0 : 0.0;
        handle.cpu_available = true;
    }
    handle.last_cpu_ticks = ticks;
    handle.last_cpu_sample_ms = now_ms;
    handle.has_cpu_prev = true;
}

nlohmann::json SupervisorEngine::operation_json(const ServiceOperation& operation) const {
    nlohmann::json body;
    body["operation_id"] = operation.id;
    body["service_id"] = operation.service_id;
    body["kind"] = operation.kind;
    body["state"] = operation.state;
    body["result"] = operation.result;
    if (operation.state == "failed") {
        body["error"] = {{"slug", operation.slug}, {"message", operation.error.message}};
    } else {
        body["error"] = nullptr;
    }
    body["created_at"] = operation.created_at / 1000;
    body["finished_at"] =
        operation.finished_at > 0 ? nlohmann::json(operation.finished_at / 1000)
                                  : nlohmann::json(nullptr);
    return body;
}

RpcResult SupervisorEngine::handle_operation(std::string_view operation_id) const {
    const auto found = operations_.find(std::string(operation_id));
    if (found == operations_.end()) {
        return failure(ErrorCode::not_found, "operation_not_found",
                       "operation not found: " + std::string(operation_id));
    }
    return success(operation_json(found->second));
}

void SupervisorEngine::finish_operation(ServiceOperation& operation, bool ok, std::string slug,
                                        std::string message, nlohmann::json result) {
    operation.state = ok ? "succeeded" : "failed";
    operation.slug = ok ? std::string() : std::move(slug);
    operation.error = ok ? Error::none()
                         : Error::make(ErrorCode::unavailable, std::move(message));
    operation.result = std::move(result);
    operation.finished_at = now_unix_millis();
}

SupervisorEngine::ServiceOperation* SupervisorEngine::pending_operation(
    std::string_view service_id, std::string_view kind) {
    for (auto& pending : pending_) {
        if (pending.service_id != service_id) continue;
        const auto found = operations_.find(pending.operation_id);
        if (found == operations_.end()) continue;
        if (found->second.state != "pending") continue;
        if (found->second.kind != kind) continue;
        return &found->second;
    }
    return nullptr;
}

std::string SupervisorEngine::begin_operation(ServiceHandle& handle, std::string kind,
                                              bool restart_after, std::int64_t now_ms,
                                              RpcResult& error) {
    const std::string service_id = handle.record.config.id;
    if (ServiceOperation* existing = pending_operation(service_id, kind)) {
        error = success(nlohmann::json::object());
        return existing->id;
    }
    if (kind == "stop") {
        // A manual stop supersedes a pending restart. The stopped service is
        // never started again by the superseded operation.
        handle.restart_after_stop = false;
        for (auto it = pending_.begin(); it != pending_.end();) {
            const auto found = operations_.find(it->operation_id);
            if (it->service_id == service_id && found != operations_.end() &&
                found->second.state == "pending" && found->second.kind == "restart") {
                finish_operation(found->second, false, "operation_superseded",
                                 "restart superseded by a stop request", nullptr);
                it = pending_.erase(it);
                continue;
            }
            ++it;
        }
    }

    ServiceOperation operation;
    operation.id = "op-" + std::to_string(++operation_sequence_) + "-" +
                   std::to_string(now_unix_millis());
    operation.service_id = service_id;
    operation.kind = kind;
    operation.created_at = now_unix_millis();
    operation.deadline_ms =
        now_ms + handle.record.config.stop_timeout_ms + kStopKillGraceMs +
        (restart_after ? 10000 : 5000);
    const std::string id = operation.id;
    operations_.emplace(id, std::move(operation));
    operation_order_.push_back(id);
    pending_.push_back(PendingOperation{id, service_id, restart_after,
                                        operations_.at(id).deadline_ms});
    prune_operations();
    error = success(nlohmann::json::object());
    return id;
}

std::string SupervisorEngine::record_completed_operation(ServiceHandle& handle,
                                                         std::string kind,
                                                         std::int64_t now_ms, bool ok,
                                                         std::string slug,
                                                         std::string message) {
    ServiceOperation operation;
    operation.id = "op-" + std::to_string(++operation_sequence_) + "-" +
                   std::to_string(now_unix_millis());
    operation.service_id = handle.record.config.id;
    operation.kind = std::move(kind);
    operation.created_at = now_unix_millis();
    finish_operation(operation, ok, std::move(slug), std::move(message),
                     ok ? record_json(handle, now_ms) : nlohmann::json(nullptr));
    const std::string id = operation.id;
    operations_.emplace(id, std::move(operation));
    operation_order_.push_back(id);
    prune_operations(id);
    return id;
}

void SupervisorEngine::prune_operations(std::string_view keep) {
    // Completed operations are evicted oldest-first even when an older pending
    // operation sits at the front of the queue. The operation created by the
    // current call is protected: removing it would make the caller's
    // operations_.at(id) throw and terminate the daemon when all older entries
    // are pending.
    if (operations_.size() <= kMaxRetainedOperations) {
        operation_order_.erase(
            std::remove_if(operation_order_.begin(), operation_order_.end(),
                           [this](const std::string& id) {
                               return operations_.find(id) == operations_.end();
                           }),
            operation_order_.end());
        return;
    }
    for (auto it = operation_order_.begin();
         it != operation_order_.end() && operations_.size() > kMaxRetainedOperations;) {
        const auto found = operations_.find(*it);
        if (found == operations_.end()) {
            it = operation_order_.erase(it);
            continue;
        }
        if (found->second.state == "pending" || *it == keep) {
            ++it;
            continue;
        }
        operations_.erase(found);
        it = operation_order_.erase(it);
    }
}

void SupervisorEngine::complete_pending(std::string_view id, std::int64_t now_ms,
                                        bool restarted) {
    ServiceHandle* handle = find(id);
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (std::string_view(it->service_id) != id) {
            ++it;
            continue;
        }
        const auto found = operations_.find(it->operation_id);
        if (found == operations_.end() || found->second.state != "pending") {
            it = pending_.erase(it);
            continue;
        }
        ServiceOperation& operation = found->second;
        if (it->restart_after) {
            const bool ok =
                restarted && handle != nullptr &&
                (handle->record.runtime.state == ServiceState::starting ||
                 handle->record.runtime.state == ServiceState::running);
            if (ok) {
                finish_operation(operation, true, {}, {}, record_json(*handle, now_ms));
            } else {
                const std::string detail =
                    handle != nullptr && !handle->record.runtime.last_error.empty()
                        ? handle->record.runtime.last_error
                        : "service could not be restarted";
                finish_operation(operation, false, "service_restart_failed", detail, nullptr);
            }
        } else {
            finish_operation(operation, true, {}, {},
                             handle != nullptr ? record_json(*handle, now_ms)
                                               : nlohmann::json(nullptr));
        }
        it = pending_.erase(it);
    }
}

void SupervisorEngine::fail_pending(std::string_view id, std::string slug,
                                    std::string message) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (std::string_view(it->service_id) != id) {
            ++it;
            continue;
        }
        const auto found = operations_.find(it->operation_id);
        if (found != operations_.end() && found->second.state == "pending") {
            finish_operation(found->second, false, slug, message, nullptr);
        }
        it = pending_.erase(it);
    }
}

bool SupervisorEngine::unverified_process_may_exist(const ServiceHandle& handle) const {
    const auto& runtime = handle.record.runtime;
    if (runtime.pid > 0) {
        ProcStat stat;
        if (read_proc_stat(runtime.pid, stat) && stat.state != 'Z') return true;
    }
    if (runtime.pgid > 0) {
        // No verifiable identity is available here, so any member of that group
        // blocks recovery. Refusing is safer than signalling or starting a
        // duplicate.
        if (process_group_has_any_members(runtime.pgid)) return true;
    }
    return false;
}

bool SupervisorEngine::leader_alive(const ServiceHandle& handle) const {
    const auto& runtime = handle.record.runtime;
    if (runtime.pid <= 0 || runtime.start_ticks == 0) return false;
    ProcStat stat;
    if (!read_proc_stat(runtime.pid, stat)) return false;
    return stat.start_ticks == runtime.start_ticks && stat.state != 'Z';
}

GroupProbe SupervisorEngine::probe_group(const ServiceHandle& handle) const {
    const auto& runtime = handle.record.runtime;
    if (runtime.pgid <= 0 || runtime.start_ticks == 0) {
        GroupProbe probe;
        probe.pgid = runtime.pgid;
        probe.leader_pid = runtime.pid;
        probe.state = GroupState::absent;
        probe.detail = "no recorded process group identity";
        return probe;
    }
    const std::string& boot_id = runtime.boot_id.empty() ? boot_id_ : runtime.boot_id;
    return probe_service_group(runtime.pgid, runtime.pid, runtime.start_ticks, boot_id, boot_id_,
                               runtime.anchor_pid, runtime.anchor_start_ticks);
}

bool SupervisorEngine::group_alive(const ServiceHandle& handle) const {
    // Verified members, an unverifiable group and a leftover sentinel all mean
    // "something may still exist"; callers that intend to signal must use
    // probe_group() and honor the state.
    return probe_group(handle).state != GroupState::absent;
}

bool SupervisorEngine::group_verifiable(const ServiceHandle& handle) const {
    return probe_group(handle).state == GroupState::verified;
}

bool SupervisorEngine::group_uncertain(const ServiceHandle& handle) const {
    return probe_group(handle).state == GroupState::uncertain;
}

bool SupervisorEngine::group_anchor_only(const ServiceHandle& handle) const {
    return probe_group(handle).state == GroupState::anchor_only;
}

void SupervisorEngine::clear_identity(ServiceHandle& handle) {
    auto& runtime = handle.record.runtime;
    if (runtime.anchor_pid > 1) {
        // The sentinel may still be a zombie waiting for poll(); remember it so
        // the reap does not look like an unknown child. The set is bounded.
        retired_anchor_pids_.insert(runtime.anchor_pid);
        if (retired_anchor_pids_.size() > 4096) retired_anchor_pids_.clear();
    }
    runtime.pid = 0;
    runtime.pgid = 0;
    runtime.start_ticks = 0;
    runtime.started_at_ms = 0;
    runtime.anchor_pid = 0;
    runtime.anchor_start_ticks = 0;
}

void SupervisorEngine::remove_exec_staging(ServiceHandle& handle) {
    if (!handle.exec_staging_link.empty()) {
        (void)::unlink(handle.exec_staging_link.c_str());
        handle.exec_staging_link.clear();
    }
    if (!handle.exec_staging_directory.empty()) {
        (void)::rmdir(handle.exec_staging_directory.c_str());
        handle.exec_staging_directory.clear();
    }
}

void SupervisorEngine::mark_persistence_failure(ServiceHandle& handle,
                                                std::string_view context) {
    handle.persistence_degraded = true;
    // No automatic replacement instance may start from a runtime record that
    // may not have been written (PSH-04).
    handle.pending_auto_restart = false;
    handle.cleanup_deadline_ms = 0;
    handle.backoff_deadline_ms = 0;
    (void)store_->mark_inconsistent(context);
}

void SupervisorEngine::handle_anchor_exit(ServiceHandle& handle, std::int64_t now_ms) {
    (void)now_ms;
    auto& runtime = handle.record.runtime;
    const ServiceState state = runtime.state;
    runtime.anchor_pid = 0;
    runtime.anchor_start_ticks = 0;
    if (state == ServiceState::stopping || state == ServiceState::backoff ||
        shutting_down_) {
        // Expected: group cleanup killed the sentinel together with the group.
        (void)save(handle);
        return;
    }
    if (state == ServiceState::running || state == ServiceState::starting) {
        runtime.state = ServiceState::unknown;
        runtime.desired_state = "stopped";
        runtime.last_error =
            "service identity anchor exited unexpectedly; the process group can no longer "
            "be proven safe";
        runtime.failed_locked = false;
        handle.pending_auto_restart = false;
        handle.cleanup_deadline_ms = 0;
        handle.backoff_deadline_ms = 0;
        (void)save(handle);
        fail_pending(handle.record.config.id, "service_unknown", runtime.last_error);
        return;
    }
    (void)save(handle);
}

void SupervisorEngine::run_timers(std::int64_t now_ms) {
    for (auto& item : services_) {
        ServiceHandle& handle = item.second;
        auto& runtime = handle.record.runtime;

        if (!shutting_down_ && handle.adopted &&
            (runtime.state == ServiceState::running || runtime.state == ServiceState::starting)) {
            if (now_ms >= handle.last_adopt_check_ms + options_.adopted_poll_interval_ms) {
                handle.last_adopt_check_ms = now_ms;
                if (!leader_alive(handle)) handle_adopted_gone(handle, now_ms);
            }
        }

        if (runtime.state == ServiceState::backoff) {
            if (shutting_down_) {
                const GroupProbe probe = probe_group(handle);
                if (probe.state == GroupState::anchor_only) {
                    std::string signal_error;
                    (void)signal_service_group(probe, SIGKILL, signal_error);
                }
                if (probe_group(handle).state == GroupState::absent) {
                    runtime.state = ServiceState::stopped;
                    clear_identity(handle);
                    (void)save(handle);
                }
                continue;
            }
            // A stop intent or a runtime persistence failure must not re-arm the
            // backoff cleanup on every poll. Move to an explicit cleanup or
            // terminal state instead; a failing save resets the backoff
            // deadlines, which would otherwise make poll_timeout_ms return 0
            // and spin the event loop.
            if (runtime.desired_state != "running" || handle.persistence_degraded) {
                const GroupProbe probe = probe_group(handle);
                handle.pending_auto_restart = false;
                handle.cleanup_deadline_ms = 0;
                handle.backoff_deadline_ms = 0;
                if (probe.state == GroupState::verified) {
                    runtime.state = ServiceState::stopping;
                    handle.restart_after_stop = false;
                    handle.stop_kill_sent = false;
                    handle.stop_term_deadline_ms =
                        now_ms + handle.record.config.stop_timeout_ms;
                    handle.stop_verify_deadline_ms =
                        handle.stop_term_deadline_ms + kStopKillGraceMs;
                    std::string signal_error;
                    (void)signal_service_group(probe, SIGTERM, signal_error);
                    (void)save(handle);
                } else if (probe.state == GroupState::anchor_only) {
                    runtime.state = ServiceState::stopping;
                    handle.restart_after_stop = false;
                    handle.stop_kill_sent = false;
                    handle.stop_term_deadline_ms = now_ms;
                    handle.stop_verify_deadline_ms = now_ms + kStopKillGraceMs;
                    (void)save(handle);
                } else if (probe.state == GroupState::absent) {
                    runtime.state = ServiceState::stopped;
                    clear_identity(handle);
                    remove_exec_staging(handle);
                    (void)save(handle);
                } else {
                    runtime.state = ServiceState::unknown;
                    runtime.desired_state = "stopped";
                    runtime.last_error =
                        "cannot verify the process group while disabling restart: " +
                        probe.detail;
                    (void)save(handle);
                    fail_pending(item.first, "service_unknown", runtime.last_error);
                }
                continue;
            }
            if (handle.pending_auto_restart) {
                const GroupProbe probe = probe_group(handle);
                if (probe.state == GroupState::uncertain) {
                    runtime.state = ServiceState::unknown;
                    runtime.desired_state = "stopped";
                    runtime.last_error =
                        "cannot verify surviving descendants; refusing to restart: " +
                        probe.detail;
                    handle.pending_auto_restart = false;
                    handle.cleanup_deadline_ms = 0;
                    (void)save(handle);
                    fail_pending(item.first, "service_unknown", runtime.last_error);
                } else if (probe.state == GroupState::absent) {
                    handle.pending_auto_restart = false;
                    handle.cleanup_kill_sent = false;
                    handle.cleanup_deadline_ms = 0;
                    clear_identity(handle);
                    remove_exec_staging(handle);
                    schedule_restart(handle, now_ms);
                } else if (probe.state == GroupState::anchor_only) {
                    // Only the sentinel remains; reap it now instead of waiting
                    // for the stop timeout.
                    std::string signal_error;
                    if (!signal_service_group(probe, SIGKILL, signal_error)) {
                        runtime.state = ServiceState::unknown;
                        runtime.desired_state = "stopped";
                        runtime.last_error = "cannot reap service identity anchor: " +
                                             signal_error;
                        handle.pending_auto_restart = false;
                        (void)save(handle);
                        fail_pending(item.first, "service_unknown", runtime.last_error);
                    } else {
                        handle.cleanup_kill_sent = true;
                        handle.cleanup_deadline_ms = now_ms + kStopKillGraceMs;
                    }
                } else if (now_ms >= handle.cleanup_deadline_ms) {
                    if (!handle.cleanup_kill_sent) {
                        std::string signal_error;
                        (void)signal_service_group(probe, SIGKILL, signal_error);
                        handle.cleanup_kill_sent = true;
                        handle.cleanup_deadline_ms = now_ms + kStopKillGraceMs;
                    } else {
                        runtime.state = ServiceState::unknown;
                        runtime.desired_state = "stopped";
                        runtime.last_error =
                            "process group did not exit after SIGKILL; refusing to duplicate";
                        handle.pending_auto_restart = false;
                        (void)save(handle);
                        fail_pending(item.first, "service_unknown", runtime.last_error);
                    }
                }
            } else if (now_ms >= handle.backoff_deadline_ms) {
                const GroupProbe probe = probe_group(handle);
                if (probe.state == GroupState::uncertain) {
                    runtime.state = ServiceState::unknown;
                    runtime.desired_state = "stopped";
                    runtime.last_error =
                        "cannot verify process group before restart: " + probe.detail;
                    (void)save(handle);
                    fail_pending(item.first, "service_unknown", runtime.last_error);
                } else if (probe.state == GroupState::verified || probe.state == GroupState::anchor_only) {
                    runtime.state = ServiceState::backoff;
                    handle.pending_auto_restart = true;
                    handle.cleanup_kill_sent = false;
                    handle.cleanup_deadline_ms = now_ms;
                    std::string signal_error;
                    if (probe.state == GroupState::anchor_only) {
                        (void)signal_service_group(probe, SIGKILL, signal_error);
                        handle.cleanup_kill_sent = true;
                        handle.cleanup_deadline_ms = now_ms + kStopKillGraceMs;
                    } else {
                        (void)signal_service_group(probe, SIGTERM, signal_error);
                        handle.cleanup_deadline_ms =
                            now_ms + handle.record.config.stop_timeout_ms;
                    }
                    (void)save(handle);
                } else {
                    if (handle.persistence_degraded) {
                        runtime.state = ServiceState::failed;
                        runtime.desired_state = "stopped";
                        runtime.last_error =
                            "automatic restart disabled after a runtime persistence failure";
                        (void)save(handle);
                        fail_pending(item.first, "supervisor_unavailable",
                                     runtime.last_error);
                    } else {
                        const Error error = spawn_locked(handle, now_ms);
                        if (!error.ok()) {
                            runtime.state = ServiceState::failed;
                            runtime.desired_state = "stopped";
                            runtime.last_error = error.message;
                            fail_pending(item.first, "service_restart_failed",
                                         runtime.last_error);
                        }
                        (void)save(handle);
                    }
                }
            }
            continue;
        }

        if (runtime.state == ServiceState::stopping) {
            const GroupProbe probe = probe_group(handle);
            if (probe.state == GroupState::uncertain) {
                runtime.state = ServiceState::unknown;
                runtime.desired_state = "stopped";
                runtime.last_error =
                    "cannot verify process group while stopping: " + probe.detail;
                (void)save(handle);
                fail_pending(item.first, "service_unknown", runtime.last_error);
                continue;
            }
            if (probe.state == GroupState::absent) {
                complete_stop(handle, now_ms);
                continue;
            }
            if (probe.state == GroupState::anchor_only) {
                // The workload is gone; reap the sentinel now rather than
                // waiting for the SIGTERM grace period.
                if (!handle.stop_kill_sent) {
                    std::string signal_error;
                    if (!signal_service_group(probe, SIGKILL, signal_error)) {
                        const GroupProbe after = probe_group(handle);
                        if (after.state == GroupState::absent) {
                            complete_stop(handle, now_ms);
                            continue;
                        }
                        runtime.state = ServiceState::unknown;
                        runtime.desired_state = "stopped";
                        runtime.last_error = "cannot reap process group sentinel: " + signal_error;
                        (void)save(handle);
                        fail_pending(item.first, "service_unknown", runtime.last_error);
                        continue;
                    }
                    handle.stop_kill_sent = true;
                }
                if (now_ms >= handle.stop_verify_deadline_ms) {
                    runtime.state = ServiceState::unknown;
                    runtime.desired_state = "stopped";
                    runtime.last_error = "identity anchor did not exit after SIGKILL";
                    (void)save(handle);
                    fail_pending(item.first, "service_unknown", runtime.last_error);
                }
                continue;
            }
            if (!handle.stop_kill_sent && now_ms >= handle.stop_term_deadline_ms) {
                std::string signal_error;
                if (!signal_service_group(probe, SIGKILL, signal_error)) {
                    const GroupProbe after = probe_group(handle);
                    if (after.state == GroupState::absent) {
                        complete_stop(handle, now_ms);
                        continue;
                    }
                    runtime.state = ServiceState::unknown;
                    runtime.desired_state = "stopped";
                    runtime.last_error = "cannot kill process group: " + signal_error;
                    (void)save(handle);
                    fail_pending(item.first, "service_unknown", runtime.last_error);
                    continue;
                }
                handle.stop_kill_sent = true;
            }
            if (now_ms >= handle.stop_verify_deadline_ms) {
                runtime.state = ServiceState::unknown;
                runtime.desired_state = "stopped";
                runtime.last_error = "process group did not exit after SIGKILL";
                (void)save(handle);
                fail_pending(item.first, "service_unknown", runtime.last_error);
            }
            continue;
        }

        if (runtime.state == ServiceState::unknown) {
            // Never guess: the identity could not be verified. An operator can
            // clear UNKNOWN through stop/start only after the process and
            // process group are confirmed absent.
            continue;
        }

        if (runtime.state == ServiceState::running) {
            if (handle.stable_deadline_ms > 0 && now_ms >= handle.stable_deadline_ms) {
                runtime.window_restarts = 0;
                runtime.window_started_at = now_ms;
                handle.stable_deadline_ms = 0;
                (void)save(handle);
            }
            if (runtime.pid > 0 && now_ms >= handle.next_metrics_ms) {
                update_metrics(handle, now_ms);
            }
        } else if (runtime.state == ServiceState::starting) {
            if (runtime.pid > 0 && now_ms >= handle.next_metrics_ms) {
                update_metrics(handle, now_ms);
            }
        }
    }

    // Safety net: an operation whose completion path never ran must not stay
    // pending forever. The service state is left untouched; the operator can
    // query it separately.
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now_ms < it->deadline_ms) {
            ++it;
            continue;
        }
        const auto found = operations_.find(it->operation_id);
        if (found != operations_.end() && found->second.state == "pending") {
            finish_operation(found->second, false, "operation_timeout",
                             "supervisor operation timed out", nullptr);
        }
        it = pending_.erase(it);
    }
    prune_operations();
}

std::vector<RpcCompletion> SupervisorEngine::poll(std::int64_t now_ms) {
    for (auto& item : services_) {
        drain_exec(item.second);
        drain_log(item.second, kLogDrainBudget);
    }

    for (;;) {
        int status = 0;
        const pid_t pid = ::waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            ServiceHandle* handle = nullptr;
            bool is_anchor = false;
            for (auto& item : services_) {
                if (item.second.record.runtime.pid == pid) {
                    handle = &item.second;
                    break;
                }
                if (item.second.record.runtime.anchor_pid == pid) {
                    handle = &item.second;
                    is_anchor = true;
                    break;
                }
            }
            if (handle != nullptr && is_anchor) {
                handle_anchor_exit(*handle, now_ms);
            } else if (handle != nullptr) {
                handle_child_exit(*handle, status, now_ms);
            } else if (retired_anchor_pids_.erase(static_cast<int>(pid)) == 0) {
                std::fprintf(stderr,
                             "edgedesk-supervisor: reaped unknown child %d\n",
                             static_cast<int>(pid));
            }
            continue;
        }
        if (pid == 0) break;
        if (errno == EINTR) continue;
        break; // ECHILD: no more children (adopted processes are not ours).
    }

    run_timers(now_ms);

    // Stop/restart no longer complete asynchronously through the RPC ticket
    // mechanism; callers poll service.operation.get instead.
    return {};
}

int SupervisorEngine::poll_timeout_ms(std::int64_t now_ms) const {
    std::int64_t next = now_ms + kMaxPollSleepMs;
    bool pipe_open = false;
    for (const auto& item : services_) {
        const ServiceHandle& handle = item.second;
        const auto& runtime = handle.record.runtime;
        if (handle.log_fd >= 0 || handle.exec_fd >= 0) pipe_open = true;
        if (runtime.state == ServiceState::backoff) {
            const std::int64_t deadline = handle.pending_auto_restart
                                              ? handle.cleanup_deadline_ms
                                              : handle.backoff_deadline_ms;
            if (deadline > 0) next = std::min(next, deadline);
        }
        if (runtime.state == ServiceState::stopping) {
            if (!handle.stop_kill_sent) next = std::min(next, handle.stop_term_deadline_ms);
            next = std::min(next, handle.stop_verify_deadline_ms);
        }
        if (runtime.state == ServiceState::running ||
            runtime.state == ServiceState::starting) {
            if (runtime.pid > 0) next = std::min(next, handle.next_metrics_ms);
            if (handle.adopted) {
                next = std::min(next, handle.last_adopt_check_ms +
                                          options_.adopted_poll_interval_ms);
            }
        }
        if (runtime.state == ServiceState::unknown) {
            next = std::min(next, now_ms + options_.adopted_poll_interval_ms);
        }
        if (handle.stable_deadline_ms > 0) next = std::min(next, handle.stable_deadline_ms);
    }
    for (const auto& pending : pending_) {
        next = std::min(next, pending.deadline_ms);
    }
    if (pipe_open) next = std::min(next, now_ms + 250);
    if (next <= now_ms) return 0;
    return static_cast<int>(std::min<std::int64_t>(next - now_ms, kMaxPollSleepMs));
}

void SupervisorEngine::shutdown(std::int64_t now_ms, int grace_ms) {
    shutting_down_ = true;
    const std::int64_t deadline = now_ms + std::max(0, grace_ms);
    for (auto& item : services_) {
        ServiceHandle& handle = item.second;
        auto& runtime = handle.record.runtime;
        if (runtime.state == ServiceState::stopping || runtime.state == ServiceState::unknown) {
            continue;
        }
        const GroupProbe probe = probe_group(handle);
        if (probe.state == GroupState::verified) {
            runtime.state = ServiceState::stopping;
            handle.restart_after_stop = false;
            handle.stop_kill_sent = false;
            handle.stop_term_deadline_ms = deadline;
            handle.stop_verify_deadline_ms = deadline + kStopKillGraceMs;
            handle.pending_auto_restart = false;
            handle.cleanup_deadline_ms = 0;
            std::string signal_error;
            (void)signal_service_group(probe, SIGTERM, signal_error);
            (void)save(handle);
        } else if (probe.state == GroupState::anchor_only) {
            runtime.state = ServiceState::stopping;
            handle.restart_after_stop = false;
            handle.stop_term_deadline_ms = deadline;
            handle.stop_verify_deadline_ms = deadline + kStopKillGraceMs;
            handle.pending_auto_restart = false;
            handle.cleanup_deadline_ms = 0;
            std::string signal_error;
            handle.stop_kill_sent = signal_service_group(probe, SIGKILL, signal_error);
            (void)save(handle);
        } else if (probe.state == GroupState::uncertain) {
            runtime.state = ServiceState::unknown;
            runtime.last_error =
                "cannot verify process group during shutdown: " + probe.detail;
            (void)save(handle);
        } else if (runtime.state == ServiceState::running ||
                   runtime.state == ServiceState::starting ||
                   runtime.state == ServiceState::backoff) {
            runtime.state = ServiceState::stopped;
            clear_identity(handle);
            (void)save(handle);
        }
    }

    for (;;) {
        (void)poll(steady_millis());
        bool alive = false;
        for (const auto& item : services_) {
            if (item.second.record.runtime.state == ServiceState::stopping) {
                const GroupState state = probe_group(item.second).state;
                if (state == GroupState::verified || state == GroupState::anchor_only) {
                    alive = true;
                    break;
                }
            }
        }
        if (!alive) break;
        const std::int64_t now = steady_millis();
        if (now >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    for (auto& item : services_) {
        ServiceHandle& handle = item.second;
        auto& runtime = handle.record.runtime;
        if (runtime.state != ServiceState::stopping) continue;
        const GroupProbe probe = probe_group(handle);
        if (probe.state == GroupState::verified ||
            probe.state == GroupState::anchor_only) {
            std::string signal_error;
            (void)signal_service_group(probe, SIGKILL, signal_error);
        }
    }
    const std::int64_t kill_deadline = steady_millis() + kStopKillGraceMs;
    while (steady_millis() < kill_deadline) {
        (void)poll(steady_millis());
        bool alive = false;
        for (const auto& item : services_) {
            if (item.second.record.runtime.state == ServiceState::stopping) {
                const GroupState state = probe_group(item.second).state;
                if (state == GroupState::verified || state == GroupState::anchor_only) {
                    alive = true;
                    break;
                }
            }
        }
        if (!alive) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    for (auto& item : services_) {
        ServiceHandle& handle = item.second;
        auto& runtime = handle.record.runtime;
        if (runtime.state == ServiceState::stopping) {
            const GroupProbe probe = probe_group(handle);
            if (probe.state == GroupState::absent) {
                runtime.state = ServiceState::stopped;
                clear_identity(handle);
            } else if (probe.state == GroupState::uncertain) {
                runtime.state = ServiceState::unknown;
                runtime.last_error =
                    "cannot verify process group after shutdown: " + probe.detail;
            }
        }
        close_fd(handle.log_fd);
        close_fd(handle.exec_fd);
        (void)save(handle);
    }
}

} // namespace edgedesk::supervisor
