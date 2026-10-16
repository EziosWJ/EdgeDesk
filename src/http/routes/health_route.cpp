#include "http/routes/health_route.hpp"

#include "core/util.hpp"
#include "system/system_metrics.hpp"

namespace edgedesk {

void register_health_routes(hv::HttpService& router, RouteContext& ctx) {
    router.GET("/api/health", [&ctx](HttpRequest*, HttpResponse* resp) -> int {
        hv::Json body;
        body["status"] = "ok";
        body["service"] = "edgedesk";
        body["phase"] = 1; // Phase 1 contract field kept stable
        body["version"] = "1.0.0";
        const auto sample = ctx.metrics->latest();
        if (sample.timestamp_ms > 0) {
            body["uptime_seconds"] = sample.uptime_seconds;
        }
        return http::write_json(resp, 200, body);
    });
}

} // namespace edgedesk
