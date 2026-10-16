#include "app/http_server.hpp"

#include "app/asset_provider.hpp"
#include "HttpServer.h"

#include <iostream>

namespace edgedesk {

int run_server(const Config& config) {
    AssetProvider assets(config.web_root);
    hv::HttpService router;

    router.GET("/api/health", [](HttpRequest*, HttpResponse* resp) {
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->headers["Cache-Control"] = "no-store";
        resp->body = R"({"status":"ok","service":"edgedesk","phase":1})";
        return 200;
    });

    router.staticHandler = [&assets](HttpRequest* req, HttpResponse* resp) -> int {
        const auto result = assets.read(req->path);
        if (!result) return 404;
        resp->headers["Content-Type"] = result->mime;
        resp->headers["X-Content-Type-Options"] = "nosniff";
        resp->headers["Cache-Control"] = "no-cache";
        if (!result->etag.empty()) {
            resp->headers["ETag"] = result->etag;
        }
        if (req->method == HTTP_GET) {
            // Own the bytes in HttpResponse; no dangling temporary buffers.
            resp->body = result->body;
        } else {
            // For HEAD, libhv skips sending the body; preserve reported length.
            resp->content_length = result->body.size();
        }
        return 200;
    };

    hv::HttpServer server(&router);
    server.setHost(config.host.c_str());
    server.setPort(config.port);
    server.setThreadNum(1);
    std::cout << "EdgeDesk Phase 1 listening on http://" << config.host << ":" << config.port
              << " (loopback-only, authentication NOT implemented)" << std::endl;
    return server.run();
}

} // namespace edgedesk
