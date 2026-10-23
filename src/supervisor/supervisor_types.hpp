#pragma once

#include "core/error.hpp"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

// Process Supervisor domain model (SPEC-0009 / ADR-0015 / ADR-0016).
//
// Configuration is deliberately explicit: services are launched from an
// absolute executable plus an argv array. There is no shell string anywhere in
// this model, and no API accepts a PID/PGID from a client.

namespace edgedesk::supervisor {

inline constexpr std::uint64_t kDefaultLogMaxBytes = 10ull * 1024 * 1024;
inline constexpr int kDefaultRestartDelayMs = 1000;
inline constexpr int kDefaultMaxRestartDelayMs = 60 * 1000;
inline constexpr int kDefaultMaxRestarts = 5;
inline constexpr int kDefaultRestartWindowSec = 60;
inline constexpr int kDefaultStableAfterSec = 300;
inline constexpr int kDefaultStopTimeoutMs = 10 * 1000;

enum class RestartPolicy { always, on_failure, never };

enum class ServiceState { stopped, starting, running, stopping, backoff, failed, unknown };

const char* restart_policy_name(RestartPolicy policy);
bool parse_restart_policy(std::string_view text, RestartPolicy& out);
const char* service_state_name(ServiceState state);
bool parse_service_state(std::string_view text, ServiceState& out);

struct ServiceConfig {
    std::string id;
    std::string name;
    std::string executable;
    std::vector<std::string> args;
    std::string cwd = "/";
    std::map<std::string, std::string> env;
    std::string run_user;
    std::string run_group;
    // Root execution is never implied by an empty identity. The administrator
    // must explicitly confirm it; this flag is what the confirmation records.
    bool allow_root = false;
    bool autostart = false;
    RestartPolicy restart_policy = RestartPolicy::on_failure;
    int max_restarts = kDefaultMaxRestarts;
    int restart_window_sec = kDefaultRestartWindowSec;
    int restart_delay_ms = kDefaultRestartDelayMs;
    int max_restart_delay_ms = kDefaultMaxRestartDelayMs;
    int stable_after_sec = kDefaultStableAfterSec;
    int stop_timeout_ms = kDefaultStopTimeoutMs;
    std::uint64_t log_max_bytes = kDefaultLogMaxBytes;
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
};

struct ServiceRuntime {
    ServiceState state = ServiceState::stopped;
    // Human expectation for the current boot: "running" or "stopped". A manual
    // stop sets it to stopped so a Supervisor restart does not undo the action.
    std::string desired_state = "stopped";
    std::string boot_id;
    int pid = 0;
    int pgid = 0;
    std::uint64_t start_ticks = 0;
    // Sentinel process that keeps the numeric PGID reserved for this instance.
    // It is not part of the service workload and is never reported as such
    // (ADR-0019 §1). Zero means the instance predates the anchor or is stopped.
    int anchor_pid = 0;
    std::uint64_t anchor_start_ticks = 0;
    std::int64_t started_at_ms = 0;
    int last_exit_code = -1;
    int last_signal = 0;
    std::int64_t last_exit_at = 0;
    std::string last_error;
    // Cumulative restarts since the service was created.
    std::uint64_t restart_count = 0;
    // Restarts inside the current restart_window_sec window.
    std::uint64_t window_restarts = 0;
    std::int64_t window_started_at = 0;
    bool failed_locked = false;
};

struct ServiceRecord {
    ServiceConfig config;
    ServiceRuntime runtime;
};

struct ResolvedIdentity {
    uid_t uid = 0;
    gid_t gid = 0;
    std::string user_name;
    std::string group_name;
    std::string home;
    std::vector<gid_t> supplementary_groups;
    bool is_root = false;
};

// Parses a create/update body. On update, only present fields override the
// caller-provided base. Unknown fields are rejected. `allow_root` is honoured
// only when it is a boolean; the explicit flag is required for UID 0.
Error service_config_from_json(const nlohmann::json& body, ServiceConfig& config,
                               bool update_existing);

// Syntactic and filesystem validation. Executable and cwd are checked before a
// service can be created or started; identity resolution is required.
Error validate_service_config(const ServiceConfig& config);

// Resolves run_user/run_group by name or decimal ID. Supplementary groups are
// resolved in the parent before fork (NSS is not async-signal-safe).
Error resolve_identity(const std::string& user, const std::string& group,
                       ResolvedIdentity& out);

// JSON views used by IPC and the EdgeDesk API.
nlohmann::json service_config_json(const ServiceConfig& config);
nlohmann::json service_record_json(const ServiceRecord& record);
nlohmann::json service_state_json(const ServiceRecord& record);

} // namespace edgedesk::supervisor
