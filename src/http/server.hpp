#pragma once

#include "core/config.hpp"
#include "http/middleware.hpp"

namespace edgedesk {

// Owns the libhv router + WebSocket service wiring. All business logic lives in
// the injected services (RouteContext).
class Server {
public:
    Server(RouteContext context, const Config& config);

    // Blocks until the server stops or a signal triggers shutdown.
    int run();

    void stop();

private:
    RouteContext context_;
    const Config& config_;
    void* server_handle_ = nullptr; // hv::HttpServer*, kept opaque in the header
};

} // namespace edgedesk
