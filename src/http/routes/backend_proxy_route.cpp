#include "http/routes/backend_proxy_route.hpp"

#include "http/http_util.hpp"

namespace edgedesk {
namespace {

int not_implemented(HttpResponse* resp) {
    return http::write_error(resp, 503, "backend_unavailable",
                             "backend proxy is not implemented");
}

} // namespace

// APPX-04 implements the JSON-RPC-facing endpoint. The path is fixed here so the
// host bridge and C++ runtime share one contract:
//   POST /api/extensions/runtime/backend/request
//   {"token","method","path","body","contentType"} -> {status,headers,body}
void register_backend_proxy_routes(hv::HttpService& router, RouteContext& ctx) {
    (void)ctx;
    router.POST("/api/extensions/runtime/backend/request",
                [](HttpRequest*, HttpResponse* resp) { return not_implemented(resp); });
}

} // namespace edgedesk
