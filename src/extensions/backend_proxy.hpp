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

    BackendProxy(ExtensionStore& store, std::string apps_root, std::size_t workers = 2,
                 std::size_t queue_limit = 32);
    ~BackendProxy();

    BackendProxy(const BackendProxy&) = delete;
    BackendProxy& operator=(const BackendProxy&) = delete;

    // Validates `params` (method/path/body/contentType) and submits the request
    // to the backend bound to `session.app_id`. Returns false only when the
    // bounded queue is full or the pool is stopping. Never trusts a caller
    // supplied appId, socket path, host or port.
    bool submit(const RuntimeSession& session, nlohmann::json params, Callback callback);

    std::size_t pending() const;
    int request_timeout_ms() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace edgedesk::extensions
