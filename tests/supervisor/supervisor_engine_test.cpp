#include "test_util.hpp"

#include "core/util.hpp"
#include "supervisor/process_control.hpp"
#include "supervisor/supervisor_engine.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <grp.h>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <string>
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
        for (const int pid : spawned_pids) {
            if (pid > 1) {
                (void)::kill(pid, SIGKILL);
                int status = 0;
                (void)::waitpid(pid, &status, WNOHANG);
            }
        }
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

bool create_engine(Harness& harness, const std::string& database,
                   const std::string& logs, const std::string& boot_id = {}) {
    EngineOptions options;
    options.database_path = database;
    options.log_directory = logs;
    options.boot_id = boot_id;
    options.metrics_interval_ms = 100;
    options.adopted_poll_interval_ms = 50;
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

bool wait_ticket(SupervisorEngine& engine, std::uint64_t ticket, RpcCompletion& out,
                 int timeout_ms = kPollTimeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& completion : engine.poll(steady_millis())) {
            if (completion.ticket == ticket) {
                out = completion;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

json get_record(Harness& harness, const std::string& id) {
    json params;
    params["id"] = id;
    RpcResult result = harness.engine->rpc("service.get", params);
    CHECK(result.ok);
    return result.ok ? result.result : json::object();
}

std::uint64_t ticket_for(SupervisorEngine& engine) {
    static std::uint64_t next = 1;
    (void)engine;
    return next++;
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

    const std::uint64_t ticket = ticket_for(*harness.engine);
    RpcResult stopping = harness.engine->rpc("service.stop", params, ticket);
    CHECK(stopping.ok);
    CHECK(stopping.async);
    RpcCompletion completion;
    CHECK(wait_ticket(*harness.engine, ticket, completion));
    CHECK(completion.ok);

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

    const std::uint64_t ticket = ticket_for(*harness.engine);
    RpcResult stopping = harness.engine->rpc("service.stop", params, ticket);
    CHECK(stopping.ok);
    if (stopping.async) {
        RpcCompletion completion;
        CHECK(wait_ticket(*harness.engine, ticket, completion));
        CHECK(completion.ok);
    }
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
    const std::uint64_t ticket = ticket_for(*harness.engine);
    RpcResult stopping = harness.engine->rpc("service.stop", params, ticket);
    CHECK(stopping.ok);
    RpcCompletion completion;
    CHECK(wait_ticket(*harness.engine, ticket, completion));
    CHECK(completion.ok);
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
    const std::uint64_t ticket = ticket_for(*harness.engine);
    CHECK(harness.engine->rpc("service.stop", params, ticket).ok);
    RpcCompletion completion;
    CHECK(wait_ticket(*harness.engine, ticket, completion));
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

    const std::uint64_t ticket = ticket_for(*harness.engine);
    CHECK(harness.engine->rpc("service.stop", params, ticket).ok);
    RpcCompletion completion;
    CHECK(wait_ticket(*harness.engine, ticket, completion));
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

    const std::uint64_t ticket = ticket_for(*harness.engine);
    CHECK(harness.engine->rpc("service.stop", params, ticket).ok);
    RpcCompletion completion;
    CHECK(wait_ticket(*harness.engine, ticket, completion));
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

    const std::uint64_t ticket = ticket_for(*harness.engine);
    CHECK(harness.engine->rpc("service.stop", params, ticket).ok);
    RpcCompletion completion;
    CHECK(wait_ticket(*harness.engine, ticket, completion));

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

    const std::uint64_t ticket = ticket_for(*harness.engine);
    CHECK(harness.engine->rpc("service.stop", params, ticket).ok);
    RpcCompletion completion;
    CHECK(wait_ticket(*harness.engine, ticket, completion));
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
    test_unknown_parameters_rejected();
    return edgedesk::test::finish("supervisor_engine_test");
}
