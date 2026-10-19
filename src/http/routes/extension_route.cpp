#include "http/routes/extension_route.hpp"

#include "core/types.hpp"
#include "core/util.hpp"
#include "extensions/extension_runtime.hpp"
#include "extensions/package_manager.hpp"
#include "http/upload_state.hpp"
#include "security/auth.hpp"
#include "terminal/terminal_manager.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

namespace edgedesk {
namespace {

using extensions::AppRecord;
using edgedesk::ExtensionUploadRequestState;
using extensions::InstallOptions;
using extensions::InstallResult;
using extensions::IntegrityResult;
using extensions::ListPage;
using extensions::Manifest;
using extensions::PackageManager;
using extensions::Permission;
using extensions::RevisionRecord;
using extensions::RuntimeSession;
using extensions::StaticAsset;
using extensions::TransferInfo;

bool manager_ready(RouteContext& ctx, HttpResponse* resp) {
    if (ctx.packages != nullptr && ctx.extension_runtime != nullptr) return true;
    http::write_error(resp, 503, "extensions_unavailable",
                      "extension platform is unavailable; configure a writable --apps-root");
    return false;
}

hv::Json app_json(const AppRecord& app) {
    hv::Json body;
    body["app_id"] = app.app_id;
    body["display_name"] = app.display_name;
    body["status"] = app.status;
    body["active_revision_id"] = app.active_revision_id;
    body["active_version"] = app.active_version;
    body["requested_permissions"] = app.requested_permissions;
    body["granted_permissions"] = app.granted_permissions;
    body["data_retained"] = app.data_retained;
    body["publisher_verified"] = false;
    body["installed_at"] = app.created_at;
    body["updated_at"] = app.updated_at;
    body["icon"] = app.icon;
    if (app.manifest.is_object()) {
        body["description"] = app.manifest.value("description", "");
        body["publisher"] = app.manifest.value("publisher", "");
        body["host_api"] = {
            {"min", app.manifest.value("minHostApiVersion", "")},
            {"max", app.manifest.value("maxHostApiVersion", "")},
        };
        if (app.manifest.contains("window") && app.manifest["window"].is_object()) {
            body["window"] = app.manifest["window"];
        }
        hv::Json origins = hv::Json::array();
        if (app.manifest.contains("network") && app.manifest["network"].is_object() &&
            app.manifest["network"].contains("origins")) {
            origins = app.manifest["network"]["origins"];
        }
        body["network_origins"] = origins;
        if (!app.active_revision_id.empty() && !app.active_version.empty() && !app.icon.empty()) {
            body["icon_url"] =
                "/apps/" + app.app_id + "/" + app.active_version + "/" + app.active_revision_id + "/" + app.icon;
        }
    }
    return body;
}

hv::Json revision_json(const RevisionRecord& revision, const AppRecord& app) {
    hv::Json body;
    body["revision_id"] = revision.revision_id;
    body["version"] = revision.version;
    body["status"] = revision.status;
    body["active"] = revision.revision_id == app.active_revision_id;
    body["pinned"] = revision.pinned;
    body["integrity"] = revision.integrity;
    body["created_at"] = revision.created_at;
    body["file_count"] = revision.file_count;
    body["total_bytes"] = revision.total_bytes;
    return body;
}

int write_runtime_error(HttpResponse* resp, const Error& error, bool storage = false) {
    if (error.code == ErrorCode::conflict && error.message.rfind("CURSOR_STALE", 0) == 0) {
        return http::write_error(resp, 409, "cursor_stale", "directory contents changed; restart from the first page");
    }
    if (error.code == ErrorCode::busy) {
        return http::write_error(resp, 423, "file_locked", error.message);
    }
    if (storage && error.code == ErrorCode::too_large) {
        return http::write_error(resp, 413, "quota_exceeded", error.message);
    }
    return http::write_error(resp, error);
}

bool json_object(HttpRequest* req, hv::Json& body, HttpResponse* resp) {
    Error error;
    if (!http::parse_json_request(req, body, error)) {
        http::write_error(resp, error);
        return false;
    }
    if (!body.is_object()) {
        http::write_error(resp, 400, "invalid_argument", "request body must be a JSON object");
        return false;
    }
    return true;
}

std::vector<std::string> string_array(const hv::Json& body, const char* field, Error& error) {
    std::vector<std::string> out;
    if (!body.contains(field)) return out;
    if (!body[field].is_array()) {
        error = Error::make(ErrorCode::invalid_argument, std::string("field '") + field + "' must be an array");
        return out;
    }
    for (const auto& item : body[field]) {
        if (!item.is_string()) {
            error = Error::make(ErrorCode::invalid_argument,
                                std::string("field '") + field + "' entries must be strings");
            return {};
        }
        out.push_back(item.get<std::string>());
    }
    error = Error::none();
    return out;
}

std::string normalized_audit_path(const RouteContext& ctx, const std::string& path) {
    if (ctx.files == nullptr) return path;
    std::string normalized;
    std::string message;
    if (PathSandbox::normalize(path, normalized, message)) return normalized;
    return path;
}

std::string runtime_detail(const RuntimeSession& session, std::string_view operation) {
    return "op=" + std::string(operation) + " app=" + session.app_id + " instance=" + session.instance_id;
}

bool runtime_session(RouteContext& ctx, HttpRequest* req, HttpResponse* resp, RuntimeSession& session) {
    if (!manager_ready(ctx, resp)) return false;
    const std::string token = req->GetHeader("X-EdgeDesk-Runtime", "");
    if (token.empty()) {
        http::write_error(resp, 401, "runtime_session_invalid", "missing runtime session token");
        return false;
    }
    Error error = ctx.extension_runtime->validate(token, session);
    if (!error.ok()) {
        ctx.audit->record("extension.runtime", "denied", error.message, http::client_ip(*ctx.config, req), "");
        http::write_error(resp, 401, "runtime_session_invalid", "runtime session is invalid or expired");
        return false;
    }
    return true;
}

// ---- management -----------------------------------------------------------

int handle_list(RouteContext& ctx, HttpRequest*, HttpResponse* resp) {
    if (!manager_ready(ctx, resp)) return 503;
    std::vector<AppRecord> apps;
    Error error = ctx.packages->store().list_apps(apps, true);
    if (!error.ok()) return http::write_error(resp, error);
    hv::Json body;
    body["ok"] = true;
    body["apps"] = hv::Json::array();
    for (const auto& app : apps) body["apps"].push_back(app_json(app));
    return http::write_json(resp, 200, body);
}

int handle_detail(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    if (!manager_ready(ctx, resp)) return 503;
    const std::string app_id = req->GetParam("app_id");
    AppRecord app;
    bool found = false;
    Error error = ctx.packages->store().get_app(app_id, app, found);
    if (!error.ok()) return http::write_error(resp, error);
    if (!found) return http::write_error(resp, 404, "app_not_found", "application not found");
    std::vector<RevisionRecord> revisions;
    error = ctx.packages->store().list_revisions(app_id, revisions);
    if (!error.ok()) return http::write_error(resp, error);
    hv::Json body;
    body["ok"] = true;
    body["app"] = app_json(app);
    body["revisions"] = hv::Json::array();
    for (const auto& revision : revisions) body["revisions"].push_back(revision_json(revision, app));
    return http::write_json(resp, 200, body);
}

int handle_approve(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    if (!manager_ready(ctx, resp)) return 503;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    Error array_error;
    const auto permissions = string_array(body, "permissions", array_error);
    if (!array_error.ok()) return http::write_error(resp, array_error);
    const bool enable = body.value("enable", false);
    const std::string app_id = req->GetParam("app_id");
    AppRecord app;
    Error error = ctx.packages->approve(app_id, permissions, enable, app);
    if (!error.ok()) {
        ctx.audit->record("extension.approve", "denied", error.message, http::client_ip(*ctx.config, req), app_id);
        return http::write_error(resp, error);
    }
    ctx.audit->record("extension.approve", "ok",
                      "permissions=" + std::to_string(permissions.size()) + " enable=" + (enable ? "1" : "0"),
                      http::client_ip(*ctx.config, req), app_id);
    hv::Json out;
    out["ok"] = true;
    out["app"] = app_json(app);
    return http::write_json(resp, 200, out);
}

int handle_enable(RouteContext& ctx, HttpRequest* req, HttpResponse* resp, bool enable) {
    if (!manager_ready(ctx, resp)) return 503;
    const std::string app_id = req->GetParam("app_id");
    AppRecord app;
    Error error = ctx.packages->set_enabled(app_id, enable, app);
    if (!error.ok()) {
        ctx.audit->record(enable ? "extension.enable" : "extension.disable", "denied", error.message,
                          http::client_ip(*ctx.config, req), app_id);
        return http::write_error(resp, error);
    }
    if (!enable) ctx.extension_runtime->invalidate_app(app_id);
    ctx.audit->record(enable ? "extension.enable" : "extension.disable", "ok", "",
                      http::client_ip(*ctx.config, req), app_id);
    hv::Json out;
    out["ok"] = true;
    out["app"] = app_json(app);
    return http::write_json(resp, 200, out);
}

int handle_permissions(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    if (!manager_ready(ctx, resp)) return 503;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    Error array_error;
    const auto permissions = string_array(body, "permissions", array_error);
    if (!array_error.ok()) return http::write_error(resp, array_error);
    const std::string app_id = req->GetParam("app_id");
    AppRecord app;
    Error error = ctx.packages->update_permissions(app_id, permissions, app);
    if (!error.ok()) {
        ctx.audit->record("extension.permissions", "denied", error.message, http::client_ip(*ctx.config, req),
                          app_id);
        return http::write_error(resp, error);
    }
    ctx.audit->record("extension.permissions", "ok",
                      "permissions=" + std::to_string(permissions.size()), http::client_ip(*ctx.config, req),
                      app_id);
    hv::Json out;
    out["ok"] = true;
    out["app"] = app_json(app);
    return http::write_json(resp, 200, out);
}

int handle_uninstall(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    if (!manager_ready(ctx, resp)) return 503;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string action = body.value("data_action", "");
    if (action != "keep" && action != "delete") {
        return http::write_error(resp, 400, "invalid_argument", "data_action must be 'keep' or 'delete'");
    }
    const bool keep = action == "keep";
    const std::string app_id = req->GetParam("app_id");
    Error error = ctx.packages->uninstall(app_id, keep);
    if (!error.ok()) {
        ctx.audit->record("extension.uninstall", "denied", error.message, http::client_ip(*ctx.config, req),
                          app_id);
        return http::write_error(resp, error);
    }
    ctx.extension_runtime->invalidate_app(app_id);
    ctx.audit->record("extension.uninstall", "ok", keep ? "data=keep" : "data=delete",
                      http::client_ip(*ctx.config, req), app_id);
    hv::Json out;
    out["ok"] = true;
    return http::write_json(resp, 200, out);
}

int handle_activate(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    if (!manager_ready(ctx, resp)) return 503;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string revision_id = body.value("revision_id", "");
    const bool confirm = body.value("confirm", false);
    if (revision_id.empty()) {
        return http::write_error(resp, 400, "invalid_argument", "revision_id is required");
    }
    const std::string app_id = req->GetParam("app_id");
    AppRecord app;
    Error error = ctx.packages->activate_revision(app_id, revision_id, confirm, app);
    if (!error.ok()) {
        ctx.audit->record("extension.activate", "denied", error.message, http::client_ip(*ctx.config, req),
                          app_id);
        return http::write_error(resp, error);
    }
    ctx.extension_runtime->invalidate_app(app_id);
    ctx.audit->record("extension.activate", "ok", "revision=" + revision_id,
                      http::client_ip(*ctx.config, req), app_id);
    hv::Json out;
    out["ok"] = true;
    out["app"] = app_json(app);
    return http::write_json(resp, 200, out);
}

int handle_pin(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    if (!manager_ready(ctx, resp)) return 503;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string revision_id = body.value("revision_id", "");
    if (revision_id.empty()) {
        return http::write_error(resp, 400, "invalid_argument", "revision_id is required");
    }
    const bool pinned = body.value("pinned", true);
    const std::string app_id = req->GetParam("app_id");
    Error error = ctx.packages->pin_revision(app_id, revision_id, pinned);
    if (!error.ok()) return http::write_error(resp, error);
    ctx.audit->record("extension.pin", "ok", "revision=" + revision_id + (pinned ? " pinned" : " unpinned"),
                      http::client_ip(*ctx.config, req), app_id);
    hv::Json out;
    out["ok"] = true;
    return http::write_json(resp, 200, out);
}

int handle_integrity(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    if (!manager_ready(ctx, resp)) return 503;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string revision_id = body.value("revision_id", "");
    const std::string app_id = req->GetParam("app_id");
    IntegrityResult result;
    Error error = ctx.packages->check_integrity(app_id, revision_id, result);
    if (!error.ok()) {
        ctx.audit->record("extension.integrity", "denied", error.message, http::client_ip(*ctx.config, req),
                          app_id);
        return http::write_error(resp, error);
    }
    // A historical revision mismatch marks only that revision non-runnable.
    // Runtime sessions and grants are invalidated solely when the app's active
    // revision is the one that failed verification.
    if (result.app_isolated) ctx.extension_runtime->invalidate_app(app_id);
    ctx.audit->record("extension.integrity", result.ok ? "ok" : "mismatch",
                      "revision=" + result.revision_id + " path=" + result.mismatch_path +
                          (result.ok ? "" : (result.app_isolated ? " app_isolated" : " revision_isolated")),
                      http::client_ip(*ctx.config, req), app_id);
    hv::Json out;
    out["ok"] = result.ok;
    out["revision_id"] = result.revision_id;
    out["mismatch_path"] = result.mismatch_path;
    out["app_isolated"] = result.app_isolated;
    return http::write_json(resp, result.ok ? 200 : 409, out);
}

int handle_revisions(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    if (!manager_ready(ctx, resp)) return 503;
    const std::string app_id = req->GetParam("app_id");
    AppRecord app;
    bool found = false;
    Error error = ctx.packages->store().get_app(app_id, app, found);
    if (!error.ok()) return http::write_error(resp, error);
    if (!found) return http::write_error(resp, 404, "app_not_found", "application not found");
    std::vector<RevisionRecord> revisions;
    error = ctx.packages->store().list_revisions(app_id, revisions);
    if (!error.ok()) return http::write_error(resp, error);
    hv::Json out;
    out["ok"] = true;
    out["revisions"] = hv::Json::array();
    for (const auto& revision : revisions) out["revisions"].push_back(revision_json(revision, app));
    return http::write_json(resp, 200, out);
}

// ---- streaming install/upgrade -------------------------------------------

bool write_all(int fd, const char* data, std::size_t length) {
    std::size_t done = 0;
    while (done < length) {
        const auto written = ::write(fd, data + done, length - done);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        done += static_cast<std::size_t>(written);
    }
    return true;
}

int handle_extension_upload_state(RouteContext& ctx, const HttpContextPtr& http, http_parser_state state,
                                  const char* data, std::size_t size) {
    switch (state) {
        case HP_HEADERS_COMPLETE: {
            auto scope = std::make_unique<ExtensionUploadRequestState>();
            scope->started_ms = now_unix_millis();
            scope->app_id = http->request->GetParam("app_id");
            scope->restore_data = http::query_bool(http->request.get(), "restore_data", false);
            scope->overwrite_same_version = http::query_bool(http->request.get(), "overwrite", false);
            scope->allow_downgrade = http::query_bool(http->request.get(), "allow_downgrade", false);
            if (ctx.packages == nullptr) {
                scope->failed = true;
                scope->error = Error::make(ErrorCode::unavailable, "extension platform is unavailable");
            } else {
                scope->temp_path = ctx.packages->temporary_directory() + "/upload-" + random_hex(16) + ".zip";
                const int fd = ::open(scope->temp_path.c_str(),
                                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
                if (fd < 0) {
                    scope->failed = true;
                    scope->error = Error::make(ErrorCode::io_error,
                                               "cannot create package temporary file: " +
                                                   std::string(std::strerror(errno)));
                } else {
                    scope->fd.reset(fd);
                }
            }
            http->userdata = scope.release();
            return 0;
        }
        case HP_BODY: {
            auto* scope = static_cast<ExtensionUploadRequestState*>(http->userdata);
            if (scope == nullptr || scope->failed || size == 0) return 0;
            if (!write_all(scope->fd.get(), data, size)) {
                scope->failed = true;
                scope->error = Error::make(ErrorCode::io_error,
                                           "cannot write package temporary file: " +
                                               std::string(std::strerror(errno)));
                return 0;
            }
            scope->received += size;
            return 0;
        }
        case HP_MESSAGE_COMPLETE: {
            std::unique_ptr<ExtensionUploadRequestState> scope(
                static_cast<ExtensionUploadRequestState*>(http->userdata));
            http->userdata = nullptr;
            if (scope == nullptr) {
                return http::write_error(http->response.get(), 400, "invalid_argument", "upload state missing");
            }
            if (scope->fd.valid() && ::fsync(scope->fd.get()) != 0) {
                scope->failed = true;
                scope->error = Error::make(ErrorCode::io_error, "cannot flush package temporary file");
            }
            scope->fd.reset();
            const bool failed = scope->failed;
            Error error = scope->error;
            InstallResult result;
            if (!failed) {
                if (ctx.packages == nullptr) {
                    error = Error::make(ErrorCode::unavailable, "extension platform is unavailable");
                } else {
                    InstallOptions options;
                    options.expected_app_id = scope->app_id;
                    options.restore_retained_data = scope->restore_data;
                    options.overwrite_same_version = scope->overwrite_same_version;
                    options.allow_downgrade = scope->allow_downgrade;
                    error = ctx.packages->install(scope->temp_path, options, result);
                }
            }
            if (!scope->temp_path.empty()) ::unlink(scope->temp_path.c_str());
            if (!error.ok() || failed) {
                if (failed) error = scope->error;
                ctx.audit->record(scope->app_id.empty() ? "extension.install" : "extension.upgrade",
                                  "denied", error.message,
                                  http::client_ip(*ctx.config, http->request.get()), scope->app_id);
                return http::write_error(http->response.get(), error);
            }
            if (ctx.extension_runtime != nullptr) ctx.extension_runtime->invalidate_app(result.app.app_id);
            ctx.audit->record(scope->app_id.empty() ? "extension.install" : "extension.upgrade", "ok",
                              "revision=" + result.revision.revision_id + " version=" + result.revision.version +
                                  " bytes=" + std::to_string(scope->received),
                              http::client_ip(*ctx.config, http->request.get()), result.app.app_id);
            hv::Json body;
            body["ok"] = true;
            body["app"] = app_json(result.app);
            body["revision"] = revision_json(result.revision, result.app);
            body["warnings"] = result.warnings;
            body["approval_required"] = result.approval_required;
            body["retained_data_restored"] = result.retained_data_restored;
            return http::write_json(http->response.get(), 201, body);
        }
        case HP_ERROR: {
            auto* scope = static_cast<ExtensionUploadRequestState*>(http->userdata);
            http->userdata = nullptr;
            if (scope != nullptr) {
                scope->fd.reset();
                if (!scope->temp_path.empty()) ::unlink(scope->temp_path.c_str());
                ctx.audit->record(scope->app_id.empty() ? "extension.install" : "extension.upgrade",
                                  "aborted", "connection closed",
                                  http::client_ip(*ctx.config, http->request.get()), scope->app_id);
                delete scope;
            }
            return 0;
        }
        default:
            return 0;
    }
}

// ---- runtime: sessions ---------------------------------------------------

int handle_runtime_create_session(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    if (!manager_ready(ctx, resp)) return 503;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string app_id = body.value("app_id", "");
    const std::string instance_id = body.value("instance_id", "");
    if (app_id.empty() || instance_id.empty()) {
        return http::write_error(resp, 400, "invalid_argument", "app_id and instance_id are required");
    }
    RuntimeSession session;
    Error error = ctx.extension_runtime->create_session(app_id, instance_id, session);
    if (!error.ok()) {
        ctx.audit->record("extension.runtime", "denied", error.message, http::client_ip(*ctx.config, req),
                          app_id);
        return write_runtime_error(resp, error);
    }
    AppRecord app;
    bool found = false;
    ctx.packages->store().get_app(app_id, app, found);
    hv::Json out;
    out["ok"] = true;
    out["token"] = session.token;
    out["app_id"] = session.app_id;
    out["instance_id"] = session.instance_id;
    out["revision_id"] = session.revision_id;
    out["version"] = session.version;
    out["permissions"] = session.permissions;
    out["origins"] = session.origins;
    out["host_api_version"] = extensions::kHostApiVersion;
    out["static_base"] =
        "/apps/" + session.app_id + "/" + session.version + "/" + session.revision_id + "/";
    out["display_name"] = app.display_name;
    if (app.manifest.is_object() && app.manifest.contains("window")) out["window"] = app.manifest["window"];
    ctx.audit->record("extension.runtime", "start", runtime_detail(session, "session.create"),
                      http::client_ip(*ctx.config, req), app_id);
    return http::write_json(resp, 201, out);
}

int handle_runtime_remove_session(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    if (!manager_ready(ctx, resp)) return 503;
    const std::string token = req->GetHeader("X-EdgeDesk-Runtime", "");
    ctx.extension_runtime->remove_session(token);
    hv::Json out;
    out["ok"] = true;
    return http::write_json(resp, 200, out);
}

// ---- runtime: storage ----------------------------------------------------

int handle_storage_get(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::storage);
    if (!permission.ok()) return write_runtime_error(resp, permission, true);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string key = body.value("key", "");
    extensions::KvEntry entry;
    bool found = false;
    Error error = ctx.packages->store().kv_get(session.app_id, key, entry, found);
    if (!error.ok()) return write_runtime_error(resp, error, true);
    if (!found) return http::write_error(resp, 404, "not_found", "storage key not found");
    const auto value = hv::Json::parse(entry.value, nullptr, false);
    hv::Json out;
    out["ok"] = true;
    out["key"] = entry.key;
    out["value"] = value.is_discarded() ? hv::Json(entry.value) : value;
    out["size_bytes"] = entry.size_bytes;
    out["updated_at"] = entry.updated_at;
    ctx.audit->record("extension.storage", "ok", runtime_detail(session, "get"),
                      http::client_ip(*ctx.config, req), "");
    return http::write_json(resp, 200, out);
}

int handle_storage_set(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::storage);
    if (!permission.ok()) return write_runtime_error(resp, permission, true);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string key = body.value("key", "");
    if (!body.contains("value")) return http::write_error(resp, 400, "invalid_argument", "field 'value' is required");
    const std::string value = body["value"].dump();
    std::uint64_t usage = 0;
    Error error = ctx.packages->store().kv_set(session.app_id, key, value, usage);
    if (!error.ok()) {
        ctx.audit->record("extension.storage", "denied", runtime_detail(session, "set"),
                          http::client_ip(*ctx.config, req), "");
        return write_runtime_error(resp, error, true);
    }
    ctx.audit->record("extension.storage", "ok", runtime_detail(session, "set"),
                      http::client_ip(*ctx.config, req), "");
    hv::Json out;
    out["ok"] = true;
    out["key"] = key;
    out["size_bytes"] = value.size();
    out["usage_bytes"] = usage;
    return http::write_json(resp, 200, out);
}

int handle_storage_delete(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::storage);
    if (!permission.ok()) return write_runtime_error(resp, permission, true);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string key = body.value("key", "");
    bool deleted = false;
    std::uint64_t usage = 0;
    Error error = ctx.packages->store().kv_delete(session.app_id, key, deleted, usage);
    if (!error.ok()) return write_runtime_error(resp, error, true);
    ctx.audit->record("extension.storage", "ok", runtime_detail(session, "delete"),
                      http::client_ip(*ctx.config, req), "");
    hv::Json out;
    out["ok"] = true;
    out["deleted"] = deleted;
    out["usage_bytes"] = usage;
    return http::write_json(resp, 200, out);
}

int handle_storage_list(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::storage);
    if (!permission.ok()) return write_runtime_error(resp, permission, true);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string cursor = body.value("cursor", "");
    const int limit = body.value("limit", 100);
    std::string after;
    if (!cursor.empty()) {
        if (!ctx.extension_runtime->decode_cursor(session.app_id, "storage", cursor, after)) {
            return http::write_error(resp, 409, "cursor_stale", "invalid storage cursor");
        }
    }
    std::vector<extensions::KvEntry> entries;
    std::string next;
    std::uint64_t usage = 0;
    Error error = ctx.packages->store().kv_list(session.app_id, after, limit, entries, next, usage);
    if (!error.ok()) return write_runtime_error(resp, error, true);
    hv::Json out;
    out["ok"] = true;
    out["entries"] = hv::Json::array();
    for (const auto& entry : entries) {
        hv::Json item;
        item["key"] = entry.key;
        item["size_bytes"] = entry.size_bytes;
        item["updated_at"] = entry.updated_at;
        out["entries"].push_back(item);
    }
    out["next_cursor"] = next.empty() ? "" : ctx.extension_runtime->encode_cursor(session.app_id, "storage", next);
    out["usage_bytes"] = usage;
    out["quota_bytes"] = extensions::kAppKvQuotaBytes;
    return http::write_json(resp, 200, out);
}

int handle_storage_transaction(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::storage);
    if (!permission.ok()) return write_runtime_error(resp, permission, true);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    if (!body.contains("operations") || !body["operations"].is_array()) {
        return http::write_error(resp, 400, "invalid_argument", "field 'operations' must be an array");
    }
    std::vector<extensions::KvOperation> operations;
    for (const auto& item : body["operations"]) {
        if (!item.is_object()) {
            return http::write_error(resp, 400, "invalid_argument", "operations entries must be objects");
        }
        const std::string op = item.value("op", "");
        extensions::KvOperation operation;
        if (op == "set") {
            operation.kind = extensions::KvOperationKind::set;
            operation.key = item.value("key", "");
            if (!item.contains("value")) {
                return http::write_error(resp, 400, "invalid_argument", "set operations require 'value'");
            }
            operation.value = item["value"].dump();
        } else if (op == "delete") {
            operation.kind = extensions::KvOperationKind::erase;
            operation.key = item.value("key", "");
        } else {
            return http::write_error(resp, 400, "invalid_argument", "operation must be 'set' or 'delete'");
        }
        operations.push_back(std::move(operation));
    }
    std::uint64_t usage = 0;
    Error error = ctx.packages->store().kv_transaction(session.app_id, operations, usage);
    if (!error.ok()) {
        ctx.audit->record("extension.storage", "denied", runtime_detail(session, "transaction"),
                          http::client_ip(*ctx.config, req), "");
        return write_runtime_error(resp, error, true);
    }
    ctx.audit->record("extension.storage", "ok", runtime_detail(session, "transaction"),
                      http::client_ip(*ctx.config, req), "");
    hv::Json out;
    out["ok"] = true;
    out["applied"] = operations.size();
    out["usage_bytes"] = usage;
    return http::write_json(resp, 200, out);
}

// ---- runtime: files ------------------------------------------------------

int handle_files_list(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::files_manage);
    if (!permission.ok()) return write_runtime_error(resp, permission);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string path = body.value("path", "/");
    const std::string cursor = body.value("cursor", "");
    const int limit = body.value("limit", extensions::kListPageDefault);
    ListPage page;
    Error error = ctx.extension_runtime->list_directory(session, path, cursor, limit, page);
    if (!error.ok()) {
        ctx.audit->record("extension.file", "denied", runtime_detail(session, "list"),
                          http::client_ip(*ctx.config, req), normalized_audit_path(ctx, path));
        return write_runtime_error(resp, error);
    }
    ctx.audit->record("extension.file", "ok", runtime_detail(session, "list"),
                      http::client_ip(*ctx.config, req), normalized_audit_path(ctx, path));
    hv::Json out;
    out["ok"] = true;
    out["path"] = page.path;
    out["entries"] = hv::Json::array();
    for (const auto& entry : page.entries) out["entries"].push_back(http::file_entry_json(entry));
    out["next_cursor"] = page.next_cursor;
    out["sort"] = "directories-first-name-ascending";
    return http::write_json(resp, 200, out);
}

int handle_files_stat(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::files_manage);
    if (!permission.ok()) return write_runtime_error(resp, permission);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string path = body.value("path", "");
    if (path.empty()) return http::write_error(resp, 400, "invalid_argument", "path is required");
    FileEntry entry;
    Error error = ctx.extension_runtime->files().stat(path, entry);
    if (!error.ok()) return write_runtime_error(resp, error);
    hv::Json out;
    out["ok"] = true;
    out["entry"] = http::file_entry_json(entry);
    return http::write_json(resp, 200, out);
}

int handle_files_read(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::files_manage);
    if (!permission.ok()) return write_runtime_error(resp, permission);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string path = body.value("path", "");
    if (path.empty()) return http::write_error(resp, 400, "invalid_argument", "path is required");
    std::string content;
    FileEntry entry;
    Error error = ctx.extension_runtime->files().read_text(path, content, entry);
    std::string encoding = "utf-8";
    if (!error.ok() && error.code == ErrorCode::unsupported) {
        FileService::ReadHandle handle;
        error = ctx.extension_runtime->files().open_for_read(path, handle);
        if (error.ok()) {
            if (handle.size > 1024 * 1024) {
                return http::write_error(resp, 413, "too_large",
                                         "use a download transfer for files above 1 MiB");
            }
            content.resize(static_cast<std::size_t>(handle.size));
            std::size_t offset = 0;
            while (offset < content.size()) {
                const auto read = ::pread(handle.fd.get(), content.data() + offset, content.size() - offset,
                                          static_cast<off_t>(offset));
                if (read < 0) {
                    if (errno == EINTR) continue;
                    return http::write_error(resp, 500, "io_error", "cannot read file");
                }
                if (read == 0) break;
                offset += static_cast<std::size_t>(read);
            }
            content.resize(offset);
            content = base64_encode(content);
            encoding = "base64";
            entry = FileEntry{};
            entry.path = path;
            entry.name = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
            entry.is_file = true;
            entry.size = handle.size;
            entry.mtime = handle.mtime;
            entry.mime = handle.mime;
        }
    }
    if (!error.ok()) {
        ctx.audit->record("extension.file", "denied", runtime_detail(session, "read"),
                          http::client_ip(*ctx.config, req), normalized_audit_path(ctx, path));
        return write_runtime_error(resp, error);
    }
    ctx.audit->record("extension.file", "ok", runtime_detail(session, "read"),
                      http::client_ip(*ctx.config, req), normalized_audit_path(ctx, path));
    hv::Json out;
    out["ok"] = true;
    out["entry"] = http::file_entry_json(entry);
    out["encoding"] = encoding;
    out["content"] = content;
    return http::write_json(resp, 200, out);
}

int handle_files_mkdir(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::files_manage);
    if (!permission.ok()) return write_runtime_error(resp, permission);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string path = body.value("path", "");
    if (path.empty()) return http::write_error(resp, 400, "invalid_argument", "path is required");
    Error error = ctx.extension_runtime->files().mkdir(path);
    if (!error.ok()) {
        ctx.audit->record("extension.file", "denied", runtime_detail(session, "mkdir"),
                          http::client_ip(*ctx.config, req), normalized_audit_path(ctx, path));
        return write_runtime_error(resp, error);
    }
    ctx.audit->record("extension.file", "ok", runtime_detail(session, "mkdir"),
                      http::client_ip(*ctx.config, req), normalized_audit_path(ctx, path));
    hv::Json out;
    out["ok"] = true;
    return http::write_json(resp, 201, out);
}

int handle_files_rename(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::files_manage);
    if (!permission.ok()) return write_runtime_error(resp, permission);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string from = body.value("from", "");
    const std::string to = body.value("to", "");
    if (from.empty() || to.empty()) {
        return http::write_error(resp, 400, "invalid_argument", "from and to are required");
    }
    Error error = ctx.extension_runtime->files().rename(from, to, body.value("overwrite", false));
    if (!error.ok()) {
        ctx.audit->record("extension.file", "denied", runtime_detail(session, "rename"),
                          http::client_ip(*ctx.config, req), normalized_audit_path(ctx, from));
        return write_runtime_error(resp, error);
    }
    ctx.audit->record("extension.file", "ok", runtime_detail(session, "rename"),
                      http::client_ip(*ctx.config, req), normalized_audit_path(ctx, from));
    hv::Json out;
    out["ok"] = true;
    return http::write_json(resp, 200, out);
}

int handle_files_delete(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::files_manage);
    if (!permission.ok()) return write_runtime_error(resp, permission);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    if (!body.contains("paths") || !body["paths"].is_array()) {
        return http::write_error(resp, 400, "invalid_argument", "paths must be an array");
    }
    std::vector<std::string> paths;
    for (const auto& item : body["paths"]) {
        if (!item.is_string()) return http::write_error(resp, 400, "invalid_argument", "paths entries must be strings");
        paths.push_back(item.get<std::string>());
    }
    RemoveResult result;
    Error error = ctx.extension_runtime->files().remove(paths, result);
    if (!error.ok()) {
        ctx.audit->record("extension.file", "denied", runtime_detail(session, "delete"),
                          http::client_ip(*ctx.config, req), paths.empty() ? "" : normalized_audit_path(ctx, paths.front()));
        return write_runtime_error(resp, error);
    }
    ctx.audit->record("extension.file", "ok", runtime_detail(session, "delete"),
                      http::client_ip(*ctx.config, req), paths.empty() ? "" : normalized_audit_path(ctx, paths.front()));
    hv::Json out;
    out["ok"] = true;
    out["removed_files"] = result.removed_files;
    out["removed_dirs"] = result.removed_dirs;
    return http::write_json(resp, 200, out);
}

int handle_upload_begin(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string path = body.value("path", "");
    const auto size = body.value("size", static_cast<std::int64_t>(-1));
    if (path.empty() || size < 0) {
        return http::write_error(resp, 400, "invalid_argument", "path and non-negative size are required");
    }
    TransferInfo info;
    Error error = ctx.extension_runtime->begin_upload(session, path, static_cast<std::uint64_t>(size), info);
    if (!error.ok()) return write_runtime_error(resp, error);
    hv::Json out;
    out["ok"] = true;
    out["transfer_id"] = info.transfer_id;
    out["chunk_size"] = info.chunk_size;
    out["total_size"] = info.total_size;
    return http::write_json(resp, 200, out);
}

int handle_upload_chunk(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    const std::string transfer_id = req->GetParam("transfer_id");
    std::uint64_t offset = 0;
    const std::string offset_text = http::query_string(req, "offset");
    if (!parse_uint64(offset_text, offset)) {
        return http::write_error(resp, 400, "invalid_argument", "offset query parameter is required");
    }
    std::uint64_t received = 0;
    Error error = ctx.extension_runtime->append_upload(session, transfer_id, offset, req->body.data(),
                                                       req->body.size(), received);
    if (!error.ok()) return write_runtime_error(resp, error);
    hv::Json out;
    out["ok"] = true;
    out["received"] = received;
    return http::write_json(resp, 200, out);
}

int handle_upload_complete(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string transfer_id = body.value("transfer_id", "");
    const auto expected = body.value("expected_size", static_cast<std::int64_t>(-1));
    if (transfer_id.empty() || expected < 0) {
        return http::write_error(resp, 400, "invalid_argument", "transfer_id and expected_size are required");
    }
    FileEntry entry;
    Error error = ctx.extension_runtime->finish_upload(session, transfer_id,
                                                       static_cast<std::uint64_t>(expected), entry);
    if (!error.ok()) {
        ctx.audit->record("extension.file", "denied", runtime_detail(session, "upload"),
                          http::client_ip(*ctx.config, req), normalized_audit_path(ctx, entry.path));
        return write_runtime_error(resp, error);
    }
    ctx.audit->record("extension.file", "ok", runtime_detail(session, "upload"),
                      http::client_ip(*ctx.config, req), normalized_audit_path(ctx, entry.path));
    hv::Json out;
    out["ok"] = true;
    out["entry"] = http::file_entry_json(entry);
    return http::write_json(resp, 201, out);
}

int handle_upload_cancel(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    Error error = ctx.extension_runtime->cancel_upload(session, body.value("transfer_id", ""));
    if (!error.ok()) return write_runtime_error(resp, error);
    hv::Json out;
    out["ok"] = true;
    return http::write_json(resp, 200, out);
}

int handle_download_begin(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string path = body.value("path", "");
    if (path.empty()) return http::write_error(resp, 400, "invalid_argument", "path is required");
    TransferInfo info;
    Error error = ctx.extension_runtime->begin_download(session, path, info);
    if (!error.ok()) {
        ctx.audit->record("extension.file", "denied", runtime_detail(session, "download"),
                          http::client_ip(*ctx.config, req), normalized_audit_path(ctx, path));
        return write_runtime_error(resp, error);
    }
    ctx.audit->record("extension.file", "ok",
                      runtime_detail(session, "download.begin") + " bytes=" + std::to_string(info.total_size),
                      http::client_ip(*ctx.config, req), normalized_audit_path(ctx, path));
    hv::Json out;
    out["ok"] = true;
    out["transfer_id"] = info.transfer_id;
    out["chunk_size"] = info.chunk_size;
    out["total_size"] = info.total_size;
    return http::write_json(resp, 200, out);
}

int handle_download_chunk(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    const std::string transfer_id = req->GetParam("transfer_id");
    std::uint64_t offset = 0;
    if (!parse_uint64(http::query_string(req, "offset"), offset)) {
        return http::write_error(resp, 400, "invalid_argument", "offset query parameter is required");
    }
    std::string data;
    bool eof = false;
    Error error = ctx.extension_runtime->read_download(session, transfer_id, offset, data, eof);
    if (!error.ok()) return write_runtime_error(resp, error);
    resp->headers["Content-Type"] = "application/octet-stream";
    resp->headers["X-EdgeDesk-EOF"] = eof ? "1" : "0";
    resp->headers["Cache-Control"] = "no-store";
    resp->content_length = data.size();
    resp->body = std::move(data);
    return 200;
}

int handle_download_complete(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    Error error = ctx.extension_runtime->finish_download(session, body.value("transfer_id", ""));
    if (!error.ok()) return write_runtime_error(resp, error);
    ctx.audit->record("extension.file", "ok", runtime_detail(session, "download.complete"),
                      http::client_ip(*ctx.config, req), "");
    hv::Json out;
    out["ok"] = true;
    return http::write_json(resp, 200, out);
}

int handle_download_cancel(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    Error error = ctx.extension_runtime->cancel_download(session, body.value("transfer_id", ""));
    if (!error.ok()) return write_runtime_error(resp, error);
    hv::Json out;
    out["ok"] = true;
    return http::write_json(resp, 200, out);
}

int handle_runtime_error_report(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    std::string type = body.value("type", "error");
    std::string message = body.value("message", "");
    std::string stack = body.value("stack", "");
    if (type.size() > 120) type.resize(120);
    if (message.size() > 1024) message.resize(1024);
    if (stack.size() > 2048) stack.resize(2048);
    ctx.audit->record("extension.error", "reported",
                      runtime_detail(session, "error") + " revision=" + session.revision_id + " type=" + type +
                          " message=" + message + " stack=" + stack,
                      http::client_ip(*ctx.config, req), "");
    hv::Json out;
    out["ok"] = true;
    return http::write_json(resp, 200, out);
}

// ---- runtime: terminal ---------------------------------------------------

hv::Json terminal_info_json(const TerminalSessionInfo& info) {
    hv::Json body;
    body["id"] = info.id;
    body["title"] = info.title;
    body["cwd"] = info.cwd;
    body["created_ms"] = info.created_ms;
    body["last_activity_ms"] = info.last_activity_ms;
    body["cols"] = info.cols;
    body["rows"] = info.rows;
    body["running"] = info.running;
    body["exited"] = info.exited;
    body["exit_code"] = info.exit_code;
    body["subscribers"] = info.subscribers;
    body["buffered_bytes"] = info.buffered_bytes;
    body["pending_confirmation"] = info.pending_confirmation;
    return body;
}

int handle_terminal_list(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::terminal_access);
    if (!permission.ok()) return write_runtime_error(resp, permission);
    hv::Json out;
    out["ok"] = true;
    out["sessions"] = hv::Json::array();
    for (const auto& info : ctx.terminals->list()) out["sessions"].push_back(terminal_info_json(info));
    out["shell"] = ctx.terminals->shell();
    out["shell_available"] = ctx.terminals->shell_available();
    out["max_sessions"] = ctx.terminals->options().max_sessions;
    ctx.audit->record("extension.terminal", "ok", runtime_detail(session, "list"),
                      http::client_ip(*ctx.config, req), "");
    return http::write_json(resp, 200, out);
}

int handle_terminal_create(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::terminal_access);
    if (!permission.ok()) return write_runtime_error(resp, permission);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    TerminalManager::CreateRequest request;
    request.title = body.value("title", "");
    request.cwd = body.value("cwd", "");
    const auto cols = body.value("cols", 100);
    const auto rows = body.value("rows", 30);
    if (cols < 1 || cols > 1000 || rows < 1 || rows > 1000) {
        return http::write_error(resp, 400, "invalid_argument", "cols and rows must be between 1 and 1000");
    }
    request.cols = static_cast<unsigned short>(cols);
    request.rows = static_cast<unsigned short>(rows);
    request.extra_env["EDGEDESK_APP_ID"] = session.app_id;
    request.extra_env["EDGEDESK_APP_INSTANCE"] = session.instance_id;
    std::shared_ptr<TerminalSession> created;
    Error error = ctx.terminals->create(request, created);
    if (!error.ok()) {
        ctx.audit->record("extension.terminal", "denied", runtime_detail(session, "create") + " " + error.message,
                          http::client_ip(*ctx.config, req), "");
        return write_runtime_error(resp, error);
    }
    ctx.audit->record("extension.terminal", "ok",
                      runtime_detail(session, "create") + " session=" + created->id(),
                      http::client_ip(*ctx.config, req), "");
    ctx.extension_runtime->track_terminal(session.token, created->id());
    hv::Json out;
    out["ok"] = true;
    out["session"] = terminal_info_json(created->info());
    return http::write_json(resp, 201, out);
}

int handle_terminal_close(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    RuntimeSession session;
    if (!runtime_session(ctx, req, resp, session)) return 401;
    Error permission = extensions::ExtensionRuntime::require_permission(session, Permission::terminal_access);
    if (!permission.ok()) return write_runtime_error(resp, permission);
    hv::Json body;
    if (!json_object(req, body, resp)) return 400;
    const std::string session_id = body.value("session_id", "");
    if (session_id.empty()) return http::write_error(resp, 400, "invalid_argument", "session_id is required");
    const bool closed = ctx.terminals->request_close(session_id);
    ctx.extension_runtime->untrack_terminal(session.token, session_id);
    ctx.audit->record("extension.terminal", closed ? "ok" : "denied",
                      runtime_detail(session, "close") + " session=" + session_id,
                      http::client_ip(*ctx.config, req), "");
    if (!closed) return http::write_error(resp, 404, "session_not_found", "terminal session not found");
    hv::Json out;
    out["ok"] = true;
    return http::write_json(resp, 200, out);
}

} // namespace

int serve_extension_asset(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    if (req->method != HTTP_GET && req->method != HTTP_HEAD) return 405;
    if (ctx.packages == nullptr) {
        resp->status_code = static_cast<http_status>(503);
        resp->headers["Content-Type"] = "text/plain; charset=utf-8";
        resp->body = "extension platform unavailable";
        return 503;
    }
    const std::string path = req->Path();
    if (path.rfind("/apps/", 0) != 0) return 404;
    const std::string rest = path.substr(6);
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= rest.size()) {
        const auto slash = rest.find('/', start);
        const std::string raw = rest.substr(start, slash == std::string::npos ? rest.size() - start : slash - start);
        std::string decoded;
        if (!url_decode(raw, decoded)) return 400;
        segments.push_back(std::move(decoded));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (segments.size() < 3) return 404;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].empty() && i + 1 < segments.size()) return 400;
    }
    const std::string& app_id = segments[0];
    const std::string& version = segments[1];
    const std::string& revision_id = segments[2];
    if (app_id.empty() || version.empty() || revision_id.empty()) return 404;
    std::string relative;
    for (std::size_t i = 3; i < segments.size(); ++i) {
        if (!relative.empty()) relative.push_back('/');
        relative += segments[i];
    }
    StaticAsset asset;
    Error error = ctx.packages->read_static(app_id, version, revision_id, relative, asset);
    if (!error.ok()) {
        const int status = error.code == ErrorCode::not_found ? 404 : 400;
        resp->status_code = static_cast<http_status>(status);
        resp->headers["Content-Type"] = "text/plain; charset=utf-8";
        resp->headers["Cache-Control"] = "no-store";
        resp->body = status == 404 ? "not found" : "invalid application asset path";
        return status;
    }
    resp->headers["Content-Type"] = asset.mime;
    resp->headers["X-Content-Type-Options"] = "nosniff";
    resp->headers["Referrer-Policy"] = "no-referrer";
    resp->headers["X-Frame-Options"] = "SAMEORIGIN";
    resp->headers["Cross-Origin-Resource-Policy"] = "cross-origin";
    resp->headers["Access-Control-Allow-Origin"] = "*";
    resp->headers["Cache-Control"] = "public, max-age=31536000, immutable";
    if (!asset.csp.empty()) {
        // A sandboxed iframe has an opaque origin, so CSP 'self' may not match
        // the asset URL in every browser. Add the exact host explicitly so
        // scripts/modules load while keeping the policy origin-scoped.
        std::string policy = asset.csp;
        const std::string host = req->GetHeader("Host", "");
        if (!host.empty()) {
            const std::string extra = "'self' http://" + host + " https://" + host;
            std::size_t position = 0;
            while ((position = policy.find("'self'", position)) != std::string::npos) {
                policy.replace(position, 6, extra);
                position += extra.size();
            }
        }
        resp->headers["Content-Security-Policy"] = policy;
    }
    if (!asset.etag.empty()) {
        resp->headers["ETag"] = asset.etag;
        const std::string if_none_match = req->GetHeader("If-None-Match", "");
        if (!if_none_match.empty() && if_none_match == asset.etag) {
            resp->status_code = static_cast<http_status>(304);
            resp->body.clear();
            resp->content_length = 0;
            return 304;
        }
    }
    if (req->method == HTTP_GET) {
        resp->content_length = asset.body.size();
        resp->body = std::move(asset.body);
    } else {
        resp->content_length = asset.body.size();
    }
    return 200;
}

void register_extension_routes(hv::HttpService& router, RouteContext& ctx) {
    router.GET("/api/extensions", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_list(ctx, req, resp);
    });
    router.GET("/api/extensions/:app_id", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_detail(ctx, req, resp);
    });
    router.POST("/api/extensions/:app_id/approve", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_approve(ctx, req, resp);
    });
    router.POST("/api/extensions/:app_id/enable", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_enable(ctx, req, resp, true);
    });
    router.POST("/api/extensions/:app_id/disable", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_enable(ctx, req, resp, false);
    });
    router.POST("/api/extensions/:app_id/permissions", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_permissions(ctx, req, resp);
    });
    router.POST("/api/extensions/:app_id/uninstall", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_uninstall(ctx, req, resp);
    });
    router.POST("/api/extensions/:app_id/activate", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_activate(ctx, req, resp);
    });
    router.POST("/api/extensions/:app_id/pin", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_pin(ctx, req, resp);
    });
    router.POST("/api/extensions/:app_id/integrity-check", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_integrity(ctx, req, resp);
    });
    router.GET("/api/extensions/:app_id/revisions", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_revisions(ctx, req, resp);
    });
    router.PUT("/api/extensions/install",
               [&ctx](const HttpContextPtr& http, http_parser_state state, const char* data, std::size_t size) {
                   return handle_extension_upload_state(ctx, http, state, data, size);
               });
    router.PUT("/api/extensions/:app_id/upgrade",
               [&ctx](const HttpContextPtr& http, http_parser_state state, const char* data, std::size_t size) {
                   return handle_extension_upload_state(ctx, http, state, data, size);
               });

    // runtime sessions
    router.POST("/api/extensions/runtime/sessions", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_runtime_create_session(ctx, req, resp);
    });
    router.Delete("/api/extensions/runtime/sessions", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_runtime_remove_session(ctx, req, resp);
    });
    // storage
    router.POST("/api/extensions/runtime/storage/get", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_storage_get(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/storage/set", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_storage_set(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/storage/delete", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_storage_delete(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/storage/list", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_storage_list(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/storage/transaction", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_storage_transaction(ctx, req, resp);
    });
    // files
    router.POST("/api/extensions/runtime/files/list", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_files_list(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/files/stat", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_files_stat(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/files/read", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_files_read(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/files/mkdir", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_files_mkdir(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/files/rename", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_files_rename(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/files/delete", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_files_delete(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/files/upload/begin", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_upload_begin(ctx, req, resp);
    });
    router.PUT("/api/extensions/runtime/files/upload/:transfer_id/chunk",
               [&ctx](HttpRequest* req, HttpResponse* resp) { return handle_upload_chunk(ctx, req, resp); });
    router.POST("/api/extensions/runtime/files/upload/complete", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_upload_complete(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/files/upload/cancel", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_upload_cancel(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/files/download/begin", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_download_begin(ctx, req, resp);
    });
    router.GET("/api/extensions/runtime/files/download/:transfer_id/chunk",
               [&ctx](HttpRequest* req, HttpResponse* resp) { return handle_download_chunk(ctx, req, resp); });
    router.POST("/api/extensions/runtime/files/download/complete", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_download_complete(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/files/download/cancel", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_download_cancel(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/errors", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_runtime_error_report(ctx, req, resp);
    });
    // terminal
    router.GET("/api/extensions/runtime/terminal/sessions", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_terminal_list(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/terminal/sessions", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_terminal_create(ctx, req, resp);
    });
    router.POST("/api/extensions/runtime/terminal/close", [&ctx](HttpRequest* req, HttpResponse* resp) {
        return handle_terminal_close(ctx, req, resp);
    });
}

} // namespace edgedesk
