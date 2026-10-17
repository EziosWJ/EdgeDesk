#include "http/routes/auth_route.hpp"

#include "core/util.hpp"

namespace edgedesk {
namespace {

int write_api_error(const HttpResponseWriterPtr& writer, int status, const std::string& code,
                    const std::string& message) {
    hv::Json body;
    body["ok"] = false;
    body["error"] = code;
    body["message"] = message;
    return http::write_json_to(writer, status, body);
}

int error_status(const Error& error) {
    switch (error.code) {
        case ErrorCode::invalid_argument: return 400;
        case ErrorCode::permission_denied: return 400;
        case ErrorCode::conflict: return 409;
        case ErrorCode::busy:
        case ErrorCode::limit_exceeded: return 429;
        case ErrorCode::unavailable: return 503;
        default: return http_status_for(error.code);
    }
}

std::string error_slug(const Error& error) {
    switch (error.code) {
        case ErrorCode::permission_denied: return "invalid_credentials";
        case ErrorCode::conflict: return "already_initialized";
        case ErrorCode::busy:
        case ErrorCode::limit_exceeded: return "rate_limited";
        case ErrorCode::unavailable: return "auth_unavailable";
        default: return error_code_slug(error.code);
    }
}

bool string_field(const hv::Json& body, const char* key, std::string& out) {
    if (!body.contains(key)) {
        out.clear();
        return true;
    }
    if (!body[key].is_string()) return false;
    out = body[key].get<std::string>();
    return true;
}

bool rate_limited(RouteContext& ctx, RateLimiter* limiter, HttpRequest* req, std::string_view event) {
    const auto ip = http::client_ip(*ctx.config, req);
    if (limiter->allow(ip, now_unix_millis())) return false;
    ctx.audit->record(std::string(event), "throttled", "", ip);
    return true;
}

hv::Json session_status_body(RouteContext& ctx, HttpRequest* req) {
    bool authenticated = false;
    bool used_cookie = false;
    std::int64_t expires_at = 0;
    const auto state = ctx.auth->state();
    if (state == AuthState::initialized) {
        const std::string authorization = req->GetHeader("Authorization", "");
        if (!authorization.empty()) {
            authenticated = ctx.auth->verify_bearer(authorization);
        } else {
            const auto cookie = http::cookie_value(req, Auth::cookie_name());
            authenticated = cookie.has_value() &&
                            ctx.auth->verify_session(*cookie, now_unix_seconds(), &expires_at);
            used_cookie = authenticated;
        }
    }
    hv::Json body;
    body["ok"] = true;
    body["authenticated"] = authenticated;
    body["method"] = authenticated ? (used_cookie ? "cookie" : "bearer") : "none";
    body["state"] = auth_state_name(state);
    if (authenticated && expires_at > 0) body["expires_at"] = expires_at;
    return body;
}

} // namespace

void register_auth_routes(hv::HttpService& router, RouteContext& ctx) {
    router.GET("/api/auth/setup-status", [&ctx](HttpRequest*, HttpResponse* resp) -> int {
        hv::Json body;
        body["ok"] = true;
        body["initialized"] = ctx.auth->state() == AuthState::initialized;
        body["state"] = auth_state_name(ctx.auth->state());
        return http::write_json(resp, 200, body);
    });

    router.POST("/api/auth/setup", [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        if (rate_limited(ctx, ctx.setup_limiter, req.get(), "auth.setup")) {
            write_api_error(writer, 429, "rate_limited", "too many initialization attempts; try again later");
            return;
        }
        hv::Json body;
        Error parse_error;
        if (!http::parse_json_request(req.get(), body, parse_error)) {
            http::write_error_to(writer, parse_error);
            return;
        }
        std::string code;
        std::string password;
        std::string confirm;
        if (!string_field(body, "code", code) || !string_field(body, "password", password) ||
            !string_field(body, "confirm_password", confirm)) {
            write_api_error(writer, 400, "invalid_request", "setup fields must be strings");
            return;
        }
        if (code.empty() || password.empty()) {
            write_api_error(writer, 400, "invalid_request", "fields 'code' and 'password' are required");
            return;
        }
        if (password != confirm) {
            write_api_error(writer, 400, "password_mismatch", "password confirmation does not match");
            return;
        }
        const auto ip = http::client_ip(*ctx.config, req.get());
        ctx.auth->setup_async(
            code, password,
            [&ctx, writer, ip](Error error) {
                if (!error.ok()) {
                    ctx.audit->record("auth.setup", "denied", error.message, ip);
                    write_api_error(writer, error_status(error), error_slug(error), error.message);
                    return;
                }
                ctx.audit->record("auth.setup", "ok", "", ip);
                hv::Json result;
                result["ok"] = true;
                result["initialized"] = true;
                http::write_json_to(writer, 201, result);
            });
    });

    router.POST("/api/auth/login", [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        if (rate_limited(ctx, ctx.login_limiter, req.get(), "login")) {
            writer->Begin();
            writer->WriteStatus(HTTP_STATUS_TOO_MANY_REQUESTS);
            writer->WriteHeader("Retry-After", "60");
            writer->WriteHeader("Content-Type", "application/json; charset=utf-8");
            const std::string text = R"({"ok":false,"error":"rate_limited","message":"too many login attempts; try again later"})";
            writer->WriteHeader("Content-Length", text.size());
            writer->EndHeaders();
            writer->WriteBody(text);
            writer->End();
            return;
        }
        hv::Json body;
        Error parse_error;
        if (!http::parse_json_request(req.get(), body, parse_error)) {
            http::write_error_to(writer, parse_error);
            return;
        }
        if (!body.contains("password") || !body["password"].is_string()) {
            write_api_error(writer, 400, "invalid_request", "field 'password' must be a string");
            return;
        }
        const auto ip = http::client_ip(*ctx.config, req.get());
        ctx.auth->login_async(
            body["password"].get<std::string>(),
            [&ctx, writer, ip](Error error, std::string cookie, std::int64_t expires_at) {
                if (!error.ok()) {
                    ctx.audit->record("login", "denied", error.message, ip);
                    if (error.code == ErrorCode::permission_denied) {
                        write_api_error(writer, 401, "invalid_credentials", "invalid credentials");
                    } else if (error.code == ErrorCode::unavailable) {
                        write_api_error(writer, 503, "auth_unavailable", error.message);
                    } else {
                        write_api_error(writer, error_status(error), error_slug(error), error.message);
                    }
                    return;
                }
                ctx.audit->record("login", "ok", "", ip);
                hv::Json result;
                result["ok"] = true;
                result["expires_in"] = ctx.auth->ttl_seconds();
                result["expires_at"] = expires_at;
                http::write_json_to(writer, 200, result, ctx.auth->session_cookie(cookie));
            });
    });

    router.POST("/api/auth/logout", [&ctx](HttpRequest* req, HttpResponse* resp) -> int {
        const auto cookie = http::cookie_value(req, Auth::cookie_name());
        if (cookie.has_value() && !request_origin_ok(req)) {
            ctx.audit->record("logout", "denied", "origin check failed", http::client_ip(*ctx.config, req));
            return http::write_error(resp, 403, "csrf_rejected", "origin check failed");
        }
        resp->headers["Set-Cookie"] = ctx.auth->expired_session_cookie();
        ctx.audit->record("logout", "ok", "", http::client_ip(*ctx.config, req));
        hv::Json result;
        result["ok"] = true;
        return http::write_json(resp, 200, result);
    });

    router.GET("/api/auth/session", [&ctx](HttpRequest* req, HttpResponse* resp) -> int {
        return http::write_json(resp, 200, session_status_body(ctx, req));
    });

    router.POST("/api/auth/password", [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        const auto cookie = http::cookie_value(req.get(), Auth::cookie_name());
        std::int64_t expires_at = 0;
        if (!cookie.has_value() || !ctx.auth->verify_session(*cookie, now_unix_seconds(), &expires_at) ||
            ctx.auth->state() != AuthState::initialized) {
            write_api_error(writer, 401, "unauthorized", "a browser session cookie is required");
            return;
        }
        if (rate_limited(ctx, ctx.login_limiter, req.get(), "auth.password_change")) {
            write_api_error(writer, 429, "rate_limited", "too many password change attempts; try again later");
            return;
        }
        hv::Json body;
        Error parse_error;
        if (!http::parse_json_request(req.get(), body, parse_error)) {
            http::write_error_to(writer, parse_error);
            return;
        }
        std::string current;
        std::string next;
        std::string confirm;
        if (!string_field(body, "current_password", current) || !string_field(body, "new_password", next) ||
            !string_field(body, "confirm_password", confirm)) {
            write_api_error(writer, 400, "invalid_request", "password fields must be strings");
            return;
        }
        if (current.empty() || next.empty()) {
            write_api_error(writer, 400, "invalid_request",
                            "fields 'current_password' and 'new_password' are required");
            return;
        }
        if (next != confirm) {
            write_api_error(writer, 400, "password_mismatch", "password confirmation does not match");
            return;
        }
        const auto ip = http::client_ip(*ctx.config, req.get());
        ctx.auth->change_password_async(
            current, next,
            [&ctx, writer, ip](Error error) {
                if (!error.ok()) {
                    ctx.audit->record("auth.password_change", "denied", error.message, ip);
                    if (error.code == ErrorCode::permission_denied) {
                        write_api_error(writer, 400, "invalid_current_password", "current password is incorrect");
                    } else {
                        write_api_error(writer, error_status(error), error_slug(error), error.message);
                    }
                    return;
                }
                ctx.audit->record("auth.password_change", "ok", "", ip);
                hv::Json result;
                result["ok"] = true;
                result["reauthenticate"] = true;
                http::write_json_to(writer, 200, result, ctx.auth->expired_session_cookie());
            });
    });
}

} // namespace edgedesk
