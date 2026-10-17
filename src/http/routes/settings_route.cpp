#include "http/routes/settings_route.hpp"

#include "core/util.hpp"

namespace edgedesk {
namespace {

hv::Json settings_json(const AppSettings& settings) {
    hv::Json body;
    body["theme"] = settings.theme;
    body["files_view"] = settings.files_view;
    body["files_show_hidden"] = settings.files_show_hidden;
    body["terminal_font_size"] = settings.terminal_font_size;
    body["dashboard_metrics_window"] = settings.dashboard_metrics_window;
    return body;
}

bool read_string(const hv::Json& body, const char* key, std::string& out, Error& error) {
    if (!body.contains(key)) return true;
    if (!body[key].is_string()) {
        error = Error::make(ErrorCode::invalid_argument, std::string("field '") + key + "' must be a string");
        return false;
    }
    out = body[key].get<std::string>();
    return true;
}

bool read_bool(const hv::Json& body, const char* key, bool& out, Error& error) {
    if (!body.contains(key)) return true;
    if (!body[key].is_boolean()) {
        error = Error::make(ErrorCode::invalid_argument, std::string("field '") + key + "' must be a boolean");
        return false;
    }
    out = body[key].get<bool>();
    return true;
}

bool read_int(const hv::Json& body, const char* key, int& out, Error& error) {
    if (!body.contains(key)) return true;
    if (!body[key].is_number_integer()) {
        error = Error::make(ErrorCode::invalid_argument, std::string("field '") + key + "' must be an integer");
        return false;
    }
    out = body[key].get<int>();
    return true;
}

} // namespace

void register_settings_routes(hv::HttpService& router, RouteContext& ctx) {
    router.GET("/api/settings", [&ctx](HttpRequest*, HttpResponse* resp) -> int {
        hv::Json body;
        body["ok"] = true;
        body["settings"] = settings_json(ctx.settings->settings());
        body["api_token_configured"] = ctx.auth->token_configured();
        return http::write_json(resp, 200, body);
    });

    router.PUT("/api/settings", [&ctx](HttpRequest* req, HttpResponse* resp) -> int {
        hv::Json body;
        Error error;
        if (!http::parse_json_request(req, body, error)) return http::write_error(resp, error);
        AppSettings next = ctx.settings->settings();
        if (!read_string(body, "theme", next.theme, error) ||
            !read_string(body, "files_view", next.files_view, error) ||
            !read_bool(body, "files_show_hidden", next.files_show_hidden, error) ||
            !read_int(body, "terminal_font_size", next.terminal_font_size, error) ||
            !read_int(body, "dashboard_metrics_window", next.dashboard_metrics_window, error)) {
            return http::write_error(resp, error);
        }
        for (const auto& item : body.items()) {
            const auto& key = item.key();
            if (key != "theme" && key != "files_view" && key != "files_show_hidden" &&
                key != "terminal_font_size" && key != "dashboard_metrics_window") {
                return http::write_error(resp, 400, "invalid_request", "unknown settings field: " + key);
            }
        }
        std::string save_error;
        if (!ctx.settings->save(next, save_error)) {
            return http::write_error(resp, 400, "invalid_settings", save_error);
        }
        ctx.audit->record("settings.update", "ok", "", http::client_ip(*ctx.config, req));
        hv::Json result;
        result["ok"] = true;
        result["settings"] = settings_json(ctx.settings->settings());
        return http::write_json(resp, 200, result);
    });
}

} // namespace edgedesk
