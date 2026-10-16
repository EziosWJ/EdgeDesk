#pragma once

#include "http/middleware.hpp"

#include "HttpServer.h"
#include "WebSocketServer.h"

namespace edgedesk {

void register_terminal_routes(hv::HttpService& router, RouteContext& ctx);

void ws_terminal_onopen(RouteContext& ctx, const WebSocketChannelPtr& channel, const HttpRequestPtr& req);
void ws_terminal_onmessage(RouteContext& ctx, const WebSocketChannelPtr& channel, const std::string& message);
void ws_terminal_onclose(RouteContext& ctx, const WebSocketChannelPtr& channel);

} // namespace edgedesk
