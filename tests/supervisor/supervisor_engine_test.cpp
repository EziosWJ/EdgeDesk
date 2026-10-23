#include "test_util.hpp"

#include "core/util.hpp"
#include "supervisor/process_control.hpp"
#include "supervisor/supervisor_engine.hpp"
#include "supervisor/supervisor_store.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <grp.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace edgedesk;
using namespace edgedesk::supervisor;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kPollTimeoutMs = 8000;

fs::path make_root(const char* name) {
    const fs::path root = fs::temp_directory_path() /
                          (std::string(name) + "-" + std::to_string(::getpid()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    return root;
}

std::string current_user() {
    struct passwd* entry = ::getpwuid(::geteuid());
    if (entry != nullptr && entry->pw_name != nullptr) return entry->pw_name;
    return std::to_string(::geteuid());
}

std::string current_group() {
    struct group* entry = ::getgrgid(::getegid());
    if (entry != nullptr && entry->gr_name != nullptr) return entry->gr_name;
    return std::to_string(::getegid());
}

ServiceConfig config_for(const std::string& name, const std::string& executable,
                         const std::vector<std::string>& args) {
    ServiceConfig config;
    config.name = name;
    config.executable = executable;
    config.args = args;
    config.cwd = "/";
    config.run_user = current_user();
    config.run_group = current_group();
    config.allow_root = true;
    config.stop_timeout_ms = 200;
    config.restart_delay_ms = 10;
    config.max_restart_delay_ms = 20;
    config.stable_after_sec = 1;
    config.max_restarts = 2;
    config.restart_window_sec = 60;
    return config;
}

struct Harness {
    fs::path root;
    std::unique_ptr<SupervisorEngine> engine;
    std::vector<int> spawned_pids;

    ~Harness() {
        if (engine) {
            engine->shutdown(steady_millis(), 1000);
            engine.reset();
        }
        // Kill both the workload groups and any sentinel that outlived a failed
        // shutdown, then wait (bounded) and reap every remaining child so a
        // later test never sees this harness's orphans.
        for (const int pid : spawned_pids) {
            if (pid > 1) {
                (void)::kill(-pid, SIGKILL);
                (void)::kill(pid, SIGKILL);
            }
        }
        for (int attempt = 0; attempt < 500; ++attempt) {
            int status = 0;
            const pid_t reaped = ::waitpid(-1, &status, WNOHANG);
            if (reaped > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (reaped < 0 && errno == ECHILD) break;
            // reaped == 0: children still exist but have not exited yet.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

bool create_engine(Harness& harness, const std::string& database,
                   const std::string& logs, const std::string& boot_id = {},
                   std::function<bool()> persistence_fault = {}) {
    EngineOptions options;
    options.database_path = database;
    options.log_directory = logs;
    options.boot_id = boot_id;
    options.metrics_interval_ms = 100;
    options.adopted_poll_interval_ms = 50;
    options.persistence_fault = std::move(persistence_fault);
    Error error;
    harness.engine = SupervisorEngine::create(options, error);
    if (!harness.engine) {
        std::fprintf(stderr, "engine create failed: %s\n", error.message.c_str());
        return false;
    }
    return true;
}

std::string create_service(Harness& harness, const ServiceConfig& config) {
    json params = service_config_json(config);
    params.erase("id");
    params.erase("created_at");
    params.erase("updated_at");
    RpcResult result = harness.engine->rpc("service.create", params);
    if (!result.ok) {
        std::fprintf(stderr, "create failed: %s\n", result.error.message.c_str());
        return {};
    }
    return result.result["id"].get<std::string>();
}

bool wait_state(SupervisorEngine& engine, const std::string& id, ServiceState wanted,
                json* out = nullptr, int timeout_ms = kPollTimeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        (void)engine.poll(steady_millis());
        json params;
        params["id"] = id;
        RpcResult result = engine.rpc("service.get", params);
        if (result.ok) {
            ServiceState state = ServiceState::unknown;
            if (parse_service_state(result.result["state"].get<std::string>(), state) &&
                state == wanted) {
                if (out != nullptr) *out = result.result;
                return true;
            }
            if (out != nullptr) *out = result.result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

json get_operation(SupervisorEngine& engine, const std::string& operation_id,
                   bool* ok = nullptr) {
    json params;
    params["operation_id"] = operation_id;
    RpcResult result = engine.rpc("service.operation.get", params);
    if (ok != nullptr) *ok = result.ok;
    return result.ok ? result.result : json::object();
}

// Polls the operation contract until the Supervisor reports a terminal state.
// Timeout only means the test stopped waiting; the operation itself is not
// cancelled.
json wait_operation(SupervisorEngine& engine, const std::string& operation_id,
                    int timeout_ms = kPollTimeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    json last;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)engine.poll(steady_millis());
        last = get_operation(engine, operation_id);
        if (last.value("state", std::string()) != "pending") return last;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return last;
}

json stop_service(SupervisorEngine& engine, const std::string& id,
                  int timeout_ms = kPollTimeoutMs) {
    json params;
    params["id"] = id;
    RpcResult result = engine.rpc("service.stop", params);
    CHECK(result.ok);
    if (!result.ok) return json::object();
    const std::string operation_id = result.result.value("operation_id", std::string());
    CHECK(!operation_id.empty());
    if (operation_id.empty()) return json::object();
    // A service with nothing to terminate is retired synchronously; otherwise
    // the operation stays pending until the stop/cleanup path completes.
    if (result.result.value("state", std::string()) != "pending") return result.result;
    return wait_operation(engine, operation_id, timeout_ms);
}

json get_record(Harness& harness, const std::string& id) {
    json params;
    params["id"] = id;
    RpcResult result = harness.engine->rpc("service.get", params);
    CHECK(result.ok);
    return result.ok ? result.result : json::object();
}

void test_start_stop_lifecycle() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-lifecycle-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    const ServiceConfig config = config_for("sleeper", "/bin/sleep", {"60"});
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    RpcResult started = harness.engine->rpc("service.start", params);
    CHECK(started.ok);
    CHECK(started.result["state"].get<std::string>() == "STARTING" ||
          started.result["state"].get<std::string>() == "RUNNING");

    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    const int pid = running.value("pid", 0);
    harness.spawned_pids.push_back(pid);
    CHECK(pid > 0);
    CHECK_EQ(running.value("desired_state", std::string()), "running");
    CHECK(::kill(pid, 0) == 0);
    // The service is a session/group leader.
    CHECK(running.value("pgid", 0) == pid);

    const json operation = stop_service(*harness.engine, id);
    CHECK_EQ(operation.value("state", std::string()), "succeeded");
    CHECK_EQ(operation.value("kind", std::string()), "stop");
    CHECK(operation.contains("result"));
    CHECK_EQ(operation["result"].value("state", std::string()), "STOPPED");

    json stopped = get_record(harness, id);
    CHECK_EQ(stopped["state"].get<std::string>(), "STOPPED");
    CHECK(stopped["pid"].is_null());
    CHECK_EQ(stopped.value("desired_state", std::string()), "stopped");
    CHECK_EQ(::kill(pid, 0), -1);
}

void test_log_capture_and_cursor() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-log-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig config = config_for("echoer", "/bin/echo", {"hello", "service", "log"});
    config.restart_policy = RestartPolicy::never;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json done;
    CHECK(wait_state(*harness.engine, id, ServiceState::stopped, &done));
    CHECK_EQ(done.value("last_exit_code", -1), 0);

    json log_params;
    log_params["id"] = id;
    RpcResult first = harness.engine->rpc("service.logs", log_params);
    CHECK(first.ok);
    CHECK_EQ(first.result["content"].get<std::string>(), "hello service log\n");
    const std::uint64_t cursor = first.result["next_cursor"].get<std::uint64_t>();
    CHECK(cursor > 0);

    log_params["cursor"] = cursor;
    RpcResult second = harness.engine->rpc("service.logs", log_params);
    CHECK(second.ok);
    CHECK(second.result["content"].get<std::string>().empty());
    CHECK_EQ(second.result["next_cursor"].get<std::uint64_t>(), cursor);
}

void test_restart_limit_enters_failed() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-restart-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig config = config_for("failer", "/bin/false", {});
    config.restart_policy = RestartPolicy::on_failure;
    config.max_restarts = 1;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json record;
    CHECK(wait_state(*harness.engine, id, ServiceState::failed, &record));
    CHECK_EQ(record.value("restart_count", 0), 1);
    CHECK_EQ(record.value("desired_state", std::string()), "stopped");
    CHECK(record.value("failed_locked", false));

    // A manual start unlocks FAILED and tries again.
    RpcResult restarted = harness.engine->rpc("service.start", params);
    CHECK(restarted.ok);
    CHECK(!restarted.result.value("failed_locked", true));
    CHECK(restarted.result["state"].get<std::string>() == "STARTING" ||
          restarted.result["state"].get<std::string>() == "RUNNING");
    CHECK(wait_state(*harness.engine, id, ServiceState::failed, &record));
    CHECK_EQ(record.value("restart_count", 0), 2);
}

void test_always_policy_restarts_clean_exit() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-always-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig config = config_for("always", "/bin/true", {});
    config.restart_policy = RestartPolicy::always;
    config.max_restarts = 1;
    config.restart_delay_ms = 10;
    config.max_restart_delay_ms = 20;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json record;
    CHECK(wait_state(*harness.engine, id, ServiceState::failed, &record));
    CHECK_EQ(record.value("restart_count", 0), 1);
    // always must restart even when the process exited successfully.
    CHECK_EQ(record.value("last_exit_code", -1), 0);
    CHECK(record.value("failed_locked", false));
}

void test_backoff_delay_grows() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-backoff-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig config = config_for("backoff", "/bin/false", {});
    config.restart_policy = RestartPolicy::on_failure;
    config.max_restarts = 2;
    config.restart_delay_ms = 200;
    config.max_restart_delay_ms = 800;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);

    std::vector<std::int64_t> backoff_durations;
    std::int64_t backoff_started = 0;
    std::string previous_state;
    // Drive poll() with a synthetic clock. Measuring the BACKOFF window with
    // real time makes the assertion depend on scheduler latency: if the test
    // thread is descheduled just before it observes BACKOFF, the measured
    // window collapses below the configured delay. The engine derives every
    // timer from the now_ms passed to poll(), so advancing a fixed step per
    // iteration makes the measurement deterministic under any load.
    std::int64_t synthetic_now = steady_millis();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kPollTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        synthetic_now += 5;
        (void)harness.engine->poll(synthetic_now);
        const json record = get_record(harness, id);
        const std::string state = record.value("state", std::string());
        const std::int64_t now = synthetic_now;
        if (state == "BACKOFF" && previous_state != "BACKOFF") {
            backoff_started = now;
        } else if (state != "BACKOFF" && previous_state == "BACKOFF" &&
                   backoff_started != 0) {
            backoff_durations.push_back(now - backoff_started);
            backoff_started = 0;
        }
        previous_state = state;
        if (state == "FAILED") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(backoff_durations.size() >= 2);
    if (backoff_durations.size() >= 2) {
        // The second wait doubles the first: 400 ms vs 200 ms (+ jitter).
        CHECK(backoff_durations[0] >= 180);
        CHECK(backoff_durations[0] < 400);
        CHECK(backoff_durations[1] >= 380);
        CHECK(backoff_durations[1] > backoff_durations[0] + 100);
        CHECK(backoff_durations[1] < 800);
    }

    json record;
    CHECK(wait_state(*harness.engine, id, ServiceState::failed, &record));
    CHECK(record.value("failed_locked", false));
}

void test_stable_run_resets_restart_window() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-stable-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig config =
        config_for("stable", "/bin/sh", {"-c", "sleep 2; exit 1"});
    config.restart_policy = RestartPolicy::on_failure;
    config.max_restarts = 1;
    config.restart_delay_ms = 10;
    config.max_restart_delay_ms = 20;
    config.stable_after_sec = 1;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);

    bool restarted_twice = false;
    bool locked = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kPollTimeoutMs + 4000);
    while (std::chrono::steady_clock::now() < deadline) {
        (void)harness.engine->poll(steady_millis());
        const json record = get_record(harness, id);
        if (record.value("restart_count", 0) >= 2) {
            restarted_twice = true;
            break;
        }
        if (record.value("failed_locked", false)) {
            locked = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Two restarts are only possible after the stable run reset the window.
    CHECK(restarted_twice);
    CHECK(!locked);

    const json operation = stop_service(*harness.engine, id);
    CHECK_EQ(operation.value("state", std::string()), "succeeded");
    CHECK_EQ(get_record(harness, id)["state"].get<std::string>(), "STOPPED");
}

void test_stop_escalates_to_sigkill() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-sigkill-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig config = config_for(
        "ignore-term", "/bin/sh", {"-c", "trap '' TERM; while :; do sleep 1; done"});
    config.restart_policy = RestartPolicy::never;
    config.stop_timeout_ms = 200;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    const int pid = running.value("pid", 0);
    harness.spawned_pids.push_back(pid);
    CHECK(pid > 0);
    if (pid <= 0) return;

    const std::int64_t started_ms = steady_millis();
    const json operation = stop_service(*harness.engine, id);
    CHECK_EQ(operation.value("state", std::string()), "succeeded");
    // The process ignores SIGTERM, so completion requires the SIGKILL
    // escalation after stop_timeout_ms.
    CHECK(steady_millis() - started_ms >= 150);
    CHECK_EQ(get_record(harness, id)["state"].get<std::string>(), "STOPPED");
    CHECK_EQ(::kill(pid, 0), -1);
}

void test_never_policy_does_not_restart() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-never-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig config = config_for("never", "/bin/false", {});
    config.restart_policy = RestartPolicy::never;
    config.max_restarts = 5;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json record;
    CHECK(wait_state(*harness.engine, id, ServiceState::stopped, &record));
    CHECK_EQ(record.value("restart_count", 0), 0);
    CHECK_EQ(record.value("last_exit_code", -1), 1);
}

void test_autostart_after_supervisor_restart() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-autostart-test");
    const std::string database = (harness.root / "state" / "supervisor.db").string();
    const std::string logs = (harness.root / "logs").string();
    CHECK(create_engine(harness, database, logs, "boot-A"));
    if (!harness.engine) return;

    ServiceConfig config = config_for("autostart", "/bin/sleep", {"60"});
    config.autostart = true;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    // No explicit start: a system reboot (new boot ID) honours autostart.
    harness.engine.reset();
    CHECK(create_engine(harness, database, logs, "boot-B"));

    json record;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &record));
    harness.spawned_pids.push_back(record.value("pid", 0));
    CHECK(::kill(record.value("pid", 0), 0) == 0);

    json params;
    params["id"] = id;
    CHECK_EQ(stop_service(*harness.engine, id).value("state", std::string()), "succeeded");
}

void test_manual_stop_survives_supervisor_restart() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-desired-state-test");
    const std::string database = (harness.root / "state" / "supervisor.db").string();
    const std::string logs = (harness.root / "logs").string();
    CHECK(create_engine(harness, database, logs));
    if (!harness.engine) return;

    ServiceConfig config = config_for("manual", "/bin/sleep", {"60"});
    config.autostart = true;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    harness.spawned_pids.push_back(running.value("pid", 0));

    CHECK_EQ(stop_service(*harness.engine, id).value("state", std::string()), "succeeded");
    CHECK(wait_state(*harness.engine, id, ServiceState::stopped));
    harness.engine.reset();

    CHECK(create_engine(harness, database, logs));
    // Give initialize/run_timers a few cycles; the manually stopped service
    // must not be restarted merely because autostart is set.
    for (int index = 0; index < 20; ++index) {
        (void)harness.engine->poll(steady_millis());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const json record = get_record(harness, id);
    CHECK_EQ(record["state"].get<std::string>(), "STOPPED");
    CHECK_EQ(record.value("desired_state", std::string()), "stopped");
}

void test_adoption_after_abrupt_supervisor_exit() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-adoption-test");
    const std::string database = (harness.root / "state" / "supervisor.db").string();
    const std::string logs = (harness.root / "logs").string();
    CHECK(create_engine(harness, database, logs));
    if (!harness.engine) return;

    ServiceConfig config = config_for("adopted", "/bin/sleep", {"60"});
    config.autostart = false;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    const int pid = running.value("pid", 0);
    harness.spawned_pids.push_back(pid);
    CHECK(pid > 0);

    // Simulate a Supervisor crash: no shutdown, the child keeps running.
    harness.engine.reset();
    CHECK(::kill(pid, 0) == 0);

    CHECK(create_engine(harness, database, logs));
    json adopted = get_record(harness, id);
    CHECK_EQ(adopted["state"].get<std::string>(), "RUNNING");
    CHECK_EQ(adopted.value("pid", 0), pid);

    CHECK_EQ(stop_service(*harness.engine, id).value("state", std::string()), "succeeded");
    CHECK_EQ(::kill(pid, 0), -1);
}

void test_delete_requires_stopped_service() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-delete-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig config = config_for("deletable", "/bin/sleep", {"60"});
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    harness.spawned_pids.push_back(running.value("pid", 0));

    RpcResult refused = harness.engine->rpc("service.delete", params);
    CHECK(!refused.ok);
    CHECK_EQ(refused.slug, "service_running");

    CHECK_EQ(stop_service(*harness.engine, id).value("state", std::string()), "succeeded");

    RpcResult deleted = harness.engine->rpc("service.delete", params);
    CHECK(deleted.ok);
    CHECK(deleted.result.value("deleted", false));
}

void test_update_does_not_restart_running_service() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-update-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig config = config_for("updatable", "/bin/sleep", {"60"});
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    const int pid = running.value("pid", 0);
    harness.spawned_pids.push_back(pid);

    json update;
    update["id"] = id;
    update["name"] = "updatable-renamed";
    RpcResult updated = harness.engine->rpc("service.update", update);
    CHECK(updated.ok);
    CHECK_EQ(updated.result["name"].get<std::string>(), "updatable-renamed");
    CHECK_EQ(updated.result["state"].get<std::string>(), "RUNNING");

    const json after = get_record(harness, id);
    CHECK_EQ(after.value("pid", 0), pid);
    CHECK_EQ(::kill(pid, 0), 0);

    CHECK_EQ(stop_service(*harness.engine, id).value("state", std::string()), "succeeded");
}

// PSH-06: stop/restart report an operation object, repeated submissions of the
// same pending operation are idempotent, and operations remain queryable.
void test_operation_contract() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-operation-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig config = config_for(
        "operation", "/bin/sh", {"-c", "trap '' TERM; while :; do sleep 1; done"});
    config.restart_policy = RestartPolicy::never;
    config.stop_timeout_ms = 150;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    const int pid = running.value("pid", 0);
    harness.spawned_pids.push_back(pid);
    CHECK(pid > 0);
    if (pid <= 0) return;

    RpcResult first = harness.engine->rpc("service.stop", params);
    CHECK(first.ok);
    if (!first.ok) return;
    const std::string operation_id = first.result.value("operation_id", std::string());
    CHECK(!operation_id.empty());
    CHECK_EQ(first.result.value("state", std::string()), "pending");
    CHECK_EQ(first.result.value("kind", std::string()), "stop");
    CHECK_EQ(first.result.value("service_id", std::string()), id);
    CHECK(first.result["error"].is_null());
    CHECK(first.result["finished_at"].is_null());

    // While the same stop is pending, another request returns the same
    // operation instead of signalling the group twice.
    RpcResult duplicate = harness.engine->rpc("service.stop", params);
    CHECK(duplicate.ok);
    CHECK_EQ(duplicate.result.value("operation_id", std::string()), operation_id);

    const json operation = wait_operation(*harness.engine, operation_id);
    CHECK_EQ(operation.value("state", std::string()), "succeeded");
    CHECK_EQ(operation["result"].value("state", std::string()), "STOPPED");
    CHECK_EQ(operation.value("service_id", std::string()), id);
    CHECK(operation["error"].is_null());
    CHECK(!operation["finished_at"].is_null());
    CHECK_EQ(::kill(pid, 0), -1);

    // Completed operations stay queryable; unknown IDs are not found.
    bool found = false;
    (void)get_operation(*harness.engine, operation_id, &found);
    CHECK(found);
    json missing;
    missing["operation_id"] = "op-does-not-exist";
    RpcResult unknown = harness.engine->rpc("service.operation.get", missing);
    CHECK(!unknown.ok);
    CHECK_EQ(unknown.slug, "operation_not_found");

    // A stop with nothing left to terminate completes immediately, and is a
    // fresh operation rather than a rewrite of the previous one.
    RpcResult again = harness.engine->rpc("service.stop", params);
    CHECK(again.ok);
    CHECK(again.result.value("operation_id", std::string()) != operation_id);
    CHECK_EQ(again.result.value("state", std::string()), "succeeded");
}

// PSH-04: a persistence failure must never leave a child running behind a
// failed start, and a stop whose intent cannot be persisted must not signal.
void test_persistence_failure_is_fail_closed() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-persistence-test");
    const std::string database = (harness.root / "state" / "supervisor.db").string();
    const std::string logs = (harness.root / "logs").string();
    auto mode = std::make_shared<std::atomic<int>>(0);  // 0 off, 1 always, 2 second
    auto calls = std::make_shared<int>(0);
    CHECK(create_engine(harness, database, logs, {},
                        [mode, calls]() -> bool {
                            const int current = mode->load();
                            if (current == 1) return true;
                            if (current == 2) return ++*calls == 2;
                            return false;
                        }));
    if (!harness.engine) return;

    ServiceConfig config = config_for("faulted", "/bin/sleep", {"60"});
    config.restart_policy = RestartPolicy::never;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;
    json params;
    params["id"] = id;

    // The start intent is persisted, then the process identity write fails.
    // The blocked child must be aborted and reaped instead of being released.
    mode->store(2);
    RpcResult failed_start = harness.engine->rpc("service.start", params);
    mode->store(0);
    CHECK(!failed_start.ok);
    const json failed = get_record(harness, id);
    CHECK_EQ(failed["state"].get<std::string>(), "FAILED");
    CHECK(failed["pid"].is_null());
    CHECK_EQ(failed.value("desired_state", std::string()), "stopped");
    CHECK(failed.value("last_error", std::string()).find("persist process identity") !=
          std::string::npos);

    // A stop intent that cannot be persisted is refused before any signal.
    *calls = 0;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    const int pid = running.value("pid", 0);
    harness.spawned_pids.push_back(pid);
    CHECK(pid > 0);
    if (pid <= 0) return;

    mode->store(1);
    RpcResult refused = harness.engine->rpc("service.stop", params);
    mode->store(0);
    CHECK(!refused.ok);
    CHECK_EQ(refused.slug, "supervisor_unavailable");
    const json still = get_record(harness, id);
    CHECK_EQ(still["state"].get<std::string>(), "RUNNING");
    CHECK_EQ(still.value("desired_state", std::string()), "running");
    CHECK_EQ(still.value("pid", 0), pid);
    CHECK(::kill(pid, 0) == 0);

    CHECK_EQ(stop_service(*harness.engine, id).value("state", std::string()), "succeeded");
    CHECK_EQ(::kill(pid, 0), -1);
}

// PSH-02: a leader that leaves descendants behind must have the whole group
// cleaned up before a restart creates a second copy.
void test_descendant_cleanup_before_restart() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-descendant-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    const std::string marker = (harness.root / "first-run.marker").string();
    const std::string descendant_file = (harness.root / "descendant.pid").string();
    const fs::path script = harness.root / "orphan.sh";
    {
        // The first instance leaves an orphan that ignores SIGTERM; the
        // restarted instance stays alive so the test can observe it.
        std::ofstream output(script);
        output << "#!/bin/sh\n"
               << "if [ ! -e " << marker << " ]; then\n"
               << "  : > " << marker << "\n"
               << "  trap '' TERM\n"
               << "  sleep 300 &\n"
               << "  echo $! > " << descendant_file << "\n"
               << "  exit 0\n"
               << "fi\n"
               << "exec sleep 300\n";
    }
    CHECK(::chmod(script.c_str(), 0755) == 0);

    ServiceConfig config = config_for("orphan-restart", script.string(), {});
    config.restart_policy = RestartPolicy::always;
    config.stop_timeout_ms = 200;
    config.max_restarts = 5;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    RpcResult started = harness.engine->rpc("service.start", params);
    CHECK(started.ok);
    const int first_pid = started.result.value("pid", 0);
    CHECK(first_pid > 0);
    if (first_pid <= 0) return;
    CHECK_EQ(::kill(first_pid, 0), 0);

    // Wait until the background descendant proved it exists.
    int descendant = 0;
    const auto marker_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < marker_deadline && descendant <= 0) {
        (void)harness.engine->poll(steady_millis());
        std::ifstream input(descendant_file);
        if (input) {
            input >> descendant;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(descendant > 0);
    if (descendant <= 0) return;
    CHECK_EQ(::kill(descendant, 0), 0);

    // The restart may only happen after the old group is gone, so the second
    // process must appear with a new identity and the descendant must be dead.
    json record;
    int second_pid = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kPollTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        (void)harness.engine->poll(steady_millis());
        record = get_record(harness, id);
        const int pid = record["pid"].is_number_integer() ? record["pid"].get<int>() : 0;
        if (pid > 0 && pid != first_pid &&
            record.value("state", std::string()) == "RUNNING") {
            second_pid = pid;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(second_pid > 0);
    if (second_pid <= 0) return;
    harness.spawned_pids.push_back(second_pid);
    CHECK_EQ(::kill(second_pid, 0), 0);
    CHECK_EQ(::kill(descendant, 0), -1);
    CHECK(!process_group_has_any_members(first_pid));

    CHECK_EQ(stop_service(*harness.engine, id).value("state", std::string()), "succeeded");
    CHECK_EQ(::kill(second_pid, 0), -1);
}

// PSH-05: a stop interrupted by a Supervisor crash is resumed; the persisted
// desired state is honoured and the old process identity survives until the
// group is confirmed absent.
void test_interrupted_stop_recovery() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-recovery-test");
    const std::string database = (harness.root / "state" / "supervisor.db").string();
    const std::string logs = (harness.root / "logs").string();
    CHECK(create_engine(harness, database, logs));
    if (!harness.engine) return;

    ServiceConfig config = config_for(
        "recovered-stop", "/bin/sh", {"-c", "trap '' TERM; while :; do sleep 1; done"});
    config.restart_policy = RestartPolicy::never;
    config.stop_timeout_ms = 200;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    const int pid = running.value("pid", 0);
    harness.spawned_pids.push_back(pid);
    CHECK(pid > 0);
    if (pid <= 0) return;

    RpcResult stop = harness.engine->rpc("service.stop", params);
    CHECK(stop.ok);
    CHECK_EQ(stop.result.value("state", std::string()), "pending");

    // Simulate a crash while the grace period is running.
    harness.engine.reset();
    CHECK(::kill(pid, 0) == 0);

    CHECK(create_engine(harness, database, logs));
    json record;
    CHECK(wait_state(*harness.engine, id, ServiceState::stopped, &record));
    CHECK_EQ(record.value("desired_state", std::string()), "stopped");
    CHECK(record["pid"].is_null());
    CHECK_EQ(::kill(pid, 0), -1);
}

// PSH-05: a persisted identity that cannot be verified must become UNKNOWN and
// must never be signalled or duplicated.
void test_recovery_unverifiable_identity() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-unknown-test");
    const std::string database = (harness.root / "state" / "supervisor.db").string();
    const std::string logs = (harness.root / "logs").string();
    CHECK(create_engine(harness, database, logs));
    if (!harness.engine) return;

    ServiceConfig config = config_for("unknown", "/bin/sleep", {"60"});
    config.restart_policy = RestartPolicy::never;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    const int pid = running.value("pid", 0);
    harness.spawned_pids.push_back(pid);
    CHECK(pid > 0);
    if (pid <= 0) return;

    // Simulate a Supervisor crash, then corrupt the persisted start time so
    // the PID can no longer be proven to be the same process.
    harness.engine.reset();
    {
        Error error;
        auto store = SupervisorStore::open(database, error);
        CHECK(store != nullptr);
        if (!store) return;
        ServiceRecord record;
        bool found = false;
        error = store->get(id, record, found);
        CHECK(error.ok());
        CHECK(found);
        record.runtime.start_ticks += 1;
        error = store->save_runtime(id, record.runtime);
        CHECK(error.ok());
    }

    CHECK(create_engine(harness, database, logs));
    json record = get_record(harness, id);
    CHECK_EQ(record["state"].get<std::string>(), "UNKNOWN");
    CHECK(record.value("last_error", std::string()).find("cannot verify") !=
          std::string::npos);
    // The unverifiable process was left untouched rather than signalled.
    CHECK_EQ(::kill(pid, 0), 0);
}

// PSH-07: a shebang script starts through a verified descriptor, so the file
// that was inspected is the file that runs; a broken interpreter fails closed.
void test_shebang_script_start() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-script-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    const fs::path script = harness.root / "hello.sh";
    {
        std::ofstream output(script);
        output << "#!/bin/sh\necho script-ran\n";
    }
    CHECK(::chmod(script.c_str(), 0755) == 0);

    ServiceConfig config = config_for("script", script.string(), {"literal-argument"});
    config.restart_policy = RestartPolicy::never;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json done;
    CHECK(wait_state(*harness.engine, id, ServiceState::stopped, &done));
    CHECK_EQ(done.value("last_exit_code", -1), 0);

    json log_params;
    log_params["id"] = id;
    RpcResult log_result = harness.engine->rpc("service.logs", log_params);
    CHECK(log_result.ok);
    if (log_result.ok) {
        CHECK(log_result.result["content"].get<std::string>().find("script-ran") !=
              std::string::npos);
    }

    const fs::path broken = harness.root / "broken.sh";
    {
        std::ofstream output(broken);
        output << "#!/definitely/not/installed/interpreter\n";
    }
    CHECK(::chmod(broken.c_str(), 0755) == 0);

    ServiceConfig broken_config = config_for("broken-script", broken.string(), {});
    broken_config.restart_policy = RestartPolicy::never;
    const std::string broken_id = create_service(harness, broken_config);
    CHECK(!broken_id.empty());
    if (broken_id.empty()) return;

    json broken_params;
    broken_params["id"] = broken_id;
    RpcResult failed = harness.engine->rpc("service.start", broken_params);
    CHECK(!failed.ok);
    const json broken_record = get_record(harness, broken_id);
    CHECK_EQ(broken_record["state"].get<std::string>(), "FAILED");
    CHECK(broken_record.value("last_error", std::string()).find("interpreter not found") !=
          std::string::npos);
}

// PSH-06: completed operations are retained up to a fixed bound; the oldest
// terminal operation is evicted instead of growing forever.
void test_operation_retention_bound() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-operation-bound-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig config = config_for("bounded", "/bin/sleep", {"60"});
    config.restart_policy = RestartPolicy::never;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    std::string first_id;
    std::string last_id;
    for (int index = 0; index < 300; ++index) {
        RpcResult result = harness.engine->rpc("service.stop", params);
        CHECK(result.ok);
        if (!result.ok) return;
        const std::string operation_id =
            result.result.value("operation_id", std::string());
        CHECK_EQ(result.result.value("state", std::string()), "succeeded");
        if (first_id.empty()) first_id = operation_id;
        last_id = operation_id;
    }
    CHECK(!first_id.empty());
    CHECK(!last_id.empty());
    CHECK(first_id != last_id);

    bool found = false;
    (void)get_operation(*harness.engine, first_id, &found);
    CHECK(!found);
    (void)get_operation(*harness.engine, last_id, &found);
    CHECK(found);
    CHECK_EQ(get_record(harness, id)["state"].get<std::string>(), "STOPPED");
}

// PSH-04: an unfinished start (persisted STARTING without a process identity)
// must not be reinterpreted as "desired=running" after a restart. A stale row
// produced by a failed write is the dangerous case.
void test_incomplete_start_is_not_auto_started() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-incomplete-start-test");
    const std::string database = (harness.root / "state" / "supervisor.db").string();
    const std::string logs = (harness.root / "logs").string();
    CHECK(create_engine(harness, database, logs));
    if (!harness.engine) return;

    ServiceConfig config = config_for("incomplete", "/bin/sleep", {"60"});
    config.autostart = true;
    config.restart_policy = RestartPolicy::always;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    harness.engine.reset();
    {
        Error error;
        auto store = SupervisorStore::open(database, error);
        CHECK(store != nullptr);
        if (!store) return;
        ServiceRecord record;
        bool found = false;
        CHECK(store->get(id, record, found).ok());
        CHECK(found);
        // Exactly the row a failed identity write leaves behind: the start
        // intent is durable but no PID/start time/anchor ever was.
        record.runtime.state = ServiceState::starting;
        record.runtime.desired_state = "running";
        record.runtime.boot_id = read_boot_id();
        record.runtime.pid = 0;
        record.runtime.pgid = 0;
        record.runtime.start_ticks = 0;
        record.runtime.anchor_pid = 0;
        record.runtime.anchor_start_ticks = 0;
        CHECK(store->save_runtime(id, record.runtime).ok());
    }

    CHECK(create_engine(harness, database, logs));
    if (!harness.engine) return;
    for (int index = 0; index < 30; ++index) {
        (void)harness.engine->poll(steady_millis());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const json record = get_record(harness, id);
    CHECK_EQ(record["state"].get<std::string>(), "FAILED");
    CHECK_EQ(record.value("desired_state", std::string()), "stopped");
    CHECK(record["pid"].is_null());
    CHECK_EQ(record.value("restart_count", 0), 0);
    CHECK(record.value("last_error", std::string()).find("interrupted") !=
          std::string::npos);

    // A manual start still works once the operator asks for it.
    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    harness.spawned_pids.push_back(running.value("pid", 0));
    CHECK_EQ(stop_service(*harness.engine, id).value("state", std::string()), "succeeded");
}

// PSH-04: the durable inconsistency marker suppresses autostart (including a
// new boot cycle) until the operator starts the service explicitly.
void test_persistence_marker_blocks_autostart() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-marker-test");
    const std::string database = (harness.root / "state" / "supervisor.db").string();
    const std::string logs = (harness.root / "logs").string();
    CHECK(create_engine(harness, database, logs, "boot-A"));
    if (!harness.engine) return;

    ServiceConfig config = config_for("marker", "/bin/sleep", {"60"});
    config.autostart = true;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;
    harness.engine.reset();

    {
        Error error;
        auto store = SupervisorStore::open(database, error);
        CHECK(store != nullptr);
        if (!store) return;
        CHECK(store->mark_inconsistent("test injection").ok());
        CHECK(store->inconsistent_marker_present());
    }

    // A new boot ID would normally start an autostart service.
    CHECK(create_engine(harness, database, logs, "boot-B"));
    if (!harness.engine) return;
    for (int index = 0; index < 30; ++index) {
        (void)harness.engine->poll(steady_millis());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const json record = get_record(harness, id);
    CHECK_EQ(record["state"].get<std::string>(), "STOPPED");
    CHECK_EQ(record.value("desired_state", std::string()), "stopped");
    CHECK(record["pid"].is_null());

    // Normalization persisted; the marker is cleared for the next start.
    {
        Error error;
        auto store = SupervisorStore::open(database, error);
        CHECK(store != nullptr);
        if (store) CHECK(!store->inconsistent_marker_present());
    }

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    harness.spawned_pids.push_back(running.value("pid", 0));
    CHECK_EQ(stop_service(*harness.engine, id).value("state", std::string()), "succeeded");
}

// PSH-04: once a runtime write fails, no automatic restart may happen, and the
// next Supervisor start refuses to derive a start from the stale record.
void test_persistence_failure_disables_auto_restart() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-persist-restart-test");
    const std::string database = (harness.root / "state" / "supervisor.db").string();
    const std::string logs = (harness.root / "logs").string();
    auto fail = std::make_shared<std::atomic<bool>>(false);
    CHECK(create_engine(harness, database, logs, {},
                        [fail]() -> bool { return fail->load(); }));
    if (!harness.engine) return;

    ServiceConfig config = config_for("persist-restart", "/bin/sleep", {"60"});
    config.restart_policy = RestartPolicy::always;
    config.restart_delay_ms = 10;
    config.max_restart_delay_ms = 20;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    const int pid = running.value("pid", 0);
    harness.spawned_pids.push_back(pid);
    CHECK(pid > 0);
    if (pid <= 0) return;

    fail->store(true);
    CHECK_EQ(::kill(pid, SIGKILL), 0);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kPollTimeoutMs);
    bool stopped = false;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)harness.engine->poll(steady_millis());
        const json record = get_record(harness, id);
        const std::string state = record.value("state", std::string());
        if (state == "FAILED" || state == "UNKNOWN") {
            stopped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(stopped);
    const json failed_record = get_record(harness, id);
    CHECK_EQ(failed_record.value("desired_state", std::string()), "stopped");
    CHECK(failed_record["pid"].is_null());

    // No replacement process may exist after the failure.
    CHECK_EQ(::kill(pid, 0), -1);

    // The next Supervisor must not autostart the service from a stale row even
    // though desired_state was running when the write failed.
    harness.engine.reset();
    fail->store(false);
    CHECK(create_engine(harness, database, logs, {}, {}));
    if (!harness.engine) return;
    for (int index = 0; index < 30; ++index) {
        (void)harness.engine->poll(steady_millis());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const json recovered = get_record(harness, id);
    CHECK_EQ(recovered.value("desired_state", std::string()), "stopped");
    CHECK(recovered["pid"].is_null());
    CHECK(recovered.value("state", std::string()) != "RUNNING");
}

// PSH-06: a pending operation at the head of the queue must not prevent older
// completed operations from being evicted.
void test_operation_pruning_with_pending_front() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-operation-pending-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig blocking = config_for(
        "pending-stop", "/bin/sh", {"-c", "trap '' TERM; while :; do sleep 1; done"});
    blocking.restart_policy = RestartPolicy::never;
    blocking.stop_timeout_ms = 300;
    const std::string blocking_id = create_service(harness, blocking);
    CHECK(!blocking_id.empty());
    if (blocking_id.empty()) return;

    ServiceConfig quick = config_for("quick-stop", "/bin/sleep", {"60"});
    quick.restart_policy = RestartPolicy::never;
    const std::string quick_id = create_service(harness, quick);
    CHECK(!quick_id.empty());
    if (quick_id.empty()) return;

    json blocking_params;
    blocking_params["id"] = blocking_id;
    CHECK(harness.engine->rpc("service.start", blocking_params).ok);
    json running;
    CHECK(wait_state(*harness.engine, blocking_id, ServiceState::running, &running));
    harness.spawned_pids.push_back(running.value("pid", 0));

    RpcResult pending = harness.engine->rpc("service.stop", blocking_params);
    CHECK(pending.ok);
    CHECK_EQ(pending.result.value("state", std::string()), "pending");
    const std::string pending_id = pending.result.value("operation_id", std::string());
    CHECK(!pending_id.empty());

    json quick_params;
    quick_params["id"] = quick_id;
    std::string first_completed;
    std::string last_completed;
    for (int index = 0; index < 300; ++index) {
        RpcResult result = harness.engine->rpc("service.stop", quick_params);
        CHECK(result.ok);
        if (!result.ok) return;
        const std::string operation_id =
            result.result.value("operation_id", std::string());
        if (first_completed.empty()) first_completed = operation_id;
        last_completed = operation_id;
    }
    CHECK(!first_completed.empty());

    bool found = false;
    (void)get_operation(*harness.engine, first_completed, &found);
    CHECK(!found);
    (void)get_operation(*harness.engine, last_completed, &found);
    CHECK(found);
    (void)get_operation(*harness.engine, pending_id, &found);
    CHECK(found);

    const json operation = wait_operation(*harness.engine, pending_id);
    CHECK_EQ(operation.value("state", std::string()), "succeeded");
}

// PSH-02: restart_policy=never must still terminate descendants left behind by
// the leader before the service becomes STOPPED.
void test_orphan_cleanup_with_never_policy() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-orphan-never-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    const std::string descendant_file = (harness.root / "orphan.pid").string();
    const fs::path script = harness.root / "orphan-never.sh";
    {
        std::ofstream output(script);
        output << "#!/bin/sh\n"
               << "trap '' TERM\n"
               << "sleep 300 &\n"
               << "echo $! > " << descendant_file << "\n"
               << "exit 0\n";
    }
    CHECK(::chmod(script.c_str(), 0755) == 0);

    ServiceConfig config = config_for("orphan-never", script.string(), {});
    config.restart_policy = RestartPolicy::never;
    config.stop_timeout_ms = 200;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);

    int descendant = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kPollTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        (void)harness.engine->poll(steady_millis());
        std::ifstream input(descendant_file);
        if (input) input >> descendant;
        const json record = get_record(harness, id);
        if (record.value("state", std::string()) == "STOPPED") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(descendant > 0);
    if (descendant > 0) CHECK_EQ(::kill(descendant, 0), -1);
    const json record = get_record(harness, id);
    CHECK_EQ(record["state"].get<std::string>(), "STOPPED");
    CHECK(record["pid"].is_null());
}

// PSH-04/PSH-05: an UNKNOWN transition (for example because the identity anchor
// disappeared) sets desired_state=stopped; when the leader later exits, the
// restart policy must not resurrect a service the Supervisor already declared
// terminal.
void test_unknown_is_not_auto_restarted() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-no-resurrect-test");
    const std::string database = (harness.root / "state" / "supervisor.db").string();
    CHECK(create_engine(harness, database, (harness.root / "logs").string()));
    if (!harness.engine) return;

    ServiceConfig config = config_for("no-resurrect", "/bin/sleep", {"60"});
    config.restart_policy = RestartPolicy::always;
    config.max_restarts = 5;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    const int pid = running.value("pid", 0);
    harness.spawned_pids.push_back(pid);
    CHECK(pid > 0);
    if (pid <= 0) return;

    int anchor = 0;
    {
        Error error;
        auto store = SupervisorStore::open(database, error);
        CHECK(store != nullptr);
        if (!store) return;
        ServiceRecord record;
        bool found = false;
        CHECK(store->get(id, record, found).ok());
        CHECK(found);
        anchor = record.runtime.anchor_pid;
    }
    CHECK(anchor > 1);
    if (anchor <= 1) return;

    // The anchor is root-owned and ignores signals; only the supervisor or a
    // root actor can remove it. Simulate that loss and let the engine reach its
    // terminal UNKNOWN state without signalling the leader.
    CHECK_EQ(::kill(anchor, SIGKILL), 0);
    json unknown;
    CHECK(wait_state(*harness.engine, id, ServiceState::unknown, &unknown));
    CHECK_EQ(unknown.value("desired_state", std::string()), "stopped");
    CHECK_EQ(::kill(pid, 0), 0);

    // The leader exits after the terminal transition. Policy=always must not
    // start a replacement.
    CHECK_EQ(::kill(pid, SIGKILL), 0);
    bool resurrected = false;
    for (int index = 0; index < 300; ++index) {
        (void)harness.engine->poll(steady_millis());
        const json record = get_record(harness, id);
        const int current = record["pid"].is_number_integer() ? record["pid"].get<int>() : 0;
        // The record keeps the old PID until waitpid is processed; only a new
        // identity or a RUNNING/STARTING state proves a resurrection.
        if ((current > 0 && current != pid) ||
            record.value("state", std::string()) == "RUNNING" ||
            record.value("state", std::string()) == "STARTING") {
            resurrected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(!resurrected);
    const json final = get_record(harness, id);
    CHECK_EQ(final.value("desired_state", std::string()), "stopped");
    CHECK(final["pid"].is_null());
}

// PSH-04: a persistent runtime-write failure during descendant cleanup must
// move the service to an explicit cleanup/terminal state instead of re-arming
// the backoff cleanup on every poll (which spins the event loop and floods the
// group with SIGTERM).
void test_persistent_failure_does_not_spin() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-persist-spin-test");
    const std::string database = (harness.root / "state" / "supervisor.db").string();
    auto fail = std::make_shared<std::atomic<bool>>(false);
    CHECK(create_engine(harness, database, (harness.root / "logs").string(), {},
                        [fail]() -> bool { return fail->load(); }));
    if (!harness.engine) return;

    const fs::path marker = harness.root / "orphan.pid";
    const fs::path script = harness.root / "orphan-spin.sh";
    {
        std::ofstream output(script);
        output << "#!/bin/sh\n"
               << "trap '' TERM\n"
               << "sh -c 'trap \"\" TERM; while :; do sleep 1; done' &\n"
               << "echo $! > " << marker.string() << "\n"
               << "sleep 0.1\n"
               << "exit 1\n";
    }
    CHECK(::chmod(script.c_str(), 0755) == 0);

    ServiceConfig config = config_for("persist-spin", script.string(), {});
    config.restart_policy = RestartPolicy::always;
    config.max_restarts = 1000;
    config.stop_timeout_ms = 200;
    const std::string id = create_service(harness, config);
    CHECK(!id.empty());
    if (id.empty()) return;

    json params;
    params["id"] = id;
    CHECK(harness.engine->rpc("service.start", params).ok);
    json running;
    CHECK(wait_state(*harness.engine, id, ServiceState::running, &running));
    const int leader_pid = running.value("pid", 0);
    const int pgid = running.value("pgid", 0);
    harness.spawned_pids.push_back(leader_pid);
    CHECK(leader_pid > 0 && pgid > 0);

    int descendant = 0;
    const auto marker_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < marker_deadline && descendant <= 0) {
        (void)harness.engine->poll(steady_millis());
        std::ifstream input(marker);
        if (input) input >> descendant;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(descendant > 0);
    if (descendant <= 0) return;

    // Everything from here on fails to persist.
    fail->store(true);

    // Wait until the leader is gone and the engine has observed its exit with a
    // live, TERM-immune descendant.
    json record;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kPollTimeoutMs);
    bool left_backoff = false;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)harness.engine->poll(steady_millis());
        record = get_record(harness, id);
        const std::string state = record.value("state", std::string());
        if (state == "STOPPED" || state == "FAILED" || state == "UNKNOWN") {
            left_backoff = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(left_backoff);
    CHECK(record["pid"].is_null());
    CHECK(!process_group_has_any_members(pgid));
    CHECK_EQ(::kill(descendant, 0), -1);
    fail->store(false);
}

void test_unknown_parameters_rejected() {
    Harness harness;
    harness.root = make_root("edgedesk-engine-params-test");
    CHECK(create_engine(harness, (harness.root / "state" / "supervisor.db").string(),
                        (harness.root / "logs").string()));
    if (!harness.engine) return;

    RpcResult result = harness.engine->rpc("service.kill", json::object());
    CHECK(!result.ok);
    CHECK_EQ(result.slug, "method_not_found");

    result = harness.engine->rpc("service.start", json::array());
    CHECK(!result.ok);
    CHECK_EQ(result.slug, "invalid_params");
}

} // namespace

int main() {
    test_start_stop_lifecycle();
    test_log_capture_and_cursor();
    test_restart_limit_enters_failed();
    test_always_policy_restarts_clean_exit();
    test_backoff_delay_grows();
    test_stable_run_resets_restart_window();
    test_stop_escalates_to_sigkill();
    test_never_policy_does_not_restart();
    test_autostart_after_supervisor_restart();
    test_manual_stop_survives_supervisor_restart();
    test_adoption_after_abrupt_supervisor_exit();
    test_delete_requires_stopped_service();
    test_update_does_not_restart_running_service();
    test_operation_contract();
    test_operation_retention_bound();
    test_persistence_failure_is_fail_closed();
    test_descendant_cleanup_before_restart();
    test_interrupted_stop_recovery();
    test_recovery_unverifiable_identity();
    test_shebang_script_start();
    test_incomplete_start_is_not_auto_started();
    test_persistence_marker_blocks_autostart();
    test_persistence_failure_disables_auto_restart();
    test_operation_pruning_with_pending_front();
    test_orphan_cleanup_with_never_policy();
    test_unknown_is_not_auto_restarted();
    test_persistent_failure_does_not_spin();
    test_unknown_parameters_rejected();
    return edgedesk::test::finish("supervisor_engine_test");
}
