#include "http/routes/native_backend_route.hpp"

#include "http/http_util.hpp"

namespace edgedesk {
namespace {

int not_implemented(HttpResponse* resp) {
    return http::write_error(resp, 503, "native_backend_unavailable",
                             "native backend lifecycle is not implemented");
}

} // namespace

// APPX-03 implements the lifecycle handlers. Registration and paths are part of
// the shared contract so the admin UI can be developed against them.
void register_native_backend_routes(hv::HttpService& router, RouteContext& ctx) {
    (void)ctx;
    router.GET("/api/extensions/:app_id/backend",
               [](HttpRequest*, HttpResponse* resp) { return not_implemented(resp); });
    router.PUT("/api/extensions/:app_id/backend/identity",
               [](HttpRequest*, HttpResponse* resp) { return not_implemented(resp); });
    router.PUT("/api/extensions/:app_id/backend/autostart",
               [](HttpRequest*, HttpResponse* resp) { return not_implemented(resp); });
    router.POST("/api/extensions/:app_id/backend/start",
                [](HttpRequest*, HttpResponse* resp) { return not_implemented(resp); });
    router.POST("/api/extensions/:app_id/backend/stop",
                [](HttpRequest*, HttpResponse* resp) { return not_implemented(resp); });
    router.POST("/api/extensions/:app_id/backend/restart",
                [](HttpRequest*, HttpResponse* resp) { return not_implemented(resp); });
    router.POST("/api/extensions/:app_id/data/purge",
                [](HttpRequest*, HttpResponse* resp) { return not_implemented(resp); });
}

} // namespace edgedesk
