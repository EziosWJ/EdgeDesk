#include "http/middleware.hpp"

#include "http/upload_state.hpp"
#include "core/types.hpp"
#include "core/util.hpp"

#include <algorithm>

namespace edgedesk {
namespace {

constexpr const char* kCsp =
    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
    "img-src 'self' data:; font-src 'self' data:; connect-src 'self' ws: wss:; "
    "object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'";

void set_security_headers(HttpResponse* resp) {
    resp->headers["X-Content-Type-Options"] = "nosniff";
    resp->headers["X-Frame-Options"] = "DENY";
    resp->headers["Referrer-Policy"] = "no-referrer";
    resp->headers["Content-Security-Policy"] = kCsp;
}

} // namespace

void release_upload_state(const HttpContextPtr& context) {
    if (!context || context->userdata == nullptr) return;
    auto* state = static_cast<UploadRequestState*>(context->userdata);
    context->userdata = nullptr;
    delete state;
}

bool is_upload_path(std::string_view path) { return path == "/api/files/content"; }

std::uint64_t body_limit_for(const Config& config, std::string_view path) {
    if (is_upload_path(path)) return config.max_upload_size;
    if (path == "/api/files/text") return config.max_text_edit_size;
    return config.max_request_body;
}

bool path_is_public(const RouteContext& ctx, std::string_view path) {
    if (path.rfind("/api/", 0) != 0 && path.rfind("/ws/", 0) != 0) return true; // static bundle
    if (path == "/api/health" || path == "/api/auth/session" || path == "/api/auth/logout") return true;
    if (ctx.auth == nullptr) return false;
    const auto state = ctx.auth->state();
    if (path == "/api/auth/setup-status") return true;
    // Setup stays public so an already-initialized instance can return 409
    // instead of leaking an authentication-required response.
    if (path == "/api/auth/setup") return true;
    if (state == AuthState::initialized && path == "/api/auth/login") return true;
    return false;
}

bool request_is_authenticated(RouteContext& ctx, HttpRequest* req, bool& used_cookie) {
    used_cookie = false;
    if (ctx.auth == nullptr || ctx.auth->state() != AuthState::initialized) return false;
    const std::string authorization = req->GetHeader("Authorization", "");
    if (!authorization.empty()) {
        return ctx.auth->verify_bearer(authorization);
    }
    const auto cookie = http::cookie_value(req, Auth::cookie_name());
    if (cookie.has_value() && ctx.auth->verify_session(*cookie, now_unix_seconds())) {
        used_cookie = true;
        return true;
    }
    return false;
}

int auth_state_gate(RouteContext& ctx, HttpRequest* req, HttpResponse* resp, std::string_view path) {
    if (path.rfind("/api/", 0) != 0 && path.rfind("/ws/", 0) != 0) return HTTP_STATUS_NEXT;
    if (path_is_public(ctx, path)) return HTTP_STATUS_NEXT;
    if (ctx.auth == nullptr) {
        return http::write_error(resp, 503, "auth_unavailable", "authentication is unavailable");
    }
    const auto state = ctx.auth->state();
    if (state == AuthState::uninitialized) {
        ctx.audit->record("auth", "denied", "setup required", http::client_ip(*ctx.config, req), std::string(path));
        return http::write_error(resp, 503, "setup_required", "administrator initialization is required");
    }
    if (state == AuthState::corrupt) {
        ctx.audit->record("auth", "denied", "auth state corrupt", http::client_ip(*ctx.config, req),
                          std::string(path));
        return http::write_error(resp, 503, "auth_state_corrupt",
                                 "authentication state is corrupt; use local recovery");
    }
    return HTTP_STATUS_NEXT;
}

bool request_origin_ok(HttpRequest* req) {
    const std::string host = req->GetHeader("Host", "");
    if (host.empty()) return false;
    const std::string origin = req->GetHeader("Origin", "");
    if (!origin.empty()) return http::origin_matches_host(origin, host);
    const std::string referer = req->GetHeader("Referer", "");
    if (!referer.empty()) return http::referer_matches_host(referer, host);
    return false;
}

void install_middleware(hv::HttpService& router, RouteContext& ctx) {
    // Header stage: runs before any body bytes are read. Only "hard"
    // rejections happen here (the connection is closed without a friendly
    // response) because libhv cannot send an error response at this point.
    // Used for the streaming upload (must not write a byte before auth) and
    // for WebSocket upgrades.
    router.headerHandler = [&ctx](HttpRequest* req, HttpResponse* resp) -> int {
        set_security_headers(resp);
        const std::string path = req->Path();
        const std::uint64_t content_length = static_cast<std::uint64_t>(req->content_length);
        const std::string ip = http::client_ip(*ctx.config, req);
        bool used_cookie = false;
        const bool authenticated = request_is_authenticated(ctx, req, used_cookie);
        const bool origin_ok = request_origin_ok(req);
        const auto auth_state = ctx.auth == nullptr ? AuthState::corrupt : ctx.auth->state();

        if (is_upload_path(path) || path.rfind("/ws/", 0) == 0) {
            if (auth_state != AuthState::initialized) {
                ctx.audit->record(is_upload_path(path) ? "auth" : "ws_auth", "denied",
                                  auth_state == AuthState::uninitialized ? "setup required" : "auth state corrupt", ip,
                                  path);
                resp->status_code = static_cast<http_status>(503);
                resp->body = auth_state == AuthState::uninitialized ? "administrator initialization is required"
                                                                    : "authentication state is corrupt";
                return resp->status_code;
            }
            // Uploads write straight to disk and upgrades switch protocols, so
            // both must be fully validated before anything is consumed.
            if (is_upload_path(path)) {
                if (!ctx.api_limiter->allow(ip, now_unix_millis())) {
                    ctx.audit->record("rate_limit", "denied", path, ip);
                    resp->body = "too many requests";
                    return 429;
                }
                if (content_length > ctx.config->max_upload_size) {
                    ctx.audit->record("file.upload", "denied", "content-length over limit", ip, path);
                    resp->body = "upload exceeds configured limit";
                    return 413;
                }
            }
            const char* event = is_upload_path(path) ? "auth" : "ws_auth";
            if (!authenticated) {
                ctx.audit->record(event, "denied", path, ip);
                resp->body = "authentication required";
                return 401;
            }
            if (!origin_ok) {
                ctx.audit->record(is_upload_path(path) ? "csrf" : "ws_origin", "denied", path, ip);
                resp->body = "origin check failed";
                return 403;
            }
            return HTTP_STATUS_OK;
        }

        const std::uint64_t hard_cap =
            std::max<std::uint64_t>({ctx.config->max_request_body, ctx.config->max_text_edit_size,
                                     4ull * 1024 * 1024});
        if (content_length > hard_cap) {
            resp->body = "request body too large";
            return 413;
        }
        return HTTP_STATUS_OK;
    };

    // Message stage: the whole body is available and a normal JSON response can
    // be produced. The upload path was fully handled above.
    router.Use([&ctx](const HttpContextPtr& context) -> int {
        HttpRequest* req = context->request.get();
        HttpResponse* resp = context->response.get();
        const std::string path = req->Path();
        const std::string ip = http::client_ip(*ctx.config, req);

        if (is_upload_path(path)) {
            // Auth/CSRF/limits were enforced at the header stage; a state
            // handler owns the request from here.
            return HTTP_STATUS_NEXT;
        }

        if (!ctx.api_limiter->allow(ip, now_unix_millis())) {
            // Defensive: never leave partial upload state behind a rejection.
            release_upload_state(context);
            ctx.audit->record("rate_limit", "denied", path, ip);
            return http::write_error(resp, 429, "rate_limited", "too many requests; slow down");
        }

        const int gate = auth_state_gate(ctx, req, resp, path);
        if (gate != HTTP_STATUS_NEXT) {
            release_upload_state(context);
            return gate;
        }

        if (!path_is_public(ctx, path)) {
            bool used_cookie = false;
            if (!request_is_authenticated(ctx, req, used_cookie)) {
                release_upload_state(context);
                ctx.audit->record("auth", "denied", path, ip);
                resp->headers["WWW-Authenticate"] = "Bearer realm=\"edgedesk\"";
                return http::write_error(resp, 401, "unauthorized", "authentication required");
            }
            const auto method = req->method;
            const bool unsafe_method = method != HTTP_GET && method != HTTP_HEAD && method != HTTP_OPTIONS;
            if (used_cookie && unsafe_method && !request_origin_ok(req)) {
                release_upload_state(context);
                ctx.audit->record("csrf", "denied", path, ip);
                return http::write_error(resp, 403, "csrf_rejected", "origin check failed");
            }
            const std::uint64_t limit = body_limit_for(*ctx.config, path);
            if (static_cast<std::uint64_t>(req->content_length) > limit ||
                static_cast<std::uint64_t>(req->body.size()) > limit) {
                release_upload_state(context);
                return http::write_error(resp, 413, "too_large", "request body exceeds the configured limit");
            }
        }
        return HTTP_STATUS_NEXT;
    });
}

} // namespace edgedesk
