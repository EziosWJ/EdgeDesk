#pragma once

#include "http/middleware.hpp"

#include "HttpServer.h"

namespace edgedesk {

// Administrator lifecycle endpoints for optional native backends (SPEC-0011
// section 6.2). All routes require the normal admin authentication/CSRF
// middleware; they are registered separately from the runtime proxy routes.
void register_native_backend_routes(hv::HttpService& router, RouteContext& ctx);

} // namespace edgedesk
