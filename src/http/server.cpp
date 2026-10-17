#include "http/server.hpp"

#include "http/routes/auth_route.hpp"
#include "http/routes/file_route.hpp"
#include "http/routes/health_route.hpp"
#include "http/routes/settings_route.hpp"
#include "http/routes/system_route.hpp"
#include "http/routes/terminal_route.hpp"
#include "http/ws_state.hpp"

#include "HttpServer.h"
#include "WebSocketServer.h"

#include <iostream>
#include <string>

namespace edgedesk {
namespace {

bool has_extension(std::string_view path) {
    const auto slash = path.rfind('/');
    const auto dot = path.rfind('.');
    return dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash);
}

} // namespace

Server::Server(RouteContext context, const Config& config) : context_(context), config_(config) {}

void Server::stop() {
    if (server_handle_ != nullptr) {
        auto* server = static_cast<hv::HttpServer*>(server_handle_);
        server->stop();
    }
}

int Server::run() {
    hv::HttpService router;
    router.enable_access_log = 0;

    install_middleware(router, context_);
    register_health_routes(router, context_);
    register_auth_routes(router, context_);
    register_file_routes(router, context_);
    register_terminal_routes(router, context_);
    register_system_routes(router, context_);
    register_settings_routes(router, context_);

    // Static frontend bundle with SPA fallback (never touches device files).
    router.staticHandler = [this](HttpRequest* req, HttpResponse* resp) -> int {
        if (req->method != HTTP_GET && req->method != HTTP_HEAD) return 405;
        const std::string path = req->Path();
        const bool api_path = path.rfind("/api/", 0) == 0 || path.rfind("/ws/", 0) == 0;
        auto asset = context_.web_root->read(path);
        if (!asset && !has_extension(path) && !api_path) {
            // SPA fallback: unknown frontend routes render the React shell.
            asset = context_.web_root->read("/index.html");
        }
        if (!asset) {
            if (api_path) {
                return http::write_error(resp, 404, "not_found", "unknown API endpoint");
            }
            resp->status_code = static_cast<http_status>(404);
            resp->headers["Content-Type"] = "text/plain; charset=utf-8";
            resp->body = "not found";
            return 404;
        }
        resp->headers["Content-Type"] = asset->mime;
        resp->headers["X-Content-Type-Options"] = "nosniff";
        if (asset->immutable) {
            resp->headers["Cache-Control"] = "public, max-age=31536000, immutable";
        } else {
            resp->headers["Cache-Control"] = "no-cache";
        }
        if (!asset->etag.empty()) {
            resp->headers["ETag"] = asset->etag;
            const std::string if_none_match = req->GetHeader("If-None-Match", "");
            if (!if_none_match.empty() && if_none_match == asset->etag) {
                resp->body.clear();
                resp->content_length = 0;
                resp->status_code = static_cast<http_status>(304);
                return 304;
            }
        }
        if (req->method == HTTP_GET) {
            resp->body = std::move(asset->body);
        } else {
            resp->content_length = asset->body.size();
        }
        return 200;
    };

    hv::WebSocketService websocket;
    websocket.setPingInterval(30000);
    websocket.onopen = [this](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
        if (req->Path() == "/ws/terminal") {
            ws_terminal_onopen(context_, channel, req);
        } else if (req->Path() == "/ws/metrics") {
            ws_metrics_onopen(context_, channel, req);
        } else {
            channel->close();
        }
    };
    websocket.onmessage = [this](const WebSocketChannelPtr& channel, const std::string& message) {
        const auto state = channel->getContextPtr<WsClientState>();
        if (!state) {
            channel->close();
            return;
        }
        if (state->path == "/ws/terminal") {
            ws_terminal_onmessage(context_, channel, message);
        } else if (state->path == "/ws/metrics") {
            ws_metrics_onmessage(context_, channel, message);
        }
    };
    websocket.onclose = [this](const WebSocketChannelPtr& channel) {
        const auto state = channel->getContextPtr<WsClientState>();
        if (!state) return;
        if (state->path == "/ws/terminal") {
            ws_terminal_onclose(context_, channel);
        } else if (state->path == "/ws/metrics") {
            ws_metrics_onclose(context_, channel);
        }
    };

    hv::WebSocketServer server(&websocket);
    server.registerHttpService(&router);
    server.setHost(config_.host.c_str());
    server.setPort(config_.port);
    server.setThreadNum(config_.http_threads);
    server_handle_ = &server;

    std::cout << "EdgeDesk " EDGEDESK_VERSION << " listening on http://" << config_.host << ":" << config_.port
              << " (auth: " << auth_state_name(context_.auth->state()) << ", assets: "
              << (context_.web_root->embedded() ? "embed" : "disk") << ")" << std::endl;
    const int result = server.run();
    server_handle_ = nullptr;
    return result;
}

} // namespace edgedesk
