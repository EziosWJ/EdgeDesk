#pragma once

#include "core/config.hpp"
#include "file/file_service.hpp"
#include "http/http_util.hpp"
#include "security/audit.hpp"
#include "security/auth.hpp"
#include "security/rate_limiter.hpp"
#include "security/settings_store.hpp"
#include "system/system_metrics.hpp"
#include "terminal/terminal_manager.hpp"
#include "web/web_root.hpp"

#include "HttpServer.h"

#include <string>
#include <string_view>

// RouteContext is the explicit dependency bundle injected into every route.
// No route reads global state; services are constructed once in Application.

namespace edgedesk {

namespace extensions {
class ExtensionRuntime;
class PackageManager;
} // namespace extensions

struct RouteContext {
    const Config* config = nullptr;
    WebRoot* web_root = nullptr;
    FileService* files = nullptr;
    TerminalManager* terminals = nullptr;
    SystemMetrics* metrics = nullptr;
    Auth* auth = nullptr;
    SettingsStore* settings = nullptr;
    AuditLog* audit = nullptr;
    RateLimiter* api_limiter = nullptr;
    RateLimiter* login_limiter = nullptr;
    RateLimiter* setup_limiter = nullptr;
    ConcurrencyGate* transfer_gate = nullptr;
    extensions::PackageManager* packages = nullptr;
    extensions::ExtensionRuntime* extension_runtime = nullptr;
};

// Public endpoints depend on the authentication state: health/static/session
// are always public; setup is public only while uninitialized; login is public
// only after initialization.
bool path_is_public(const RouteContext& ctx, std::string_view path);

// Installs security headers, rate limiting, auth + CSRF and body-size limits.
void install_middleware(hv::HttpService& router, RouteContext& ctx);

// Body limit for a path (upload/text/JSON).
std::uint64_t body_limit_for(const Config& config, std::string_view path);

// True for the streaming upload endpoint (validated at the header stage).
bool is_upload_path(std::string_view path);

// Raw ZIP install/upgrade endpoints streamed without a fixed size limit.
bool is_extension_upload_path(std::string_view path);

// True for both streaming families; these skip the buffered-body middleware.
bool is_streaming_path(std::string_view path);

// Shared helper for the WebSocket upgrade path and the upload route.
bool request_is_authenticated(RouteContext& ctx, HttpRequest* req, bool& used_cookie);

// Origin/Referer validation against the Host header.
bool request_origin_ok(HttpRequest* req);

} // namespace edgedesk
