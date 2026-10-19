#pragma once

#include "http/middleware.hpp"

#include "HttpServer.h"

namespace edgedesk {

void register_extension_routes(hv::HttpService& router, RouteContext& ctx);

// Streams /apps/{appId}/{version}/{revisionId}/... from the active immutable
// revision. The FD is opened by PackageManager (PathSandbox) and streamed in
// bounded chunks; the whole asset is never buffered. Public: the sandboxed
// iframe has an opaque origin and cannot present the host session cookie.
void serve_extension_asset_stream(RouteContext& ctx, const HttpRequestPtr& req,
                                  const HttpResponseWriterPtr& writer, bool head);

} // namespace edgedesk
