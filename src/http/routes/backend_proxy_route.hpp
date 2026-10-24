#pragma once

#include "http/middleware.hpp"

#include "HttpServer.h"

namespace edgedesk {

// Runtime HTTP-over-UDS proxy used by the sandboxed front end through the host
// JSON-RPC bridge (SPEC-0011 section 7). The session token is the only
// application identity input; the socket is never supplied by the client.
void register_backend_proxy_routes(hv::HttpService& router, RouteContext& ctx);

} // namespace edgedesk
