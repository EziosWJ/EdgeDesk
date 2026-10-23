#include "http/routes/backend_proxy_route.hpp"

#include "core/error.hpp"
#include "extensions/backend_proxy.hpp"
#include "extensions/extension_runtime.hpp"
#include "http/http_util.hpp"

#include <string>
#include <utility>

namespace edgedesk {
namespace {

// Stable slugs travel in Error.message ("slug: detail") so the proxy can use
// the existing ErrorCode vocabulary without inventing enum values. Only a
// known proxy slug is accepted; any other message keeps the generic error code
// (a raw message like "invalid JSON body: ..." must not become the code).
bool known_slug(const std::string& slug) {
    return slug == "backend_not_configured" || slug == "backend_not_running" ||
           slug == "backend_unavailable" || slug == "backend_timeout" ||
           slug == "backend_protocol_error" || slug == "invalid_argument" ||
           slug == "permission_denied";
}

std::string error_slug(const Error& error) {
    const std::size_t colon = error.message.find(':');
    if (colon != std::string::npos && colon > 0) {
        const std::string candidate = error.message.substr(0, colon);
        if (known_slug(candidate)) return candidate;
    }
    return error_code_slug(error.code);
}

int status_for_slug(const std::string& slug, ErrorCode code) {
    if (slug == "backend_not_configured") return 404;
    if (slug == "backend_not_running") return 409;
    if (slug == "backend_timeout") return 504;
    if (slug == "backend_protocol_error") return 502;
    if (slug == "backend_unavailable") return 503;
    if (slug == "invalid_argument") return 400;
    if (slug == "permission_denied") return 403;
    return http_status_for(code);
}

int write_code(const HttpResponseWriterPtr& writer, int status, const std::string& code,
               const std::string& message) {
    hv::Json body;
    body["ok"] = false;
    body["error"] = code;
    body["message"] = message;
    return http::write_json_to(writer, status, body);
}

int write_proxy_error(const HttpResponseWriterPtr& writer, const Error& error) {
    const std::string slug = error_slug(error);
    hv::Json body;
    body["ok"] = false;
    body["error"] = slug;
    body["message"] = error.message;
    if (error.system_errno != 0) body["errno"] = error.system_errno;
    return http::write_json_to(writer, status_for_slug(slug, error.code), body);
}

} // namespace

// POST /api/extensions/runtime/backend/request
// Body: {"method","path","body"?,"contentType"?} -> 200 {"status","headers","body"}
//
// The session token is read only from X-EdgeDesk-Runtime. A "token" field in
// the JSON body is discarded; the validated session is the only identity
// source and the proxy derives the socket from it. Bodies and responses are
// never written to the audit log.
void register_backend_proxy_routes(hv::HttpService& router, RouteContext& ctx) {
    router.POST("/api/extensions/runtime/backend/request",
                [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                    if (ctx.backend_proxy == nullptr || ctx.extension_runtime == nullptr) {
                        write_code(writer, 503, "extensions_unavailable",
                                   "extension runtime is unavailable");
                        return;
                    }
                    const std::string token = req->GetHeader("X-EdgeDesk-Runtime", "");
                    if (token.empty()) {
                        write_code(writer, 401, "runtime_session_invalid",
                                   "missing runtime session token");
                        return;
                    }
                    extensions::RuntimeSession session;
                    const Error session_error = ctx.extension_runtime->validate(token, session);
                    if (!session_error.ok()) {
                        write_code(writer, 401, "runtime_session_invalid",
                                   "runtime session is invalid or expired");
                        return;
                    }
                    hv::Json params;
                    Error parse_error;
                    if (!http::parse_json_request(req.get(), params, parse_error)) {
                        write_proxy_error(writer, parse_error);
                        return;
                    }
                    // The header is authoritative. Ignoring the body field
                    // keeps a stray token from being interpreted anywhere.
                    params.erase("token");
                    const bool queued = ctx.backend_proxy->submit(
                        session, std::move(params),
                        [writer](Error error, extensions::BackendHttpResponse response) {
                            if (!error.ok()) {
                                write_proxy_error(writer, error);
                                return;
                            }
                            hv::Json body;
                            body["status"] = response.status;
                            body["headers"] = std::move(response.headers);
                            body["body"] = std::move(response.body);
                            http::write_json_to(writer, 200, body);
                        });
                    if (!queued) {
                        write_code(writer, 503, "backend_busy",
                                   "too many backend requests are pending; try again");
                    }
                });
}

} // namespace edgedesk
