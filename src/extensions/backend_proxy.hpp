#pragma once

#include "core/error.hpp"
#include "extensions/extension_runtime.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

// Host-side HTTP-over-Unix-domain-socket proxy for one application's optional
// native backend (SPEC-0011 section 7). The application front end cannot name
// a socket or URL: the caller passes the validated RuntimeSession and this
// class resolves the socket from the host-owned record. Requests run on a
// small bounded worker set; the libhv loop only enqueues and consumes the
// completion callback.
namespace edgedesk::extensions {

class ExtensionStore;

struct BackendHttpResponse {
    int status = 0;
    // Allowlisted response headers (content-type and x-* only); hop-by-hop and
    // framing headers are never copied.
    nlohmann::json headers = nlohmann::json::object();
    nlohmann::json body;
    std::string raw_body;
};

class BackendProxy {
public:
    using Callback = std::function<void(Error, BackendHttpResponse)>;
    // Revalidates a queued request's runtime session on the worker thread. A
    // session can be invalidated (disable/upgrade/uninstall) after submission
    // but before the request runs; the callback must then fail so the backend
    // is never reached by a stale instance.
    using SessionValidator = std::function<Error(std::string_view token, RuntimeSession& out)>;

    // `timeout_ms` bounds connect + write + read for one request. It exists as
    // an explicit parameter so tests can use a short, deterministic deadline;
    // production uses the V1 default of 10 s.
    BackendProxy(ExtensionStore& store, std::string apps_root, std::size_t workers = 2,
                 std::size_t queue_limit = 32, int timeout_ms = 10000);
    ~BackendProxy();

    BackendProxy(const BackendProxy&) = delete;
    BackendProxy& operator=(const BackendProxy&) = delete;

    // Optional; when unset, only the persisted application status is checked.
    void set_session_validator(SessionValidator validator);

    // Validates `params` (method/path/body/contentType, no other keys) and
    // submits the request to the backend bound to `session.app_id`. Invalid
    // input completes the callback with ErrorCode::invalid_argument and still
    // returns true; false means only "queue full" or "pool stopping". The
    // callback runs on a worker, except for validation failures, which are
    // reported synchronously before anything is enqueued. Never trusts a
    // caller supplied appId, socket path, host or port.
    bool submit(const RuntimeSession& session, nlohmann::json params, Callback callback);

    std::size_t pending() const;
    int request_timeout_ms() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace edgedesk::extensions
