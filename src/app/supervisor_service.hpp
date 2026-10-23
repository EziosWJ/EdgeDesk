#pragma once

#include "core/error.hpp"
#include "supervisor/supervisor_client.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

// Bounded execution context for Supervisor IPC calls made by HTTP handlers.
// libhv's event loop never waits on the socket itself: every call runs on one
// of a small fixed set of workers, and the HTTP response is written from the
// completion callback (the same pattern used by the password hash service).

namespace edgedesk {

struct SupervisorResult {
    int status = 503;
    std::string code;
    std::string message;
    nlohmann::json value;
};

class SupervisorService {
public:
    using Callback = std::function<void(SupervisorResult)>;

    // `client` may be null when process management is disabled; every call
    // then completes with supervisor_unavailable instead of blocking.
    SupervisorService(std::unique_ptr<supervisor::SupervisorClient> client,
                      std::size_t workers = 2, std::size_t queue_limit = 32);
    ~SupervisorService();

    SupervisorService(const SupervisorService&) = delete;
    SupervisorService& operator=(const SupervisorService&) = delete;

    bool configured() const;
    // False when the bounded queue is full or the pool is stopping.
    bool submit(std::string method, nlohmann::json params, Callback callback);

    // Stops a service (when needed), waits for the stop operation to finish,
    // then clears autostart and returns the final service record. Used by the
    // application-disable path, which must not report success while the
    // process may still be alive or a boot autostart remains armed. Runs as
    // one bounded worker job; the callback receives supervisor_unavailable or
    // the Supervisor's error slug on failure.
    bool submit_stop_and_lock(std::string service_id, Callback callback);

    std::size_t pending() const;
    int request_timeout_ms() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace edgedesk
