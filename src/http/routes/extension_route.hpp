#pragma once

#include "http/middleware.hpp"

#include "HttpServer.h"

namespace edgedesk {

void register_extension_routes(hv::HttpService& router, RouteContext& ctx);

// Serves /apps/{appId}/{version}/{revisionId}/... from the active immutable
// revision. Returns an HTTP status code. Public: the sandboxed iframe has an
// opaque origin and cannot present the host session cookie.
int serve_extension_asset(RouteContext& ctx, HttpRequest* req, HttpResponse* resp);

} // namespace edgedesk
