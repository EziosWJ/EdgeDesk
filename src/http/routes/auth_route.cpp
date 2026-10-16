#include "http/routes/auth_route.hpp"

#include "core/util.hpp"

namespace edgedesk {

void register_auth_routes(hv::HttpService& router, RouteContext& ctx) {
    router.POST("/api/auth/login", [&ctx](HttpRequest* req, HttpResponse* resp) -> int {
        const std::string ip = http::client_ip(req);
        if (!ctx.login_limiter->allow(ip, now_unix_millis())) {
            ctx.audit->record("login", "throttled", "", ip);
            resp->headers["Retry-After"] = "60";
            return http::write_error(resp, 429, "rate_limited", "too many login attempts; try again later");
        }

        hv::Json body;
        Error error;
        if (!http::parse_json_request(req, body, error)) {
            ctx.audit->record("login", "invalid_request", error.message, ip);
            return http::write_error(resp, error);
        }
        if (!body.contains("token") || !body["token"].is_string()) {
            ctx.audit->record("login", "invalid_request", "missing token", ip);
            return http::write_error(resp, 400, "invalid_request", "field 'token' must be a string");
        }
        const std::string token = body["token"].get<std::string>();
        if (!ctx.auth->verify_token(token)) {
            ctx.audit->record("login", "denied", "invalid token", ip);
            return http::write_error(resp, 401, "invalid_token", "access token is invalid");
        }
        const std::string session = ctx.auth->issue_session(now_unix_seconds());
        resp->headers["Set-Cookie"] = ctx.auth->session_cookie(session);
        ctx.audit->record("login", "ok", "", ip);
        hv::Json result;
        result["ok"] = true;
        result["expires_in"] = ctx.auth->ttl_seconds();
        return http::write_json(resp, 200, result);
    });

    router.POST("/api/auth/logout", [&ctx](HttpRequest* req, HttpResponse* resp) -> int {
        resp->headers["Set-Cookie"] = ctx.auth->expired_session_cookie();
        ctx.audit->record("logout", "ok", "", http::client_ip(req));
        hv::Json result;
        result["ok"] = true;
        return http::write_json(resp, 200, result);
    });

    router.GET("/api/auth/session", [&ctx](HttpRequest* req, HttpResponse* resp) -> int {
        bool used_cookie = false;
        const bool authenticated = request_is_authenticated(ctx, req, used_cookie);
        hv::Json result;
        result["ok"] = true;
        result["authenticated"] = authenticated;
        result["method"] = authenticated ? (used_cookie ? "cookie" : "bearer") : "none";
        return http::write_json(resp, 200, result);
    });
}

} // namespace edgedesk
