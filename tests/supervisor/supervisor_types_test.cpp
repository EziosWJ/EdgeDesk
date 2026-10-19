#include "test_util.hpp"

#include "supervisor/supervisor_types.hpp"

#include <nlohmann/json.hpp>
#include <pwd.h>
#include <grp.h>
#include <string>
#include <unistd.h>

using namespace edgedesk;
using namespace edgedesk::supervisor;
using nlohmann::json;

namespace {

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

json base_body() {
    json body;
    body["name"] = "edge-test";
    body["executable"] = "/bin/true";
    body["cwd"] = "/";
    body["run_user"] = current_user();
    body["run_group"] = current_group();
    body["allow_root"] = true;
    return body;
}

ServiceConfig validated(const json& body) {
    ServiceConfig config;
    const Error parsed = service_config_from_json(body, config, false);
    CHECK(parsed.ok());
    if (parsed.ok()) {
        const Error valid = validate_service_config(config);
        CHECK(valid.ok());
        if (!valid.ok()) std::fprintf(stderr, "validation error: %s\n", valid.message.c_str());
    }
    return config;
}

void test_create_requires_identity_and_paths() {
    {
        json body = base_body();
        body.erase("run_user");
        ServiceConfig config;
        CHECK(!service_config_from_json(body, config, false).ok());
    }
    {
        json body = base_body();
        body["run_user"] = "";
        ServiceConfig config;
        CHECK(!service_config_from_json(body, config, false).ok());
    }
    {
        json body = base_body();
        body["executable"] = "relative/program";
        ServiceConfig config;
        CHECK(service_config_from_json(body, config, false).ok());
        CHECK(!validate_service_config(config).ok());
    }
    {
        json body = base_body();
        body["cwd"] = "relative";
        ServiceConfig config;
        CHECK(service_config_from_json(body, config, false).ok());
        CHECK(!validate_service_config(config).ok());
    }
    {
        json body = base_body();
        body["executable"] = "/bin/definitely-not-here";
        ServiceConfig config;
        CHECK(service_config_from_json(body, config, false).ok());
        CHECK(!validate_service_config(config).ok());
    }
    {
        json body = base_body();
        body["cwd"] = "/definitely-not-here";
        ServiceConfig config;
        CHECK(service_config_from_json(body, config, false).ok());
        CHECK(!validate_service_config(config).ok());
    }
}

void test_unknown_fields_rejected() {
    json body = base_body();
    body["shell"] = "/bin/sh -c whoami";
    ServiceConfig config;
    const Error error = service_config_from_json(body, config, false);
    CHECK(!error.ok());
    CHECK(error.message.find("unknown service field") != std::string::npos);
}

void test_argv_preserves_spaces_and_empty_values() {
    json body = base_body();
    body["args"] = json::array({"a b", "", "  ", "x"});
    const ServiceConfig config = validated(body);
    CHECK_EQ(config.args.size(), static_cast<std::size_t>(4));
    if (config.args.size() == 4) {
        CHECK_EQ(config.args[0], "a b");
        CHECK_EQ(config.args[1], "");
        CHECK_EQ(config.args[2], "  ");
    }
}

void test_arg_types_rejected() {
    json body = base_body();
    body["args"] = json::array({"ok", 42});
    ServiceConfig config;
    CHECK(!service_config_from_json(body, config, false).ok());

    body = base_body();
    body["args"] = "sh -c whoami";
    CHECK(!service_config_from_json(body, config, false).ok());
}

void test_root_requires_explicit_confirmation() {
    json body = base_body();
    body["run_user"] = "0";
    body["run_group"] = "0";
    body["allow_root"] = false;
    ServiceConfig config;
    const Error parsed = service_config_from_json(body, config, false);
    CHECK(parsed.ok());
    const Error valid = validate_service_config(config);
    CHECK(!valid.ok());
    CHECK(valid.code == ErrorCode::permission_denied);

    body["allow_root"] = true;
    ServiceConfig confirmed;
    CHECK(service_config_from_json(body, confirmed, false).ok());
    CHECK(validate_service_config(confirmed).ok());
}

void test_root_allow_flag_requires_boolean() {
    json body = base_body();
    body["allow_root"] = "true";
    ServiceConfig config;
    CHECK(!service_config_from_json(body, config, false).ok());
}

void test_range_validation() {
    const char* fields[] = {"max_restarts",
                            "restart_window_sec",
                            "restart_delay_ms",
                            "stable_after_sec",
                            "stop_timeout_ms"};
    const json values[] = {json(-1), json(0), json(5), json(0), json(1)};
    for (std::size_t index = 0; index < 5; ++index) {
        json body = base_body();
        body[fields[index]] = values[index];
        ServiceConfig config;
        CHECK(service_config_from_json(body, config, false).ok());
        CHECK(!validate_service_config(config).ok());
    }

    json body = base_body();
    body["log_max_bytes"] = 1;
    ServiceConfig config;
    CHECK(service_config_from_json(body, config, false).ok());
    CHECK(!validate_service_config(config).ok());

    body = base_body();
    body["max_restart_delay_ms"] = 1;
    body["restart_delay_ms"] = 100;
    CHECK(service_config_from_json(body, config, false).ok());
    CHECK(!validate_service_config(config).ok());
}

void test_restart_policy_and_defaults() {
    const ServiceConfig config = validated(base_body());
    CHECK(config.restart_policy == RestartPolicy::on_failure);
    CHECK_EQ(config.restart_delay_ms, kDefaultRestartDelayMs);
    CHECK_EQ(config.max_restart_delay_ms, kDefaultMaxRestartDelayMs);
    CHECK_EQ(config.max_restarts, kDefaultMaxRestarts);
    CHECK_EQ(config.restart_window_sec, kDefaultRestartWindowSec);
    CHECK_EQ(config.stable_after_sec, kDefaultStableAfterSec);
    CHECK_EQ(config.stop_timeout_ms, kDefaultStopTimeoutMs);
    CHECK_EQ(config.log_max_bytes, kDefaultLogMaxBytes);
    CHECK(!config.autostart);

    RestartPolicy policy = RestartPolicy::never;
    CHECK(parse_restart_policy("always", policy));
    CHECK(policy == RestartPolicy::always);
    CHECK(parse_restart_policy("on-failure", policy));
    CHECK(policy == RestartPolicy::on_failure);
    CHECK(parse_restart_policy("never", policy));
    CHECK(policy == RestartPolicy::never);
    CHECK(!parse_restart_policy("sometimes", policy));

    json body = base_body();
    body["restart_policy"] = "sometimes";
    ServiceConfig invalid;
    CHECK(!service_config_from_json(body, invalid, false).ok());
}

void test_update_only_changes_supplied_fields() {
    ServiceConfig config;
    config.name = "existing";
    config.executable = "/bin/true";
    config.run_user = current_user();
    config.run_group = current_group();
    config.allow_root = true;
    config.max_restarts = 7;

    json patch;
    patch["name"] = "renamed";
    const Error error = service_config_from_json(patch, config, true);
    CHECK(error.ok());
    CHECK_EQ(config.name, "renamed");
    CHECK_EQ(config.executable, "/bin/true");
    CHECK_EQ(config.max_restarts, 7);
}

void test_record_json_shape() {
    ServiceRecord record;
    record.config.id = "abc123";
    record.config.name = "demo";
    record.config.executable = "/bin/true";
    record.config.run_user = current_user();
    record.config.run_group = current_group();
    record.runtime.state = ServiceState::running;
    record.runtime.desired_state = "running";
    record.runtime.pid = 123;
    record.runtime.pgid = 123;

    const json body = service_record_json(record);
    CHECK_EQ(body["id"].get<std::string>(), "abc123");
    CHECK_EQ(body["state"].get<std::string>(), "RUNNING");
    CHECK_EQ(body["desired_state"].get<std::string>(), "running");
    CHECK_EQ(body["pid"].get<int>(), 123);
    CHECK(!body.contains("env_password"));
}

void test_state_names_round_trip() {
    const ServiceState states[] = {ServiceState::stopped,  ServiceState::starting,
                                   ServiceState::running,  ServiceState::stopping,
                                   ServiceState::backoff,  ServiceState::failed,
                                   ServiceState::unknown};
    for (const ServiceState state : states) {
        ServiceState parsed = ServiceState::failed;
        CHECK(parse_service_state(service_state_name(state), parsed));
        CHECK(parsed == state);
    }
}

} // namespace

int main() {
    test_create_requires_identity_and_paths();
    test_unknown_fields_rejected();
    test_argv_preserves_spaces_and_empty_values();
    test_arg_types_rejected();
    test_root_requires_explicit_confirmation();
    test_root_allow_flag_requires_boolean();
    test_range_validation();
    test_restart_policy_and_defaults();
    test_update_only_changes_supplied_fields();
    test_record_json_shape();
    test_state_names_round_trip();
    return edgedesk::test::finish("supervisor_types_test");
}
