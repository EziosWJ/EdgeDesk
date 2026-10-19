#pragma once

#include "core/error.hpp"
#include "supervisor/log_store.hpp"
#include "supervisor/supervisor_store.hpp"
#include "supervisor/supervisor_types.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

// Supervisor lifecycle state machine and RPC surface.
//
// The engine is strictly single-threaded: all SQLite access, fork/exec,
// waitpid, signals and log I/O happen on the caller's event-loop thread.
// stop/restart requests that must wait for a grace period are completed later
// through poll(); no RPC ever blocks the loop.

namespace edgedesk::supervisor {

struct EngineOptions {
    std::string database_path;
    std::string log_directory;
    // Test hook / recovery validation. Empty means read the live boot ID.
    std::string boot_id;
    std::int64_t metrics_interval_ms = 1000;
    std::int64_t adopted_poll_interval_ms = 250;
};

struct RpcResult {
    bool async = false;
    bool ok = true;
    nlohmann::json result;
    Error error;
    std::string slug;
    // For async results: how long the caller should wait for the completion.
    int async_timeout_ms = 0;
};

struct RpcCompletion {
    std::uint64_t ticket = 0;
    bool ok = true;
    nlohmann::json result;
    Error error;
    std::string slug;
};

class SupervisorEngine {
public:
    static std::unique_ptr<SupervisorEngine> create(const EngineOptions& options, Error& error);

    SupervisorEngine(const SupervisorEngine&) = delete;
    SupervisorEngine& operator=(const SupervisorEngine&) = delete;
    ~SupervisorEngine();

    // Handles one RPC. For stop/restart that need the grace period the result
    // has async=true and a completion arrives from poll().
    RpcResult rpc(std::string_view method, const nlohmann::json& params,
                  std::uint64_t ticket = 0);

    // Drains child pipes, reaps children, runs timers and returns completed
    // async RPCs. Must be called regularly (IPC loop + tests).
    std::vector<RpcCompletion> poll(std::int64_t now_ms);

    // Upper bound until the next timer matters.
    int poll_timeout_ms(std::int64_t now_ms) const;

    // Graceful shutdown: SIGTERM each running group, wait (bounded) for exit,
    // then SIGKILL. Services keep desired_state=running so the next Supervisor
    // start recovers/starts them.
    void shutdown(std::int64_t now_ms, int grace_ms = 15000);

    const std::string& boot_id() const { return boot_id_; }
    std::size_t service_count() const { return services_.size(); }
    std::size_t running_count() const;
    std::int64_t started_at_ms() const { return started_at_ms_; }

private:
    struct ServiceHandle;
    struct PendingOperation;

    explicit SupervisorEngine(EngineOptions options);
    Error initialize();

    ServiceHandle* find(std::string_view id);
    const ServiceHandle* find(std::string_view id) const;

    nlohmann::json record_json(const ServiceHandle& handle, std::int64_t now_ms) const;
    nlohmann::json health_json(std::int64_t now_ms) const;

    RpcResult handle_create(const nlohmann::json& params, std::int64_t now_ms);
    RpcResult handle_update(const nlohmann::json& params, std::int64_t now_ms);
    RpcResult handle_delete(const nlohmann::json& params);
    RpcResult handle_start(std::string_view id, std::int64_t now_ms);
    RpcResult handle_stop(std::string_view id, std::uint64_t ticket, std::int64_t now_ms);
    RpcResult handle_restart(std::string_view id, std::uint64_t ticket, std::int64_t now_ms);
    RpcResult handle_logs(const nlohmann::json& params) const;

    Error spawn_locked(ServiceHandle& handle, std::int64_t now_ms);
    void initiate_stop(ServiceHandle& handle, bool restart_after, std::int64_t now_ms);
    void complete_stop(ServiceHandle& handle, std::int64_t now_ms);
    void schedule_restart(ServiceHandle& handle, std::int64_t now_ms);
    void handle_child_exit(ServiceHandle& handle, int status, std::int64_t now_ms);
    void handle_adopted_gone(ServiceHandle& handle, std::int64_t now_ms);
    void apply_exit(ServiceHandle& handle, bool failed, int exit_code, int signal,
                    std::int64_t now_ms);
    void save(ServiceHandle& handle);
    void close_fd(int& fd);
    void drain_log(ServiceHandle& handle, std::size_t budget);
    void drain_exec(ServiceHandle& handle);
    void update_metrics(ServiceHandle& handle, std::int64_t now_ms);
    void run_timers(std::int64_t now_ms);
    void complete_pending(std::string_view id, std::int64_t now_ms, bool restart_after);
    void complete_ticket(std::uint64_t ticket, RpcCompletion completion);
    bool group_alive(const ServiceHandle& handle) const;
    bool leader_alive(const ServiceHandle& handle) const;
    bool unverified_process_may_exist(const ServiceHandle& handle) const;

    EngineOptions options_;
    std::string boot_id_;
    std::int64_t started_at_ms_ = 0;
    std::int64_t engine_steady_ms_ = 0;
    bool shutting_down_ = false;
    std::uint64_t persistence_errors_ = 0;
    std::uint64_t log_errors_ = 0;
    std::unique_ptr<SupervisorStore> store_;
    std::unique_ptr<LogStore> logs_;
    std::map<std::string, ServiceHandle> services_;
    std::vector<PendingOperation> pending_;
    std::vector<RpcCompletion> completions_;
};

} // namespace edgedesk::supervisor
