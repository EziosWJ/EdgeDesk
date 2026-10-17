#include "http/routes/terminal_route.hpp"

#include "core/utf8.hpp"
#include "core/util.hpp"
#include "http/ws_state.hpp"

#include <algorithm>
#include <string_view>

namespace edgedesk {
namespace {

constexpr std::size_t kMaxWsMessage = 256 * 1024;
constexpr std::size_t kMaxWsBacklog = 2 * 1024 * 1024;

hv::Json session_info_json(const TerminalSessionInfo& info) {
    hv::Json json;
    json["id"] = info.id;
    json["title"] = info.title;
    json["cwd"] = info.cwd;
    json["created_ms"] = info.created_ms;
    json["last_activity_ms"] = info.last_activity_ms;
    json["cols"] = info.cols;
    json["rows"] = info.rows;
    json["running"] = info.running;
    json["exited"] = info.exited;
    json["exit_code"] = info.exit_code;
    json["subscribers"] = info.subscribers;
    json["buffered_bytes"] = info.buffered_bytes;
    json["pending_confirmation"] = info.pending_confirmation;
    return json;
}

std::string dump_json(const hv::Json& body) {
    // Replace invalid UTF-8 instead of letting nlohmann::json::dump throw: this
    // callback runs on the PTY reader thread, where an exception terminates the
    // process.
    return body.dump(-1, ' ', false, hv::Json::error_handler_t::replace);
}

void send_json(const WebSocketChannelPtr& channel, const hv::Json& body) {
    channel->send(dump_json(body), WS_OPCODE_TEXT);
}

void send_error(const WebSocketChannelPtr& channel, const std::string& code, const std::string& message) {
    hv::Json body;
    body["type"] = "error";
    body["code"] = code;
    body["message"] = message;
    send_json(channel, body);
}

void detach_session(const std::shared_ptr<WsClientState>& state) {
    if (state->session && state->subscription != 0) {
        state->session->unsubscribe(state->subscription);
    }
    state->subscription = 0;
    state->session_id.clear();
    state->session.reset();
}

void attach_session(RouteContext& ctx, const WebSocketChannelPtr& channel,
                    const std::shared_ptr<WsClientState>& state, const std::string& session_id) {
    std::shared_ptr<TerminalSession> session = ctx.terminals->find(session_id);
    if (!session) {
        send_error(channel, "session_not_found", "terminal session is gone or was never created");
        return;
    }
    detach_session(state);

    hv::Json attached;
    attached["type"] = "attached";
    attached["session"] = session_info_json(session->info());
    send_json(channel, attached);

    const std::weak_ptr<hv::WebSocketChannel> weak_channel(channel);
    // A single subscription spans backlog replay and live output. Keep one
    // sanitizer so a UTF-8 sequence split across the replay/live boundary (or
    // between PTY reads) is reassembled instead of becoming U+FFFD.
    auto sanitizer = std::make_shared<Utf8StreamSanitizer>();
    state->session = session;
    state->session_id = session_id;
    state->subscription = session->subscribe(TerminalSession::Subscriber{
        [weak_channel, sanitizer](const char* data, std::size_t length) {
            const auto target = weak_channel.lock();
            if (!target) return;
            if (target->writeBufsize() > kMaxWsBacklog) {
                // Slow client: drop the connection instead of buffering forever.
                target->close();
                return;
            }
            const std::string text = sanitizer->feed(std::string_view(data, length));
            if (text.empty()) return;
            hv::Json body;
            body["type"] = "output";
            body["data"] = text;
            target->send(dump_json(body), WS_OPCODE_TEXT);
        },
        [weak_channel, sanitizer](int exit_code) {
            const auto target = weak_channel.lock();
            if (!target) return;
            const std::string tail = sanitizer->finish();
            if (!tail.empty()) {
                hv::Json output;
                output["type"] = "output";
                output["data"] = tail;
                target->send(dump_json(output), WS_OPCODE_TEXT);
            }
            hv::Json body;
            body["type"] = "exit";
            body["code"] = exit_code;
            target->send(dump_json(body), WS_OPCODE_TEXT);
        }});
    if (state->subscription == 0) {
        state->session.reset();
        state->session_id.clear();
        send_error(channel, "subscriber_limit", "too many viewers for this terminal session");
    }
}

void handle_create(RouteContext& ctx, const WebSocketChannelPtr& channel, const std::shared_ptr<WsClientState>& state,
                   const hv::Json& message) {
    TerminalManager::CreateRequest request;
    request.title = message.value("title", "");
    request.cwd = message.value("cwd", "");
    request.cols = static_cast<unsigned short>(std::clamp(message.value("cols", 80), 1, 1000));
    request.rows = static_cast<unsigned short>(std::clamp(message.value("rows", 24), 1, 1000));
    std::shared_ptr<TerminalSession> session;
    Error error = ctx.terminals->create(request, session);
    if (!error.ok()) {
        send_error(channel, error_code_slug(error.code), error.message);
        return;
    }
    ctx.audit->record("terminal.create", "ok", session->title(), state->peer_ip, session->id());
    hv::Json created;
    created["type"] = "created";
    created["session"] = session_info_json(session->info());
    send_json(channel, created);
    attach_session(ctx, channel, state, session->id());
}

void handle_message(RouteContext& ctx, const WebSocketChannelPtr& channel, const std::shared_ptr<WsClientState>& state,
                    const hv::Json& message) {
    const std::string type = message.value("type", "");
    if (type == "create") {
        handle_create(ctx, channel, state, message);
        return;
    }
    if (type == "attach") {
        const std::string session_id = message.value("session_id", "");
        if (session_id.empty()) {
            send_error(channel, "invalid_argument", "field 'session_id' is required");
            return;
        }
        attach_session(ctx, channel, state, session_id);
        return;
    }
    if (type == "input") {
        if (!state->session) {
            send_error(channel, "not_attached", "attach or create a session first");
            return;
        }
        const std::string data = message.value("data", "");
        if (data.empty()) return;
        std::string reason;
        const auto outcome = state->session->submit_input(data, reason);
        if (outcome == TerminalSession::InputOutcome::guard_blocked) {
            hv::Json body;
            body["type"] = "guard";
            body["reason"] = reason;
            send_json(channel, body);
        } else if (outcome == TerminalSession::InputOutcome::rejected) {
            send_error(channel, "input_rejected", reason);
        }
        return;
    }
    if (type == "confirm") {
        if (state->session) {
            const bool confirmed = state->session->confirm_pending();
            hv::Json body;
            body["type"] = "confirmed";
            body["confirmed"] = confirmed;
            send_json(channel, body);
        }
        return;
    }
    if (type == "cancel") {
        if (state->session) {
            state->session->cancel_pending();
            hv::Json body;
            body["type"] = "cancelled";
            send_json(channel, body);
        }
        return;
    }
    if (type == "resize") {
        if (!state->session) return;
        const int cols = std::clamp(message.value("cols", 80), 1, 1000);
        const int rows = std::clamp(message.value("rows", 24), 1, 1000);
        Error error = state->session->resize(static_cast<unsigned short>(cols), static_cast<unsigned short>(rows));
        if (!error.ok()) send_error(channel, error_code_slug(error.code), error.message);
        return;
    }
    if (type == "close") {
        if (state->session) {
            ctx.audit->record("terminal.close", "ok", state->session->title(), state->peer_ip, state->session_id);
            const std::string session_id = state->session_id;
            detach_session(state);
            ctx.terminals->request_close(session_id);
        }
        return;
    }
    if (type == "ping") {
        hv::Json body;
        body["type"] = "pong";
        send_json(channel, body);
        return;
    }
    send_error(channel, "unknown_type", "unsupported message type: " + type);
}

} // namespace

void register_terminal_routes(hv::HttpService& router, RouteContext& ctx) {
    router.GET("/api/terminal/sessions", [&ctx](HttpRequest* req, HttpResponse* resp) -> int {
        (void)req;
        hv::Json body;
        body["ok"] = true;
        body["shell"] = ctx.terminals->shell();
        body["shell_available"] = ctx.terminals->shell_available();
        body["max_sessions"] = ctx.terminals->options().max_sessions;
        body["idle_timeout_seconds"] = ctx.terminals->options().idle_timeout_seconds;
        body["running"] = ctx.terminals->running_count();
        hv::Json sessions = hv::Json::array();
        for (const auto& info : ctx.terminals->list()) {
            sessions.push_back(session_info_json(info));
        }
        body["sessions"] = std::move(sessions);
        return http::write_json(resp, 200, body);
    });

    router.POST("/api/terminal/sessions", [&ctx](HttpRequest* req, HttpResponse* resp) -> int {
        hv::Json body;
        Error error;
        if (!http::parse_json_request(req, body, error)) return http::write_error(resp, error);
        TerminalManager::CreateRequest request;
        request.title = body.value("title", "");
        request.cwd = body.value("cwd", "");
        const int cols = std::clamp(body.value("cols", 80), 1, 1000);
        const int rows = std::clamp(body.value("rows", 24), 1, 1000);
        request.cols = static_cast<unsigned short>(cols);
        request.rows = static_cast<unsigned short>(rows);
        std::shared_ptr<TerminalSession> session;
        error = ctx.terminals->create(request, session);
        if (!error.ok()) {
            ctx.audit->record("terminal.create", "denied", error.message, http::client_ip(*ctx.config, req), request.cwd);
            return http::write_error(resp, error);
        }
        ctx.audit->record("terminal.create", "ok", session->title(), http::client_ip(*ctx.config, req), session->id());
        hv::Json result;
        result["ok"] = true;
        result["session"] = session_info_json(session->info());
        return http::write_json(resp, 201, result);
    });

    router.POST("/api/terminal/sessions/close", [&ctx](HttpRequest* req, HttpResponse* resp) -> int {
        hv::Json body;
        Error error;
        if (!http::parse_json_request(req, body, error)) return http::write_error(resp, error);
        const std::string session_id = body.value("session_id", "");
        if (session_id.empty()) return http::write_error(resp, 400, "invalid_argument", "field 'session_id' is required");
        const bool closed = ctx.terminals->request_close(session_id);
        if (!closed) return http::write_error(resp, 404, "not_found", "unknown terminal session");
        ctx.audit->record("terminal.close", "ok", "http", http::client_ip(*ctx.config, req), session_id);
        hv::Json result;
        result["ok"] = true;
        return http::write_json(resp, 200, result);
    });
}

void ws_terminal_onopen(RouteContext& ctx, const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
    auto state = std::make_shared<WsClientState>();
    state->path = req->Path();
    state->peer_ip = http::client_ip(*ctx.config, req.get());
    channel->setContextPtr(state);

    // Convenience: attach directly when the session id is in the query string.
    const std::string session_id = req->GetParam("session_id");
    if (!session_id.empty()) {
        attach_session(ctx, channel, state, session_id);
    } else {
        hv::Json ready;
        ready["type"] = "ready";
        ready["shell"] = ctx.terminals->shell();
        ready["shell_available"] = ctx.terminals->shell_available();
        ready["max_sessions"] = ctx.terminals->options().max_sessions;
        send_json(channel, ready);
    }
}

void ws_terminal_onmessage(RouteContext& ctx, const WebSocketChannelPtr& channel, const std::string& message) {
    auto state = channel->getContextPtr<WsClientState>();
    if (!state) return;
    if (message.size() > kMaxWsMessage) {
        send_error(channel, "too_large", "websocket message exceeds the limit");
        return;
    }
    hv::Json parsed;
    try {
        parsed = hv::Json::parse(message);
    } catch (const std::exception&) {
        send_error(channel, "invalid_json", "websocket messages must be JSON objects");
        return;
    }
    if (!parsed.is_object()) {
        send_error(channel, "invalid_json", "websocket messages must be JSON objects");
        return;
    }
    try {
        handle_message(ctx, channel, state, parsed);
    } catch (const std::exception& error) {
        send_error(channel, "internal", std::string("terminal handler error: ") + error.what());
    }
}

void ws_terminal_onclose(RouteContext& ctx, const WebSocketChannelPtr& channel) {
    auto state = channel->getContextPtr<WsClientState>();
    if (!state) return;
    state->closed = true;
    // The PTY keeps running: detaching only removes this viewer.
    (void)ctx;
    detach_session(state);
    channel->deleteContextPtr();
}

} // namespace edgedesk
