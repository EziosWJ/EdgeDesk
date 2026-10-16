#pragma once

#include "http/middleware.hpp"

#include "HttpServer.h"

namespace edgedesk {

void register_auth_routes(hv::HttpService& router, RouteContext& ctx);

} // namespace edgedesk
