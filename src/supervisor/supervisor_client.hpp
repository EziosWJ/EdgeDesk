#pragma once

#include "core/error.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

// Client for the Supervisor's Unix-socket JSON-RPC endpoint.
//
// One short-lived connection is used per call, so a client instance is safe to
// share between worker threads. The call is bounded by explicit connect and
// request deadlines and never touches PIDs or process groups directly.

namespace edgedesk::supervisor {

struct SupervisorClientOptions {
    std::string socket_path;
    int connect_timeout_ms = 1500;
    // Must exceed the configured stop grace period for async stop/restart.
    int request_timeout_ms = 65000;
    std::size_t max_message_bytes = 2 * 1024 * 1024;
};

struct SupervisorRpcError {
    int code = 0;
    std::string message;
    std::string slug;
};

class SupervisorClient {
public:
    static std::unique_ptr<SupervisorClient> create(const SupervisorClientOptions& options,
                                                    Error& error);

    SupervisorClient(const SupervisorClient&) = delete;
    SupervisorClient& operator=(const SupervisorClient&) = delete;
    ~SupervisorClient();

    // Transport/protocol failures return a non-ok Error. A well-formed JSON-RPC
    // error response fills rpc_error and returns Error::none() so callers can
    // map the supervisor error to an HTTP status without losing the slug.
    Error call(std::string_view method, const nlohmann::json& params,
               nlohmann::json& result, SupervisorRpcError& rpc_error) const;

    const std::string& socket_path() const { return options_.socket_path; }
    int request_timeout_ms() const { return options_.request_timeout_ms; }

private:
    explicit SupervisorClient(SupervisorClientOptions options);

    SupervisorClientOptions options_;
    mutable std::atomic<std::uint64_t> next_id_{1};
};

} // namespace edgedesk::supervisor
