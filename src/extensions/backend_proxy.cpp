#include "extensions/backend_proxy.hpp"

#include "extensions/extension_store.hpp"

namespace edgedesk::extensions {

struct BackendProxy::Impl {
    ExtensionStore& store;
    std::string apps_root;
};

BackendProxy::BackendProxy(ExtensionStore& store, std::string apps_root, std::size_t workers,
                           std::size_t queue_limit)
    : impl_(new Impl{store, std::move(apps_root)}) {
    (void)workers;
    (void)queue_limit;
}

BackendProxy::~BackendProxy() = default;

// APPX-04 implements the real bounded worker and HTTP-over-UDS client.
bool BackendProxy::submit(const RuntimeSession& session, nlohmann::json params, Callback callback) {
    (void)session;
    (void)params;
    if (callback) {
        callback(Error::make(ErrorCode::unsupported, "backend proxy is not implemented"),
                 BackendHttpResponse{});
    }
    return true;
}

std::size_t BackendProxy::pending() const { return 0; }

int BackendProxy::request_timeout_ms() const { return 10000; }

} // namespace edgedesk::extensions
