#include "http/routes/supervisor_route.hpp"

#include "app/supervisor_service.hpp"

#include <functional>
#include <limits>
#include <string>
#include <utility>

namespace edgedesk {
namespace {

void write_result(const HttpResponseWriterPtr& writer, SupervisorResult result) {
    hv::Json body;
    if (result.status >= 200 && result.status < 300) {
        body = result.value.is_object() ? std::move(result.value) : hv::Json::object();
        body["ok"] = true;
    } else {
        body["ok"] = false;
        body["error"] = result.code.empty() ? "supervisor_error" : result.code;
        body["message"] = result.message;
    }
    http::write_json_to(writer, result.status, body);
}

void audit_result(RouteContext& ctx, const std::string& action, const std::string& detail,
                  const std::string& ip, const SupervisorResult& result) {
    if (result.status >= 200 && result.status < 300) {
        ctx.audit->record(action, "ok", detail, ip);
    } else {
        ctx.audit->record(action, "denied",
                          detail.empty() ? result.code : detail + " error=" + result.code, ip);
    }
}

// Dispatches one privileged operation to the bounded Supervisor IPC pool.
// Returns an HTTP status; a full queue is reported as 503 rather than blocking
// the libhv event loop.
int dispatch(RouteContext& ctx, const HttpResponseWriterPtr& writer, std::string method,
             nlohmann::json params, std::string action, std::string detail, std::string ip) {
    if (ctx.supervisor == nullptr) {
        SupervisorResult result;
        result.status = 503;
        result.code = "supervisor_unavailable";
        result.message = "process supervisor is not configured";
        write_result(writer, std::move(result));
        return 503;
    }
    const bool queued = ctx.supervisor->submit(
        std::move(method), std::move(params),
        [&ctx, writer, action = std::move(action), detail = std::move(detail),
         ip = std::move(ip)](SupervisorResult result) {
            audit_result(ctx, action, detail, ip, result);
            write_result(writer, std::move(result));
        });
    if (!queued) {
        ctx.audit->record("supervisor.queue", "denied", "worker queue is full", ip);
        SupervisorResult result;
        result.status = 503;
        result.code = "supervisor_busy";
        result.message = "too many supervisor operations are pending; try again";
        write_result(writer, std::move(result));
        return 503;
    }
    return 202;
}

// Read-only endpoints are not audited individually; they expose no arguments or
// secrets and would otherwise flood the audit log during UI polling.
void submit_read(RouteContext& ctx, const HttpResponseWriterPtr& writer, std::string method,
                 nlohmann::json params) {
    if (ctx.supervisor == nullptr) {
        SupervisorResult result;
        result.status = 503;
        result.code = "supervisor_unavailable";
        result.message = "process supervisor is not configured";
        write_result(writer, std::move(result));
        return;
    }
    const bool queued = ctx.supervisor->submit(
        std::move(method), std::move(params),
        [writer](SupervisorResult result) { write_result(writer, std::move(result)); });
    if (!queued) {
        SupervisorResult result;
        result.status = 503;
        result.code = "supervisor_busy";
        result.message = "too many supervisor operations are pending; try again";
        write_result(writer, std::move(result));
    }
}

std::string path_id(HttpRequest* req) {
    std::string id = req->GetParam("id");
    if (id.size() > 64) id.resize(64);
    return id;
}

std::string detail_for(const std::string& id) { return "service=" + id; }

} // namespace

void register_supervisor_routes(hv::HttpService& router, RouteContext& ctx) {
    router.GET("/api/supervisor/health",
               [&ctx](const HttpRequestPtr&, const HttpResponseWriterPtr& writer) {
                   submit_read(ctx, writer, "health", nlohmann::json::object());
               });

    router.GET("/api/supervisor/services",
               [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                   nlohmann::json params;
                   params["offset"] = http::query_int(req.get(), "offset", 0, 0, 1000000);
                   params["limit"] = http::query_int(req.get(), "limit", 200, 1, 1000);
                   const std::string status = http::query_string(req.get(), "status");
                   if (!status.empty()) params["status"] = status;
                   submit_read(ctx, writer, "service.list", std::move(params));
               });

    router.POST("/api/supervisor/services",
                [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                    hv::Json body;
                    Error parse_error;
                    if (!http::parse_json_request(req.get(), body, parse_error)) {
                        http::write_error_to(writer, parse_error);
                        return;
                    }
                    const std::string ip = http::client_ip(*ctx.config, req.get());
                    dispatch(ctx, writer, "service.create", std::move(body), "supervisor.create",
                             "", ip);
                });

    router.GET("/api/supervisor/services/:id",
               [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                   nlohmann::json params;
                   params["id"] = path_id(req.get());
                   submit_read(ctx, writer, "service.get", std::move(params));
               });

    router.PUT("/api/supervisor/services/:id",
               [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                   const std::string id = path_id(req.get());
                   hv::Json body;
                   Error parse_error;
                   if (!http::parse_json_request(req.get(), body, parse_error)) {
                       http::write_error_to(writer, parse_error);
                       return;
                   }
                   if (!body.is_object()) {
                       SupervisorResult result;
                       result.status = 400;
                       result.code = "invalid_request";
                       result.message = "service body must be a JSON object";
                       write_result(writer, std::move(result));
                       return;
                   }
                   body["id"] = id;
                   const std::string ip = http::client_ip(*ctx.config, req.get());
                   dispatch(ctx, writer, "service.update", std::move(body), "supervisor.update",
                            detail_for(id), ip);
               });

    router.Delete("/api/supervisor/services/:id",
                  [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                      const std::string id = path_id(req.get());
                      nlohmann::json params;
                      params["id"] = id;
                      const std::string ip = http::client_ip(*ctx.config, req.get());
                      dispatch(ctx, writer, "service.delete", std::move(params),
                               "supervisor.delete", detail_for(id), ip);
                  });

    router.POST("/api/supervisor/services/:id/start",
                [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                    const std::string id = path_id(req.get());
                    nlohmann::json params;
                    params["id"] = id;
                    const std::string ip = http::client_ip(*ctx.config, req.get());
                    dispatch(ctx, writer, "service.start", std::move(params),
                             "supervisor.start", detail_for(id), ip);
                });

    router.POST("/api/supervisor/services/:id/stop",
                [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                    const std::string id = path_id(req.get());
                    nlohmann::json params;
                    params["id"] = id;
                    const std::string ip = http::client_ip(*ctx.config, req.get());
                    dispatch(ctx, writer, "service.stop", std::move(params), "supervisor.stop",
                             detail_for(id), ip);
                });

    router.POST("/api/supervisor/services/:id/restart",
                [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                    const std::string id = path_id(req.get());
                    nlohmann::json params;
                    params["id"] = id;
                    const std::string ip = http::client_ip(*ctx.config, req.get());
                    dispatch(ctx, writer, "service.restart", std::move(params),
                             "supervisor.restart", detail_for(id), ip);
                });

    router.GET("/api/supervisor/services/:id/logs",
               [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                   const std::string id = path_id(req.get());
                   nlohmann::json params;
                   params["id"] = id;
                   if (!http::query_string(req.get(), "tail_bytes").empty()) {
                       params["tail_bytes"] =
                           http::query_int(req.get(), "tail_bytes", 0, 0, 256 * 1024);
                   }
                   if (!http::query_string(req.get(), "cursor").empty()) {
                       params["cursor"] =
                           http::query_int(req.get(), "cursor", 0, 0,
                                           std::numeric_limits<std::int64_t>::max());
                   }
                   if (!http::query_string(req.get(), "max_bytes").empty()) {
                       params["max_bytes"] =
                           http::query_int(req.get(), "max_bytes", 0, 0, 256 * 1024);
                   }
                   const std::string ip = http::client_ip(*ctx.config, req.get());
                   dispatch(ctx, writer, "service.logs", std::move(params), "supervisor.logs",
                            detail_for(id), ip);
               });
}

} // namespace edgedesk
