#include "supervisor/supervisor_engine.hpp"

#include "core/util.hpp"
#include "supervisor/process_control.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
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

} // namespace

struct SupervisorEngine::ServiceHandle {
    ServiceRecord record;
    int log_fd = -1;
    int exec_fd = -1;
    bool exec_confirmed = false;
    bool exec_failed = false;
    std::string exec_error;
    bool adopted = false;
    bool log_discarding = false;

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
    std::uint64_t ticket = 0;
    std::string service_id;
    bool restart_after = false;
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

    std::vector<ServiceRecord> records;
    error = store_->load_all(records);
    if (!error.ok()) return error;

    const std::int64_t now = steady_millis();
    for (auto& record : records) {
        ServiceHandle handle;
        handle.record = std::move(record);
        auto& runtime = handle.record.runtime;
        if (runtime.desired_state != "running" && runtime.desired_state != "stopped") {
            runtime.desired_state = "stopped";
        }

        const bool same_boot = !runtime.boot_id.empty() && runtime.boot_id == boot_id_;
        bool start_expected = false;
        if (!same_boot) {
            // A new system boot clears boot-specific overrides. autostart now
            // decides whether the service should be running.
            runtime.boot_id = boot_id_;
            runtime.state = ServiceState::stopped;
            runtime.pid = 0;
            runtime.pgid = 0;
            runtime.start_ticks = 0;
            runtime.started_at_ms = 0;
            runtime.window_restarts = 0;
            runtime.window_started_at = 0;
            runtime.failed_locked = false;
            runtime.desired_state = handle.record.config.autostart ? "running" : "stopped";
            start_expected = handle.record.config.autostart;
        } else {
            runtime.boot_id = boot_id_;
            if (runtime.pid > 0 && runtime.start_ticks > 0 &&
                process_identity_matches(runtime.pid, boot_id_, runtime.start_ticks)) {
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
            } else if (runtime.pid > 0 && runtime.start_ticks == 0) {
                // A persisted PID without a verifiable start time could be a
                // reused PID. Never signal it or start a second copy.
                runtime.state = ServiceState::unknown;
                runtime.last_error =
                    "recovered PID has no verifiable start time; manual intervention required";
            } else if (runtime.pid > 0 && runtime.start_ticks > 0 &&
                       process_group_has_members(runtime.pgid, runtime.start_ticks)) {
                // Leadership cannot be verified while the group still exists;
                // never signal or start a duplicate.
                runtime.state = ServiceState::unknown;
                runtime.last_error =
                    "cannot verify recovered process identity; manual intervention required";
            } else if (runtime.pgid > 0 && runtime.start_ticks == 0) {
                runtime.state = ServiceState::unknown;
                runtime.last_error =
                    "recovered process group has no verifiable start time; manual intervention "
                    "required";
            } else if (runtime.pgid > 0 && runtime.start_ticks > 0 &&
                       process_group_has_members(runtime.pgid, runtime.start_ticks)) {
                runtime.state = ServiceState::unknown;
                runtime.last_error =
                    "cannot verify recovered process group; manual intervention required";
            } else {
                runtime.pid = 0;
                runtime.pgid = 0;
                runtime.start_ticks = 0;
                runtime.started_at_ms = 0;
                runtime.state = ServiceState::stopped;
                if (runtime.failed_locked) {
                    runtime.state = ServiceState::failed;
                } else if (runtime.desired_state == "running") {
                    start_expected = true;
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
    return Error::none();
}

SupervisorEngine::SupervisorEngine(EngineOptions options) : options_(std::move(options)) {}

SupervisorEngine::~SupervisorEngine() {
    // Service processes deliberately survive an abrupt Supervisor exit so a
    // replacement can adopt them; only our pipe descriptors are released.
    for (auto& item : services_) {
        close_fd(item.second.log_fd);
        close_fd(item.second.exec_fd);
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
        return handle_stop(id, ticket, now);
    }
    if (method == "service.restart") {
        std::string id;
        RpcResult error;
        if (!read_id(body, id, error)) return error;
        return handle_restart(id, ticket, now);
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
        handle->record.runtime.pid = 0;
        handle->record.runtime.pgid = 0;
        handle->record.runtime.start_ticks = 0;
        handle->record.runtime.started_at_ms = 0;
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
        runtime.state = ServiceState::stopped;
        runtime.desired_state = "stopped";
        runtime.last_error = error.message;
        save(*handle);
        return failure(error.code, config_error_slug(error), error.message);
    }
    save(*handle);
    return success(record_json(*handle, now_ms));
}

RpcResult SupervisorEngine::handle_stop(std::string_view id, std::uint64_t ticket,
                                        std::int64_t now_ms) {
    ServiceHandle* handle = find(id);
    if (handle == nullptr) {
        return failure(ErrorCode::not_found, "service_not_found",
                       "service not found: " + std::string(id));
    }
    const ServiceState state = handle->record.runtime.state;
    if (state == ServiceState::stopped || state == ServiceState::failed ||
        state == ServiceState::backoff) {
        handle->record.runtime.desired_state = "stopped";
        handle->record.runtime.failed_locked = false;
        handle->record.runtime.last_error.clear();
        handle->pending_auto_restart = false;
        handle->backoff_deadline_ms = 0;
        handle->cleanup_deadline_ms = 0;
        if (group_alive(*handle)) {
            // A previous leader exited but descendants are still alive. Stop
            // them now instead of forgetting the process group.
            initiate_stop(*handle, false, now_ms);
            save(*handle);
            if (handle->record.runtime.state != ServiceState::stopping) {
                return success(record_json(*handle, now_ms));
            }
            PendingOperation operation;
            operation.ticket = ticket;
            operation.service_id = std::string(id);
            operation.restart_after = false;
            operation.deadline_ms =
                now_ms + handle->record.config.stop_timeout_ms + kStopKillGraceMs + 5000;
            pending_.push_back(std::move(operation));
            RpcResult result = success(record_json(*handle, now_ms));
            result.async = true;
            result.async_timeout_ms = handle->record.config.stop_timeout_ms +
                                      static_cast<int>(kStopKillGraceMs) + 15000;
            return result;
        }
        handle->record.runtime.state = ServiceState::stopped;
        handle->record.runtime.pid = 0;
        handle->record.runtime.pgid = 0;
        handle->record.runtime.start_ticks = 0;
        handle->record.runtime.started_at_ms = 0;
        save(*handle);
        return success(record_json(*handle, now_ms));
    }
    if (state == ServiceState::unknown) {
        if (unverified_process_may_exist(*handle)) {
            return failure(ErrorCode::conflict, "service_unknown",
                           "service process identity is unknown; refusing to signal a reused PID");
        }
        handle->record.runtime.state = ServiceState::stopped;
        handle->record.runtime.desired_state = "stopped";
        handle->record.runtime.failed_locked = false;
        handle->record.runtime.pid = 0;
        handle->record.runtime.pgid = 0;
        handle->record.runtime.start_ticks = 0;
        handle->record.runtime.started_at_ms = 0;
        handle->record.runtime.last_error.clear();
        save(*handle);
        return success(record_json(*handle, now_ms));
    }
    if (state == ServiceState::running || state == ServiceState::starting ||
        state == ServiceState::stopping) {
        handle->record.runtime.desired_state = "stopped";
        if (state == ServiceState::stopping) {
            // A manual stop wins over a pending restart.
            handle->restart_after_stop = false;
        }
        initiate_stop(*handle, false, now_ms);
        save(*handle);
        if (handle->record.runtime.state != ServiceState::stopping) {
            return success(record_json(*handle, now_ms));
        }
        PendingOperation operation;
        operation.ticket = ticket;
        operation.service_id = std::string(id);
        operation.restart_after = false;
        operation.deadline_ms =
            now_ms + handle->record.config.stop_timeout_ms + kStopKillGraceMs + 5000;
        pending_.push_back(std::move(operation));
        RpcResult result = success(record_json(*handle, now_ms));
        result.async = true;
        result.async_timeout_ms =
            handle->record.config.stop_timeout_ms + static_cast<int>(kStopKillGraceMs) + 15000;
        return result;
    }
    return failure(ErrorCode::conflict, "service_state",
                   "service cannot be stopped from its current state");
}

RpcResult SupervisorEngine::handle_restart(std::string_view id, std::uint64_t ticket,
                                           std::int64_t now_ms) {
    ServiceHandle* handle = find(id);
    if (handle == nullptr) {
        return failure(ErrorCode::not_found, "service_not_found",
                       "service not found: " + std::string(id));
    }
    const ServiceState state = handle->record.runtime.state;
    if (state == ServiceState::unknown) {
        if (unverified_process_may_exist(*handle)) {
            return failure(ErrorCode::conflict, "service_unknown",
                           "service process identity is unknown; refusing to start a duplicate");
        }
        handle->record.runtime.pid = 0;
        handle->record.runtime.pgid = 0;
        handle->record.runtime.start_ticks = 0;
        handle->record.runtime.started_at_ms = 0;
    } else if (state != ServiceState::running && state != ServiceState::starting &&
               group_alive(*handle)) {
        return failure(ErrorCode::conflict, "service_running",
                       "a process from the previous run is still alive");
    }
    if (state == ServiceState::stopping) {
        return failure(ErrorCode::conflict, "service_stopping",
                       "service is stopping; start it again after the stop completes");
    }
    if (state == ServiceState::running || state == ServiceState::starting) {
        handle->record.runtime.desired_state = "running";
        initiate_stop(*handle, true, now_ms);
        save(*handle);
        if (handle->record.runtime.state != ServiceState::stopping) {
            return success(record_json(*handle, now_ms));
        }
        PendingOperation operation;
        operation.ticket = ticket;
        operation.service_id = std::string(id);
        operation.restart_after = true;
        operation.deadline_ms =
            now_ms + handle->record.config.stop_timeout_ms + kStopKillGraceMs + 10000;
        pending_.push_back(std::move(operation));
        RpcResult result = success(record_json(*handle, now_ms));
        result.async = true;
        result.async_timeout_ms =
            handle->record.config.stop_timeout_ms + static_cast<int>(kStopKillGraceMs) + 20000;
        return result;
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
    Error error = spawn_locked(*handle, now_ms);
    if (!error.ok()) {
        runtime.state = ServiceState::stopped;
        runtime.desired_state = "stopped";
        runtime.last_error = error.message;
        save(*handle);
        return failure(error.code, config_error_slug(error), error.message);
    }
    save(*handle);
    return success(record_json(*handle, now_ms));
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

Error SupervisorEngine::spawn_locked(ServiceHandle& handle, std::int64_t now_ms) {
    ResolvedIdentity identity;
    Error error = resolve_identity(handle.record.config.run_user,
                                   handle.record.config.run_group, identity);
    if (!error.ok()) return error;
    if (identity.is_root && !handle.record.config.allow_root) {
        return Error::make(ErrorCode::permission_denied,
                           "running as root requires explicit confirmation (allow_root=true)");
    }
    SpawnOutcome outcome = spawn_service(handle.record.config, identity);
    if (!outcome.error.ok()) return outcome.error;
    if (outcome.pid <= 0) {
        close_fd(outcome.log_read_fd);
        close_fd(outcome.exec_read_fd);
        return Error::make(ErrorCode::io_error, "spawn returned no child PID");
    }
    if (outcome.start_ticks == 0) {
        // Without a kernel start time the process identity can never be
        // verified after a Supervisor restart. Kill and reap it now so we do
        // not leave an unmanaged process behind.
        (void)::kill(outcome.pid, SIGKILL);
        int status = 0;
        while (::waitpid(outcome.pid, &status, 0) < 0 && errno == EINTR) {
        }
        close_fd(outcome.log_read_fd);
        close_fd(outcome.exec_read_fd);
        return Error::make(ErrorCode::unavailable,
                           "cannot determine child start time; refusing to manage the process");
    }

    auto& runtime = handle.record.runtime;
    runtime.state = ServiceState::starting;
    runtime.boot_id = boot_id_;
    runtime.pid = outcome.pid;
    runtime.pgid = outcome.pgid;
    runtime.start_ticks = outcome.start_ticks;
    runtime.started_at_ms = now_unix_millis();
    runtime.last_error.clear();
    handle.log_fd = outcome.log_read_fd;
    handle.exec_fd = outcome.exec_read_fd;
    handle.exec_confirmed = false;
    handle.exec_failed = false;
    handle.exec_error.clear();
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

    if (!group_alive(handle)) {
        complete_stop(handle, now_ms);
        return;
    }
    std::string signal_error;
    if (!signal_service_group(runtime.pgid, runtime.start_ticks, SIGTERM, signal_error)) {
        if (group_alive(handle)) {
            runtime.state = ServiceState::unknown;
            runtime.last_error = "cannot signal process group: " + signal_error;
        } else {
            complete_stop(handle, now_ms);
        }
    }
}

void SupervisorEngine::complete_stop(ServiceHandle& handle, std::int64_t now_ms) {
    auto& runtime = handle.record.runtime;
    const bool restart = handle.restart_after_stop;
    runtime.state = ServiceState::stopped;
    runtime.pid = 0;
    runtime.pgid = 0;
    runtime.start_ticks = 0;
    runtime.started_at_ms = 0;
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

    bool restarted = false;
    if (restart && !shutting_down_ && runtime.desired_state == "running") {
        Error error = spawn_locked(handle, now_ms);
        if (!error.ok()) {
            runtime.state = ServiceState::failed;
            runtime.desired_state = "stopped";
            runtime.last_error = error.message;
        } else {
            restarted = true;
        }
    }
    save(handle);
    complete_pending(handle.record.config.id, now_ms, restarted);
}

void SupervisorEngine::schedule_restart(ServiceHandle& handle, std::int64_t now_ms) {
    auto& runtime = handle.record.runtime;
    const auto& config = handle.record.config;
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
        save(handle);
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
    save(handle);
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
        runtime.state = ServiceState::stopped;
        runtime.pid = 0;
        runtime.pgid = 0;
        runtime.start_ticks = 0;
        runtime.started_at_ms = 0;
        save(handle);
        return;
    }
    if (!restart_policy_wants_restart(handle.record.config.restart_policy, failed)) {
        runtime.state = ServiceState::stopped;
        runtime.desired_state = "stopped";
        runtime.pid = 0;
        runtime.pgid = 0;
        runtime.start_ticks = 0;
        runtime.started_at_ms = 0;
        save(handle);
        return;
    }
    if (group_alive(handle)) {
        // Descendants still exist. Terminate them before a new instance is
        // started so two copies cannot run concurrently.
        runtime.state = ServiceState::backoff;
        handle.pending_auto_restart = true;
        handle.cleanup_kill_sent = false;
        handle.cleanup_deadline_ms = now_ms + handle.record.config.stop_timeout_ms;
        handle.backoff_deadline_ms = 0;
        std::string signal_error;
        (void)signal_service_group(runtime.pgid, runtime.start_ticks, SIGTERM, signal_error);
        save(handle);
        return;
    }
    runtime.pid = 0;
    runtime.pgid = 0;
    runtime.start_ticks = 0;
    runtime.started_at_ms = 0;
    schedule_restart(handle, now_ms);
}

void SupervisorEngine::handle_child_exit(ServiceHandle& handle, int status,
                                         std::int64_t now_ms) {
    auto& runtime = handle.record.runtime;
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    const int exit_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    runtime.pid = 0;
    close_fd(handle.exec_fd);
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

void SupervisorEngine::save(ServiceHandle& handle) {
    const Error error = store_->save_runtime(handle.record.config.id, handle.record.runtime);
    if (!error.ok()) {
        ++persistence_errors_;
        std::fprintf(stderr, "edgedesk-supervisor: cannot persist service %s: %s\n",
                     handle.record.config.id.c_str(), error.message.c_str());
    }
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

void SupervisorEngine::complete_ticket(std::uint64_t ticket, RpcCompletion completion) {
    completion.ticket = ticket;
    completions_.push_back(std::move(completion));
}

void SupervisorEngine::complete_pending(std::string_view id, std::int64_t now_ms,
                                        bool restarted) {
    std::vector<PendingOperation> keep;
    keep.reserve(pending_.size());
    for (auto& operation : pending_) {
        if (std::string_view(operation.service_id) != id) {
            keep.push_back(std::move(operation));
            continue;
        }
        ServiceHandle* handle = find(id);
        RpcCompletion completion;
        if (operation.restart_after) {
            if (restarted && handle != nullptr &&
                (handle->record.runtime.state == ServiceState::starting ||
                 handle->record.runtime.state == ServiceState::running)) {
                completion.ok = true;
                completion.result = record_json(*handle, now_ms);
            } else {
                completion.ok = false;
                completion.slug = "service_restart_failed";
                completion.error = Error::make(
                    ErrorCode::unavailable,
                    handle != nullptr && !handle->record.runtime.last_error.empty()
                        ? handle->record.runtime.last_error
                        : "service could not be restarted");
            }
        } else {
            completion.ok = true;
            if (handle != nullptr) completion.result = record_json(*handle, now_ms);
        }
        complete_ticket(operation.ticket, std::move(completion));
    }
    pending_ = std::move(keep);
}

bool SupervisorEngine::unverified_process_may_exist(const ServiceHandle& handle) const {
    const auto& runtime = handle.record.runtime;
    if (runtime.pid > 0) {
        ProcStat stat;
        if (read_proc_stat(runtime.pid, stat) && stat.state != 'Z') return true;
    }
    if (runtime.pgid > 0) {
        // No minimum start time is available or trustworthy here, so any
        // member of that group blocks recovery. Refusing is safer than
        // signaling or starting a duplicate.
        if (process_group_has_members(runtime.pgid, 0)) return true;
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

bool SupervisorEngine::group_alive(const ServiceHandle& handle) const {
    const auto& runtime = handle.record.runtime;
    if (runtime.pgid <= 0 || runtime.start_ticks == 0) return false;
    return process_group_has_members(runtime.pgid, runtime.start_ticks);
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
                if (!group_alive(handle)) {
                    runtime.state = ServiceState::stopped;
                    runtime.pid = 0;
                    runtime.pgid = 0;
                    runtime.start_ticks = 0;
                    runtime.started_at_ms = 0;
                    save(handle);
                }
                continue;
            }
            if (handle.pending_auto_restart) {
                if (!group_alive(handle)) {
                    handle.pending_auto_restart = false;
                    handle.cleanup_kill_sent = false;
                    handle.cleanup_deadline_ms = 0;
                    runtime.pid = 0;
                    runtime.pgid = 0;
                    runtime.start_ticks = 0;
                    runtime.started_at_ms = 0;
                    schedule_restart(handle, now_ms);
                } else if (now_ms >= handle.cleanup_deadline_ms) {
                    if (!handle.cleanup_kill_sent) {
                        std::string signal_error;
                        (void)signal_service_group(runtime.pgid, runtime.start_ticks, SIGKILL,
                                                   signal_error);
                        handle.cleanup_kill_sent = true;
                        handle.cleanup_deadline_ms = now_ms + kStopKillGraceMs;
                    } else {
                        runtime.state = ServiceState::unknown;
                        runtime.desired_state = "stopped";
                        runtime.last_error =
                            "process group did not exit after SIGKILL; refusing to duplicate";
                        handle.pending_auto_restart = false;
                        save(handle);
                    }
                }
            } else if (now_ms >= handle.backoff_deadline_ms) {
                if (group_alive(handle)) {
                    runtime.state = ServiceState::backoff;
                    handle.pending_auto_restart = true;
                    handle.cleanup_kill_sent = false;
                    handle.cleanup_deadline_ms = now_ms + handle.record.config.stop_timeout_ms;
                    std::string signal_error;
                    (void)signal_service_group(runtime.pgid, runtime.start_ticks, SIGTERM,
                                               signal_error);
                    save(handle);
                } else {
                    Error error = spawn_locked(handle, now_ms);
                    if (!error.ok()) {
                        runtime.state = ServiceState::failed;
                        runtime.desired_state = "stopped";
                        runtime.last_error = error.message;
                    }
                    save(handle);
                }
            }
            continue;
        }

        if (runtime.state == ServiceState::stopping) {
            if (!group_alive(handle)) {
                complete_stop(handle, now_ms);
                continue;
            }
            if (!handle.stop_kill_sent && now_ms >= handle.stop_term_deadline_ms) {
                std::string signal_error;
                if (!signal_service_group(runtime.pgid, runtime.start_ticks, SIGKILL,
                                          signal_error)) {
                    if (!group_alive(handle)) {
                        complete_stop(handle, now_ms);
                        continue;
                    }
                    runtime.state = ServiceState::unknown;
                    runtime.desired_state = "stopped";
                    runtime.last_error = "cannot kill process group: " + signal_error;
                    complete_pending(item.first, now_ms, false);
                    save(handle);
                    continue;
                }
                handle.stop_kill_sent = true;
            }
            if (now_ms >= handle.stop_verify_deadline_ms) {
                runtime.state = ServiceState::unknown;
                runtime.desired_state = "stopped";
                runtime.last_error = "process group did not exit after SIGKILL";
                complete_pending(item.first, now_ms, false);
                save(handle);
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
                save(handle);
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

    // Safety net: an async operation that has outlived its deadline must not
    // leave the caller hanging.
    std::vector<PendingOperation> keep;
    keep.reserve(pending_.size());
    for (auto& operation : pending_) {
        if (now_ms >= operation.deadline_ms) {
            RpcCompletion completion;
            completion.ok = false;
            completion.slug = "operation_timeout";
            completion.error =
                Error::make(ErrorCode::unavailable, "supervisor operation timed out");
            complete_ticket(operation.ticket, std::move(completion));
        } else {
            keep.push_back(std::move(operation));
        }
    }
    pending_ = std::move(keep);
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
            for (auto& item : services_) {
                if (item.second.record.runtime.pid == pid) {
                    handle = &item.second;
                    break;
                }
            }
            if (handle != nullptr) {
                handle_child_exit(*handle, status, now_ms);
            } else {
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

    std::vector<RpcCompletion> ready = std::move(completions_);
    completions_.clear();
    return ready;
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
        if (group_alive(handle) && runtime.start_ticks > 0) {
            runtime.state = ServiceState::stopping;
            handle.restart_after_stop = false;
            handle.stop_kill_sent = false;
            handle.stop_term_deadline_ms = deadline;
            handle.stop_verify_deadline_ms = deadline + kStopKillGraceMs;
            handle.pending_auto_restart = false;
            handle.cleanup_deadline_ms = 0;
            std::string signal_error;
            (void)signal_service_group(runtime.pgid, runtime.start_ticks, SIGTERM,
                                       signal_error);
            save(handle);
        } else if (runtime.state == ServiceState::running ||
                   runtime.state == ServiceState::starting ||
                   runtime.state == ServiceState::backoff) {
            runtime.state = ServiceState::stopped;
            runtime.pid = 0;
            runtime.pgid = 0;
            runtime.start_ticks = 0;
            runtime.started_at_ms = 0;
            save(handle);
        }
    }

    for (;;) {
        (void)poll(steady_millis());
        bool alive = false;
        for (const auto& item : services_) {
            if (item.second.record.runtime.state == ServiceState::stopping &&
                group_alive(item.second)) {
                alive = true;
                break;
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
        if (runtime.state == ServiceState::stopping && group_alive(handle)) {
            std::string signal_error;
            (void)signal_service_group(runtime.pgid, runtime.start_ticks, SIGKILL, signal_error);
        }
    }
    const std::int64_t kill_deadline = steady_millis() + kStopKillGraceMs;
    while (steady_millis() < kill_deadline) {
        (void)poll(steady_millis());
        bool alive = false;
        for (const auto& item : services_) {
            if (item.second.record.runtime.state == ServiceState::stopping &&
                group_alive(item.second)) {
                alive = true;
                break;
            }
        }
        if (!alive) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    for (auto& item : services_) {
        ServiceHandle& handle = item.second;
        auto& runtime = handle.record.runtime;
        if (runtime.state == ServiceState::stopping && !group_alive(handle)) {
            runtime.state = ServiceState::stopped;
            runtime.pid = 0;
            runtime.pgid = 0;
            runtime.start_ticks = 0;
            runtime.started_at_ms = 0;
        }
        close_fd(handle.log_fd);
        close_fd(handle.exec_fd);
        save(handle);
    }
}

} // namespace edgedesk::supervisor
