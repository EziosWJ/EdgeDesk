#include "http/routes/native_backend_route.hpp"

#include "app/supervisor_service.hpp"
#include "extensions/extension_store.hpp"
#include "extensions/native_backend_installer.hpp"
#include "extensions/native_backend_lifecycle.hpp"
#include "extensions/package_manager.hpp"
#include "http/http_util.hpp"

#include <functional>
#include <string>
#include <utility>

// Administrator lifecycle endpoints for optional native backends
// (SPEC-0011 section 6.2 and the shared API contract in
// docs/tasks/0011-workstreams.md).
//
// Local validation, path preparation and persistence happen synchronously on
// the handler thread. Every Supervisor call is submitted to the bounded
// SupervisorService pool and the HTTP response is written from its completion
// callback, so the libhv event loop never waits on IPC or a slow process.
namespace edgedesk {
namespace {

using extensions::AppRecord;
using extensions::BackendRecord;
using extensions::ExtensionStore;
using extensions::NativeBackendLifecycle;
using extensions::PackageManager;

int write_error_slug(const HttpResponseWriterPtr& writer, int status, const std::string& slug,
                     const std::string& message) {
    nlohmann::json body;
    body["ok"] = false;
    body["error"] = slug;
    body["message"] = message;
    return http::write_json_to(writer, status, body);
}

int write_lifecycle_error(const HttpResponseWriterPtr& writer, const Error& error) {
    const ErrorCode code = error.code == ErrorCode::ok ? ErrorCode::io_error : error.code;
    return write_error_slug(writer, http_status_for(code),
                            extensions::backend_error_slug(error, error_code_slug(code)),
                            extensions::backend_error_message(error));
}

void audit(RouteContext& ctx, const char* event, bool ok, const std::string& detail,
           const std::string& ip, const std::string& app_id) {
    if (ctx.audit == nullptr) return;
    ctx.audit->record(event, ok ? "ok" : "denied", detail, ip, app_id);
}

bool backend_ready(RouteContext& ctx, const HttpResponseWriterPtr& writer) {
    if (ctx.native_backends != nullptr && ctx.packages != nullptr) return true;
    write_error_slug(writer, 503, "native_backend_unavailable",
                     "the native application backend platform is unavailable; configure a "
                     "writable --apps-root");
    return false;
}

bool supervisor_ready(RouteContext& ctx, const HttpResponseWriterPtr& writer) {
    if (ctx.supervisor != nullptr && ctx.supervisor->configured()) return true;
    write_error_slug(writer, 503, "supervisor_unavailable",
                     "the process supervisor is not configured; native backends cannot be "
                     "started or stopped");
    return false;
}

bool path_app_id(HttpRequest* req, const HttpResponseWriterPtr& writer, std::string& app_id) {
    app_id = req->GetParam("app_id");
    std::string error;
    if (!extensions::valid_app_id(app_id, error)) {
        write_error_slug(writer, 400, "invalid_argument", "invalid application id");
        return false;
    }
    return true;
}

bool load_app(RouteContext& ctx, const HttpResponseWriterPtr& writer, const std::string& app_id,
              bool require_enabled, AppRecord& app) {
    bool found = false;
    Error error = ctx.packages->get_app(app_id, app, found);
    if (!error.ok()) {
        http::write_error_to(writer, error);
        return false;
    }
    if (!found || app.status == extensions::app_status::uninstalled) {
        write_error_slug(writer, 404, "app_not_found", "application not found");
        return false;
    }
    if (require_enabled && app.status != extensions::app_status::enabled) {
        write_error_slug(writer, 409, "backend_app_disabled",
                         "enable the application before starting its backend");
        return false;
    }
    return true;
}

bool load_backend(RouteContext& ctx, const HttpResponseWriterPtr& writer,
                  const std::string& app_id, BackendRecord& record) {
    bool found = false;
    Error error = ctx.packages->store().get_backend(app_id, record, found);
    if (!error.ok()) {
        http::write_error_to(writer, error);
        return false;
    }
    if (!found) {
        write_error_slug(writer, 409, "backend_not_configured",
                         "this application has no native backend");
        return false;
    }
    if (record.service_id.empty()) record.service_id = extensions::backend_service_id(app_id);
    return true;
}

struct SupervisorFailure {
    int status = 502;
    std::string slug = "supervisor_error";
    std::string message;
};

SupervisorFailure map_supervisor_failure(const SupervisorResult& result) {
    SupervisorFailure failure;
    failure.status = result.status >= 400 ? result.status : 502;
    failure.message =
        result.message.empty() ? "the process supervisor request failed" : result.message;
    const std::string& code = result.code;
    if (result.status == 503 || code == "supervisor_unavailable") {
        failure.status = 503;
        failure.slug = "supervisor_unavailable";
    } else if (code == "service_not_found" || result.status == 404) {
        failure.status = 409;
        failure.slug = "backend_not_configured";
    } else if (code == "service_running") {
        failure.status = 409;
        failure.slug = "backend_running";
    } else if (code == "service_stopping") {
        failure.status = 409;
        failure.slug = "backend_stopping";
    } else if (code == "service_unknown") {
        failure.status = 409;
        failure.slug = "backend_unknown";
    } else if (result.status == 403) {
        failure.status = 403;
        failure.slug = "backend_permission_denied";
    } else if (result.status == 400) {
        failure.status = 400;
        failure.slug = "invalid_service_config";
    } else if (!code.empty()) {
        failure.slug = code;
    }
    return failure;
}

int write_supervisor_failure(const HttpResponseWriterPtr& writer, const SupervisorResult& result,
                             const std::string& prefix = {}) {
    const SupervisorFailure failure = map_supervisor_failure(result);
    return write_error_slug(writer, failure.status, failure.slug,
                            prefix.empty() ? failure.message : prefix + failure.message);
}

nlohmann::json supervisor_snapshot(const nlohmann::json& service) {
    if (!service.is_object()) return nullptr;
    nlohmann::json snapshot;
    snapshot["state"] = service.value("state", "UNKNOWN");
    snapshot["pid"] = service.contains("pid") ? service["pid"] : nlohmann::json(nullptr);
    snapshot["last_exit_code"] =
        service.contains("last_exit_code") ? service["last_exit_code"] : nlohmann::json(nullptr);
    if (service.contains("last_error") && service["last_error"].is_string()) {
        snapshot["last_error"] = service["last_error"];
    }
    return snapshot;
}

bool write_backend(RouteContext& ctx, const HttpResponseWriterPtr& writer,
                   const std::string& app_id, int status, bool with_ok,
                   const nlohmann::json* supervisor,
                   const nlohmann::json& extra = nlohmann::json::object()) {
    nlohmann::json backend;
    Error error = ctx.native_backends->describe(app_id, backend);
    if (!error.ok()) {
        write_lifecycle_error(writer, error);
        return false;
    }
    if (supervisor != nullptr) backend["supervisor"] = *supervisor;
    nlohmann::json body;
    if (with_ok) body["ok"] = true;
    for (const auto& item : extra.items()) body[item.key()] = item.value();
    body["backend"] = std::move(backend);
    http::write_json_to(writer, status, body);
    return true;
}

bool submit_or_busy(RouteContext& ctx, const HttpResponseWriterPtr& writer, std::string method,
                    nlohmann::json params, SupervisorService::Callback callback) {
    if (ctx.supervisor != nullptr &&
        ctx.supervisor->submit(std::move(method), std::move(params), std::move(callback))) {
        return true;
    }
    write_error_slug(writer, 503, "supervisor_busy",
                     "too many supervisor operations are pending; try again");
    return false;
}

const char* start_event = "extension.backend.start";

// Persists the Supervisor-generated service id. The Supervisor assigns ids on
// create; the extension platform stores that stable id so every later call uses
// the same mapping (SPEC-0011 section 6.1).
bool persist_service_id(RouteContext& ctx, const HttpResponseWriterPtr& writer,
                        const std::string& app_id, const std::string& service_id) {
    BackendRecord record;
    bool found = false;
    Error error = ctx.packages->store().get_backend(app_id, record, found);
    if (!error.ok() || !found) {
        write_error_slug(writer, 500, "backend_not_configured",
                         "the backend record disappeared while registering its service");
        return false;
    }
    record.service_id = service_id;
    error = ctx.packages->store().upsert_backend(record);
    if (!error.ok()) {
        write_lifecycle_error(writer, error);
        return false;
    }
    return true;
}

void complete_start(RouteContext& ctx, const HttpResponseWriterPtr& writer,
                    const std::string& app_id, const std::string& ip,
                    SupervisorResult result) {
    if (result.status < 200 || result.status >= 300) {
        audit(ctx, start_event, false,
              result.code + (result.message.empty() ? "" : " " + result.message), ip, app_id);
        write_supervisor_failure(writer, result);
        return;
    }
    nlohmann::json extra = nlohmann::json::object();
    const Error persisted =
        ctx.native_backends->set_state(app_id, extensions::backend_state::stopped, "");
    if (!persisted.ok()) {
        extra["warning"] =
            "the backend process was started but its persisted state could not be updated: " +
            persisted.message;
    }
    audit(ctx, start_event, true, "state=started", ip, app_id);
    nlohmann::json snapshot = supervisor_snapshot(result.value);
    write_backend(ctx, writer, app_id, 200, true, &snapshot, extra);
}

void submit_start(RouteContext& ctx, const HttpResponseWriterPtr& writer,
                  const std::string& app_id, const std::string& service_id,
                  const std::string& ip) {
    submit_or_busy(ctx, writer, "service.start", {{"id", service_id}},
                   [&ctx, writer, app_id, ip](SupervisorResult result) {
                       complete_start(ctx, writer, app_id, ip, std::move(result));
                   });
}

// Creates the Supervisor service without starting it. Used when an
// administrator saves an identity or enables autostart for a backend that has
// never been started: the service must exist for autostart to work at boot,
// while installation itself still never starts anything.
void submit_create(RouteContext& ctx, const HttpResponseWriterPtr& writer,
                   const std::string& app_id, const std::string& ip,
                   const std::string& event, const std::string& detail) {
    BackendRecord record;
    bool found = false;
    Error get_error = ctx.packages->store().get_backend(app_id, record, found);
    if (!get_error.ok()) {
        write_lifecycle_error(writer, get_error);
        return;
    }
    if (!found) {
        write_error_slug(writer, 409, "backend_not_configured",
                         "this application has no native backend");
        return;
    }
    if (record.run_user.empty() || record.run_group.empty()) {
        write_error_slug(writer, 400, "backend_identity_required",
                         "configure run_user and run_group before enabling a Supervisor "
                         "service");
        return;
    }
    std::string socket_path;
    const Error prepared = ctx.native_backends->prepare_runtime(app_id, socket_path);
    if (!prepared.ok()) {
        audit(ctx, event.c_str(), false, prepared.message, ip, app_id);
        write_lifecycle_error(writer, prepared);
        return;
    }
    nlohmann::json params;
    const Error configured = ctx.native_backends->service_params(app_id, params);
    if (!configured.ok()) {
        audit(ctx, event.c_str(), false, configured.message, ip, app_id);
        write_lifecycle_error(writer, configured);
        return;
    }
    // The Supervisor assigns the id on create; the placeholder in the params is
    // only meaningful for updates.
    params.erase("id");
    submit_or_busy(
        ctx, writer, "service.create", std::move(params),
        [&ctx, writer, app_id, ip, event, detail](SupervisorResult created) {
            if (created.status < 200 || created.status >= 300) {
                audit(ctx, event.c_str(), false, created.code, ip, app_id);
                write_supervisor_failure(writer, created);
                return;
            }
            const std::string created_id = created.value.value("id", std::string());
            if (created_id.empty() ||
                !persist_service_id(ctx, writer, app_id, created_id)) {
                if (created_id.empty()) {
                    audit(ctx, event.c_str(), false, "service.create returned no id", ip, app_id);
                    write_error_slug(writer, 502, "supervisor_error",
                                     "the Supervisor created a service without an id");
                }
                return;
            }
            audit(ctx, event.c_str(), true, detail, ip, app_id);
            nlohmann::json snapshot = supervisor_snapshot(created.value);
            write_backend(ctx, writer, app_id, 200, true, &snapshot);
        });
}

// Runs after the Supervisor confirmed there is no live process. The local
// run-directory preparation happens here, on a Supervisor worker thread, not
// on the libhv event loop.
void prepare_and_start(RouteContext& ctx, const HttpResponseWriterPtr& writer,
                       const std::string& app_id, const std::string& service_id,
                       const std::string& ip, bool service_exists) {
    std::string socket_path;
    const Error prepared = ctx.native_backends->prepare_runtime(app_id, socket_path);
    if (!prepared.ok()) {
        audit(ctx, start_event, false, prepared.message, ip, app_id);
        write_lifecycle_error(writer, prepared);
        return;
    }
    // Always regenerate the service configuration from the current package and
    // administrator settings. An upgrade can change args or the display name;
    // a stale Supervisor definition must never be started silently.
    nlohmann::json params;
    const Error configured = ctx.native_backends->service_params(app_id, params);
    if (!configured.ok()) {
        audit(ctx, start_event, false, configured.message, ip, app_id);
        write_lifecycle_error(writer, configured);
        return;
    }
    if (service_exists) {
        submit_or_busy(ctx, writer, "service.update", std::move(params),
                       [&ctx, writer, app_id, service_id, ip](SupervisorResult updated) {
                           if (updated.status < 200 || updated.status >= 300) {
                               audit(ctx, start_event, false, updated.code, ip, app_id);
                               write_supervisor_failure(writer, updated,
                                                        "cannot apply the current backend "
                                                        "configuration: ");
                               return;
                           }
                           submit_start(ctx, writer, app_id, service_id, ip);
                       });
        return;
    }
    // service.create must not receive "id": the Supervisor assigns it. The
    // placeholder id is only used by service.update.
    params.erase("id");
    submit_or_busy(ctx, writer, "service.create", std::move(params),
                   [&ctx, writer, app_id, ip](SupervisorResult created) {
                       if (created.status < 200 || created.status >= 300) {
                           audit(ctx, start_event, false, created.code, ip, app_id);
                           write_supervisor_failure(writer, created);
                           return;
                       }
                       const std::string created_id = created.value.value("id", std::string());
                       if (created_id.empty()) {
                           audit(ctx, start_event, false,
                                 "service.create returned no id", ip, app_id);
                           write_error_slug(writer, 502, "supervisor_error",
                                            "the Supervisor created a service without an id");
                           return;
                       }
                       if (!persist_service_id(ctx, writer, app_id, created_id)) return;
                       submit_start(ctx, writer, app_id, created_id, ip);
                   });
}

// Re-reads the service after stop/restart (both return an operation, not the
// service record) so the response can carry live state. Failure to read the
// state does not turn an accepted lifecycle operation into an error.
void write_after_operation(RouteContext& ctx, const HttpResponseWriterPtr& writer,
                           const std::string& app_id, const std::string& service_id,
                           bool with_ok) {
    const bool queued = ctx.supervisor->submit(
        "service.get", {{"id", service_id}},
        [&ctx, writer, app_id, with_ok](SupervisorResult result) {
            if (result.status >= 200 && result.status < 300) {
                nlohmann::json snapshot = supervisor_snapshot(result.value);
                write_backend(ctx, writer, app_id, 200, with_ok, &snapshot);
            } else {
                write_backend(ctx, writer, app_id, 200, with_ok, nullptr);
            }
        });
    if (!queued) {
        // The management operation itself already succeeded; report it without
        // a live snapshot rather than failing a completed action.
        write_backend(ctx, writer, app_id, 200, with_ok, nullptr);
    }
}

} // namespace

void register_native_backend_routes(hv::HttpService& router, RouteContext& ctx) {
    router.GET("/api/extensions/:app_id/backend",
               [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                   if (!backend_ready(ctx, writer)) return;
                   std::string app_id;
                   if (!path_app_id(req.get(), writer, app_id)) return;

                   nlohmann::json backend;
                   Error error = ctx.native_backends->describe(app_id, backend);
                   if (!error.ok()) {
                       write_lifecycle_error(writer, error);
                       return;
                   }
                   const bool present = backend.value("present", false);
                   if (!present || ctx.supervisor == nullptr || !ctx.supervisor->configured()) {
                       if (present) backend["supervisor"] = nullptr;
                       nlohmann::json body;
                       body["backend"] = std::move(backend);
                       http::write_json_to(writer, 200, body);
                       return;
                   }

                   const std::string service_id = backend.value("service_id", std::string());
                   const bool queued = ctx.supervisor->submit(
                       "service.get", {{"id", service_id}},
                       [writer, backend = std::move(backend)](SupervisorResult result) mutable {
                           if (result.status >= 200 && result.status < 300) {
                               backend["supervisor"] = supervisor_snapshot(result.value);
                           } else {
                               // Read-only status: an unavailable or missing live
                               // state never fails the configuration view.
                               backend["supervisor"] = nullptr;
                           }
                           nlohmann::json body;
                           body["backend"] = std::move(backend);
                           http::write_json_to(writer, 200, body);
                       });
                   if (!queued) {
                       write_error_slug(writer, 503, "supervisor_busy",
                                        "too many supervisor operations are pending; try again");
                   }
               });

    router.PUT("/api/extensions/:app_id/backend/identity",
               [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                   if (!backend_ready(ctx, writer)) return;
                   std::string app_id;
                   if (!path_app_id(req.get(), writer, app_id)) return;
                   const std::string ip = http::client_ip(*ctx.config, req.get());

                   nlohmann::json body;
                   Error parse_error;
                   if (!http::parse_json_request(req.get(), body, parse_error)) {
                       http::write_error_to(writer, parse_error);
                       return;
                   }
                   if (!body.contains("run_user") || !body["run_user"].is_string() ||
                       !body.contains("run_group") || !body["run_group"].is_string()) {
                       audit(ctx, "extension.backend.identity", false,
                             "run_user/run_group missing or not strings", ip, app_id);
                       write_error_slug(writer, 400, "invalid_argument",
                                        "fields 'run_user' and 'run_group' must be strings");
                       return;
                   }
                   bool allow_root = false;
                   if (body.contains("allow_root")) {
                       if (!body["allow_root"].is_boolean()) {
                           audit(ctx, "extension.backend.identity", false,
                                 "allow_root is not a boolean", ip, app_id);
                           write_error_slug(writer, 400, "invalid_argument",
                                            "field 'allow_root' must be a boolean");
                           return;
                       }
                       allow_root = body["allow_root"].get<bool>();
                   }
                   const std::string run_user = body["run_user"].get<std::string>();
                   const std::string run_group = body["run_group"].get<std::string>();
                   const std::string detail = "run_user=" + run_user + " run_group=" + run_group +
                                              " allow_root=" + (allow_root ? "1" : "0");

                   const Error error =
                       ctx.native_backends->set_identity(app_id, run_user, run_group, allow_root);
                   if (!error.ok()) {
                       audit(ctx, "extension.backend.identity", false, error.message, ip, app_id);
                       write_lifecycle_error(writer, error);
                       return;
                   }

                   BackendRecord record;
                   if (!load_backend(ctx, writer, app_id, record)) {
                       // set_identity already succeeded; a vanished row is a race.
                       return;
                   }
                   if (ctx.supervisor == nullptr || !ctx.supervisor->configured()) {
                       audit(ctx, "extension.backend.identity", true, detail, ip, app_id);
                       write_backend(ctx, writer, app_id, 200, true, nullptr);
                       return;
                   }

                   const std::string service_id = record.service_id;
                   submit_or_busy(
                       ctx, writer, "service.get", {{"id", service_id}},
                       [&ctx, writer, app_id, service_id, ip,
                        detail](SupervisorResult fetched) {
                           if (fetched.status == 404 || fetched.code == "service_not_found") {
                               // Create the service now (without starting it) so
                               // autostart and a later start use the saved identity.
                               submit_create(ctx, writer, app_id, ip,
                                             "extension.backend.identity", detail);
                               return;
                           }
                           if (fetched.status < 200 || fetched.status >= 300) {
                               audit(ctx, "extension.backend.identity", false, fetched.code, ip,
                                     app_id);
                               write_supervisor_failure(
                                   writer, fetched,
                                   "identity was saved locally, but the Supervisor service "
                                   "could not be inspected: ");
                               return;
                           }
                           nlohmann::json params;
                           const Error configured =
                               ctx.native_backends->service_params(app_id, params);
                           if (!configured.ok()) {
                               audit(ctx, "extension.backend.identity", false,
                                     configured.message, ip, app_id);
                               write_lifecycle_error(writer, configured);
                               return;
                           }
                           params["id"] = service_id;
                           submit_or_busy(
                               ctx, writer, "service.update", std::move(params),
                               [&ctx, writer, app_id, ip,
                                detail](SupervisorResult updated) {
                                   if (updated.status < 200 || updated.status >= 300) {
                                       audit(ctx, "extension.backend.identity", false,
                                             updated.code, ip, app_id);
                                       write_supervisor_failure(
                                           writer, updated,
                                           "identity was saved locally, but the Supervisor "
                                           "service was not updated: ");
                                       return;
                                   }
                                   audit(ctx, "extension.backend.identity", true, detail, ip,
                                         app_id);
                                   nlohmann::json snapshot = supervisor_snapshot(updated.value);
                                   write_backend(ctx, writer, app_id, 200, true, &snapshot);
                               });
                       });
               });

    router.PUT("/api/extensions/:app_id/backend/autostart",
               [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                   if (!backend_ready(ctx, writer)) return;
                   std::string app_id;
                   if (!path_app_id(req.get(), writer, app_id)) return;
                   const std::string ip = http::client_ip(*ctx.config, req.get());

                   nlohmann::json body;
                   Error parse_error;
                   if (!http::parse_json_request(req.get(), body, parse_error)) {
                       http::write_error_to(writer, parse_error);
                       return;
                   }
                   if (!body.contains("enabled") || !body["enabled"].is_boolean()) {
                       audit(ctx, "extension.backend.autostart", false,
                             "enabled is missing or not a boolean", ip, app_id);
                       write_error_slug(writer, 400, "invalid_argument",
                                        "field 'enabled' must be a boolean");
                       return;
                   }
                   const bool enabled = body["enabled"].get<bool>();
                   const std::string detail = std::string("enabled=") + (enabled ? "1" : "0");

                   const Error error = ctx.native_backends->set_autostart(app_id, enabled);
                   if (!error.ok()) {
                       audit(ctx, "extension.backend.autostart", false, error.message, ip, app_id);
                       write_lifecycle_error(writer, error);
                       return;
                   }

                   BackendRecord record;
                   if (!load_backend(ctx, writer, app_id, record)) return;
                   if (ctx.supervisor == nullptr || !ctx.supervisor->configured()) {
                       audit(ctx, "extension.backend.autostart", true, detail, ip, app_id);
                       write_backend(ctx, writer, app_id, 200, true, nullptr);
                       return;
                   }

                   // Autostart is persisted first; the Supervisor update is a
                   // partial update so the generated executable/args/identity
                   // can never be overwritten through this endpoint.
                   const std::string service_id = record.service_id;
                   submit_or_busy(
                       ctx, writer, "service.get", {{"id", service_id}},
                       [&ctx, writer, app_id, service_id, enabled, ip,
                        detail](SupervisorResult fetched) {
                           if (fetched.status == 404 || fetched.code == "service_not_found") {
                               if (!enabled) {
                                   audit(ctx, "extension.backend.autostart", true, detail, ip,
                                         app_id);
                                   write_backend(ctx, writer, app_id, 200, true, nullptr);
                                   return;
                               }
                               // Autostart is independent from a manual start:
                               // create the service (stopped) so the Supervisor
                               // can start it on the next boot.
                               submit_create(ctx, writer, app_id, ip,
                                             "extension.backend.autostart", detail);
                               return;
                           }
                           if (fetched.status < 200 || fetched.status >= 300) {
                               audit(ctx, "extension.backend.autostart", false, fetched.code, ip,
                                     app_id);
                               write_supervisor_failure(
                                   writer, fetched,
                                   "autostart was saved locally, but the Supervisor service "
                                   "could not be inspected: ");
                               return;
                           }
                           nlohmann::json params;
                           params["id"] = service_id;
                           params["autostart"] = enabled;
                           submit_or_busy(
                               ctx, writer, "service.update", std::move(params),
                               [&ctx, writer, app_id, ip,
                                detail](SupervisorResult updated) {
                                   if (updated.status < 200 || updated.status >= 300) {
                                       audit(ctx, "extension.backend.autostart", false,
                                             updated.code, ip, app_id);
                                       write_supervisor_failure(
                                           writer, updated,
                                           "autostart was saved locally, but the Supervisor "
                                           "service was not updated: ");
                                       return;
                                   }
                                   audit(ctx, "extension.backend.autostart", true, detail, ip,
                                         app_id);
                                   nlohmann::json snapshot = supervisor_snapshot(updated.value);
                                   write_backend(ctx, writer, app_id, 200, true, &snapshot);
                               });
                       });
               });

    router.POST("/api/extensions/:app_id/backend/start",
                [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                    if (!backend_ready(ctx, writer)) return;
                    std::string app_id;
                    if (!path_app_id(req.get(), writer, app_id)) return;
                    const std::string ip = http::client_ip(*ctx.config, req.get());

                    AppRecord app;
                    if (!load_app(ctx, writer, app_id, true, app)) return;
                    BackendRecord record;
                    if (!load_backend(ctx, writer, app_id, record)) return;
                    if (record.run_user.empty() || record.run_group.empty()) {
                        audit(ctx, start_event, false, "identity is not configured", ip, app_id);
                        write_error_slug(writer, 400, "backend_identity_required",
                                         "configure run_user and run_group before starting the "
                                         "backend");
                        return;
                    }
                    if (!supervisor_ready(ctx, writer)) {
                        audit(ctx, start_event, false, "supervisor is not configured", ip, app_id);
                        return;
                    }

                    const std::string service_id = record.service_id;
                    const bool queued = ctx.supervisor->submit(
                        "service.get", {{"id", service_id}},
                        [&ctx, writer, app_id, service_id, ip](SupervisorResult fetched) {
                            if (fetched.status >= 200 && fetched.status < 300) {
                                const std::string state =
                                    fetched.value.value("state", std::string("UNKNOWN"));
                                if (state == "RUNNING" || state == "STARTING") {
                                    audit(ctx, start_event, false, "service already running", ip,
                                          app_id);
                                    write_error_slug(writer, 409, "backend_running",
                                                     "the backend is already running");
                                    return;
                                }
                                if (state == "STOPPING") {
                                    audit(ctx, start_event, false, "service is stopping", ip,
                                          app_id);
                                    write_error_slug(writer, 409, "backend_stopping",
                                                     "the backend is stopping; try again after it "
                                                     "has stopped");
                                    return;
                                }
                                if (state == "UNKNOWN") {
                                    audit(ctx, start_event, false,
                                          "service process identity is unknown", ip, app_id);
                                    write_error_slug(writer, 409, "backend_unknown",
                                                     "the Supervisor cannot verify whether a "
                                                     "previous backend process is still alive; "
                                                     "refusing to start a duplicate");
                                    return;
                                }
                                // Stopped/failed/backoff: no live process, so the stale
                                // socket may be removed.
                                prepare_and_start(ctx, writer, app_id, service_id, ip, true);
                                return;
                            }
                            if (fetched.status == 404 || fetched.code == "service_not_found") {
                                prepare_and_start(ctx, writer, app_id, service_id, ip, false);
                                return;
                            }
                            audit(ctx, start_event, false, fetched.code, ip, app_id);
                            write_supervisor_failure(writer, fetched);
                        });
                    if (!queued) {
                        audit(ctx, start_event, false, "supervisor queue is full", ip, app_id);
                        write_error_slug(writer, 503, "supervisor_busy",
                                         "too many supervisor operations are pending; try again");
                    }
                });

    router.POST("/api/extensions/:app_id/backend/stop",
                [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                    if (!backend_ready(ctx, writer)) return;
                    std::string app_id;
                    if (!path_app_id(req.get(), writer, app_id)) return;
                    const std::string ip = http::client_ip(*ctx.config, req.get());

                    AppRecord app;
                    if (!load_app(ctx, writer, app_id, false, app)) return;
                    BackendRecord record;
                    if (!load_backend(ctx, writer, app_id, record)) return;
                    if (!supervisor_ready(ctx, writer)) return;

                    const std::string service_id = record.service_id;
                    submit_or_busy(
                        ctx, writer, "service.get", {{"id", service_id}},
                        [&ctx, writer, app_id, service_id, ip](SupervisorResult fetched) {
                            if (fetched.status == 404 || fetched.code == "service_not_found") {
                                audit(ctx, "extension.backend.stop", false,
                                      "no Supervisor service exists", ip, app_id);
                                write_error_slug(writer, 409, "backend_not_configured",
                                                 "the process supervisor has no service for this "
                                                 "application");
                                return;
                            }
                            if (fetched.status < 200 || fetched.status >= 300) {
                                audit(ctx, "extension.backend.stop", false, fetched.code, ip,
                                      app_id);
                                write_supervisor_failure(writer, fetched);
                                return;
                            }
                            submit_or_busy(
                                ctx, writer, "service.stop", {{"id", service_id}},
                                [&ctx, writer, app_id, service_id, ip](SupervisorResult stopped) {
                                    if (stopped.status < 200 || stopped.status >= 300) {
                                        audit(ctx, "extension.backend.stop", false, stopped.code,
                                              ip, app_id);
                                        write_supervisor_failure(writer, stopped);
                                        return;
                                    }
                                    const Error persisted = ctx.native_backends->set_state(
                                        app_id, extensions::backend_state::stopped, "");
                                    nlohmann::json extra = nlohmann::json::object();
                                    if (!persisted.ok()) {
                                        extra["warning"] =
                                            "the backend stop was accepted but its persisted "
                                            "state could not be updated: " +
                                            persisted.message;
                                    }
                                    audit(ctx, "extension.backend.stop", true, "state=stopped",
                                          ip, app_id);
                                    const bool queued = ctx.supervisor->submit(
                                        "service.get", {{"id", service_id}},
                                        [&ctx, writer, app_id,
                                         extra](SupervisorResult current) mutable {
                                            if (current.status >= 200 &&
                                                current.status < 300) {
                                                nlohmann::json snapshot =
                                                    supervisor_snapshot(current.value);
                                                write_backend(ctx, writer, app_id, 200, true,
                                                              &snapshot, extra);
                                            } else {
                                                write_backend(ctx, writer, app_id, 200, true,
                                                              nullptr, extra);
                                            }
                                        });
                                    if (!queued) {
                                        write_backend(ctx, writer, app_id, 200, true, nullptr,
                                                      extra);
                                    }
                                });
                        });
                });

    router.POST("/api/extensions/:app_id/backend/restart",
                [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                    if (!backend_ready(ctx, writer)) return;
                    std::string app_id;
                    if (!path_app_id(req.get(), writer, app_id)) return;
                    const std::string ip = http::client_ip(*ctx.config, req.get());

                    AppRecord app;
                    if (!load_app(ctx, writer, app_id, false, app)) return;
                    BackendRecord record;
                    if (!load_backend(ctx, writer, app_id, record)) return;
                    if (!supervisor_ready(ctx, writer)) return;

                    const std::string service_id = record.service_id;
                    submit_or_busy(
                        ctx, writer, "service.get", {{"id", service_id}},
                        [&ctx, writer, app_id, service_id, ip](SupervisorResult fetched) {
                            if (fetched.status == 404 || fetched.code == "service_not_found") {
                                audit(ctx, "extension.backend.restart", false,
                                      "no Supervisor service exists", ip, app_id);
                                write_error_slug(writer, 409, "backend_not_configured",
                                                 "the process supervisor has no service for this "
                                                 "application; start it first");
                                return;
                            }
                            if (fetched.status < 200 || fetched.status >= 300) {
                                audit(ctx, "extension.backend.restart", false, fetched.code, ip,
                                      app_id);
                                write_supervisor_failure(writer, fetched);
                                return;
                            }
                            // Restarting a stopped/failed/backoff service is a fresh
                            // start: prepare the run directory and drop an abandoned
                            // socket. A live process keeps its socket untouched.
                            const std::string state =
                                fetched.value.value("state", std::string("UNKNOWN"));
                            if (state == "STOPPED" || state == "FAILED" || state == "BACKOFF") {
                                std::string socket_path;
                                const Error prepared =
                                    ctx.native_backends->prepare_runtime(app_id, socket_path);
                                if (!prepared.ok()) {
                                    audit(ctx, "extension.backend.restart", false,
                                          prepared.message, ip, app_id);
                                    write_lifecycle_error(writer, prepared);
                                    return;
                                }
                            }
                            submit_or_busy(
                                ctx, writer, "service.restart", {{"id", service_id}},
                                [&ctx, writer, app_id, service_id,
                                 ip](SupervisorResult restarted) {
                                    if (restarted.status < 200 || restarted.status >= 300) {
                                        audit(ctx, "extension.backend.restart", false,
                                              restarted.code, ip, app_id);
                                        write_supervisor_failure(writer, restarted);
                                        return;
                                    }
                                    audit(ctx, "extension.backend.restart", true,
                                          "state=restarted", ip, app_id);
                                    write_after_operation(ctx, writer, app_id, service_id, true);
                                });
                        });
                });

    router.POST("/api/extensions/:app_id/data/purge",
                [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
                    if (!backend_ready(ctx, writer)) return;
                    std::string app_id;
                    if (!path_app_id(req.get(), writer, app_id)) return;
                    const std::string ip = http::client_ip(*ctx.config, req.get());

                    nlohmann::json body;
                    Error parse_error;
                    if (!http::parse_json_request(req.get(), body, parse_error)) {
                        http::write_error_to(writer, parse_error);
                        return;
                    }
                    if (!body.contains("app_id") || !body["app_id"].is_string() ||
                        body["app_id"].get<std::string>() != app_id) {
                        audit(ctx, "extension.data_purge", false,
                              "app_id did not match the path", ip, app_id);
                        write_error_slug(writer, 400, "invalid_argument",
                                         "field 'app_id' must exactly match the application id in "
                                         "the path");
                        return;
                    }
                    if (!body.contains("confirm") || !body["confirm"].is_boolean() ||
                        !body["confirm"].get<bool>()) {
                        audit(ctx, "extension.data_purge", false, "confirmation missing", ip,
                              app_id);
                        write_error_slug(writer, 400, "confirmation_required",
                                         "data purge requires \"confirm\": true; user data and "
                                         "application KV are permanently deleted");
                        return;
                    }

                    BackendRecord record;
                    bool found = false;
                    Error error = ctx.packages->store().get_backend(app_id, record, found);
                    if (!error.ok()) {
                        http::write_error_to(writer, error);
                        return;
                    }
                    const std::string service_id =
                        found ? (record.service_id.empty() ? extensions::backend_service_id(app_id)
                                                           : record.service_id)
                              : std::string();

                    auto purge = [&ctx, writer, app_id, ip]() {
                        extensions::NativeBackendInstaller* installer =
                            ctx.packages->backend_installer();
                        if (installer == nullptr) {
                            audit(ctx, "extension.data_purge", false,
                                  "native backend installer is unavailable", ip, app_id);
                            write_error_slug(writer, 503, "native_backend_unavailable",
                                             "the native application backend platform is "
                                             "unavailable");
                            return;
                        }
                        // Only this app's dedicated data directory and KV
                        // namespace. The installer never follows symlinks out of
                        // the EdgeDesk-owned roots; audits are never touched.
                        Error purged = installer->purge_data(app_id);
                        if (!purged.ok()) {
                            audit(ctx, "extension.data_purge", false, purged.message, ip, app_id);
                            http::write_error_to(writer, purged);
                            return;
                        }
                        Error kv = ctx.packages->store().kv_delete_all(app_id);
                        if (!kv.ok()) {
                            audit(ctx, "extension.data_purge", false, kv.message, ip, app_id);
                            http::write_error_to(writer, kv);
                            return;
                        }
                        audit(ctx, "extension.data_purge", true, "data=purged kv=purged", ip,
                              app_id);
                        nlohmann::json out;
                        out["ok"] = true;
                        out["purged"] = true;
                        http::write_json_to(writer, 200, out);
                    };

                    if (!found || ctx.supervisor == nullptr || !ctx.supervisor->configured()) {
                        // No Supervisor is configured, so no managed backend can
                        // be running. The purge stays synchronous here (the same
                        // model as extension uninstall); when a Supervisor is
                        // available the check and purge run on its worker.
                        purge();
                        return;
                    }

                    submit_or_busy(
                        ctx, writer, "service.get", {{"id", service_id}},
                        [&ctx, writer, app_id, service_id, ip, purge](SupervisorResult fetched) {
                            if (fetched.status == 404 || fetched.code == "service_not_found") {
                                purge();
                                return;
                            }
                            if (fetched.status < 200 || fetched.status >= 300) {
                                audit(ctx, "extension.data_purge", false,
                                      "cannot confirm the backend is stopped", ip, app_id);
                                write_supervisor_failure(
                                    writer, fetched,
                                    "cannot confirm the backend is stopped; data was not "
                                    "purged: ");
                                return;
                            }
                            const std::string state =
                                fetched.value.value("state", std::string("UNKNOWN"));
                            if (state != "STOPPED" && state != "FAILED") {
                                audit(ctx, "extension.data_purge", false,
                                      "backend may be running: " + state, ip, app_id);
                                write_error_slug(writer, 409, "backend_running",
                                                 "stop the backend and wait until it is fully "
                                                 "stopped before purging data");
                                return;
                            }
                            purge();
                        });
                });
}

} // namespace edgedesk
