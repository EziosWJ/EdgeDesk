#include "http/routes/file_route.hpp"

#include "core/types.hpp"
#include "core/util.hpp"
#include "http/upload_state.hpp"

#include <algorithm>
#include <cerrno>
#include <memory>
#include <thread>
#include <vector>

#include <unistd.h>

namespace edgedesk {
namespace {

std::string basename_of(std::string_view path) {
    const auto slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

constexpr std::size_t kDownloadChunk = 64 * 1024;
constexpr std::size_t kDownloadBufferHighWater = 1024 * 1024;
constexpr int kDownloadStallGuardMs = 60 * 1000;

struct TransferGuard {
    ConcurrencyGate* gate = nullptr;
    bool acquired = false;
    explicit TransferGuard(ConcurrencyGate* target) : gate(target) {
        if (gate != nullptr) acquired = gate->try_acquire();
    }
    ~TransferGuard() {
        if (acquired && gate != nullptr) gate->release();
    }
};

std::string download_disposition(const std::string& name) {
    return "attachment; filename*=UTF-8''" + url_encode_component(name);
}

int handle_list(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    ListOptions options;
    options.path = http::query_string(req, "path", "/");
    options.page = static_cast<int>(http::query_int(req, "page", 1, 1, 100000));
    options.page_size = static_cast<int>(http::query_int(req, "page_size", 200, 1, 1000));
    options.sort = http::query_string(req, "sort", "name");
    const auto order = to_lower_ascii(req->GetParam("order"));
    options.desc = order == "desc" || order == "descending";
    options.show_hidden = http::query_bool(req, "hidden", true);
    options.dirs_only = http::query_bool(req, "dirs_only", false);

    ListResult result;
    Error error = ctx.files->list(options, result);
    if (!error.ok()) {
        ctx.audit->record("file.list", "denied", error.message, http::client_ip(*ctx.config, req), options.path);
        return http::write_error(resp, error);
    }
    return http::write_json(resp, 200, http::file_list_json(result));
}

int handle_stat(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    const std::string path = http::query_string(req, "path");
    if (path.empty()) return http::write_error(resp, 400, "invalid_argument", "missing 'path' query parameter");
    FileEntry entry;
    Error error = ctx.files->stat(path, entry);
    if (!error.ok()) return http::write_error(resp, error);
    hv::Json body;
    body["ok"] = true;
    body["entry"] = http::file_entry_json(entry);
    return http::write_json(resp, 200, body);
}

int handle_read_text(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    const std::string path = http::query_string(req, "path");
    if (path.empty()) return http::write_error(resp, 400, "invalid_argument", "missing 'path' query parameter");
    std::string content;
    FileEntry info;
    Error error = ctx.files->read_text(path, content, info);
    if (!error.ok()) {
        ctx.audit->record("file.read_text", "denied", error.message, http::client_ip(*ctx.config, req), path);
        return http::write_error(resp, error);
    }
    hv::Json body;
    body["ok"] = true;
    body["content"] = content;
    body["entry"] = http::file_entry_json(info);
    return http::write_json(resp, 200, body);
}

int handle_write_text(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    const std::string path = http::query_string(req, "path");
    if (path.empty()) return http::write_error(resp, 400, "invalid_argument", "missing 'path' query parameter");
    const std::string& content = req->body;
    if (static_cast<std::uint64_t>(content.size()) > ctx.config->max_text_edit_size) {
        return http::write_error(resp, 413, "too_large", "content exceeds the text edit limit");
    }
    if (!looks_like_utf8_text(content)) {
        return http::write_error(resp, 415, "unsupported", "content must be valid UTF-8 text");
    }
    FileEntry info;
    Error error = ctx.files->write_text(path, content, true, info);
    if (!error.ok()) {
        ctx.audit->record("file.write_text", "denied", error.message, http::client_ip(*ctx.config, req), path);
        return http::write_error(resp, error);
    }
    ctx.audit->record("file.write_text", "ok", std::to_string(content.size()) + " bytes", http::client_ip(*ctx.config, req),
                      path);
    hv::Json body;
    body["ok"] = true;
    body["entry"] = http::file_entry_json(info);
    return http::write_json(resp, 200, body);
}

int handle_mkdir(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    hv::Json body;
    Error error;
    if (!http::parse_json_request(req, body, error)) return http::write_error(resp, error);
    const std::string path = body.value("path", "");
    if (path.empty()) return http::write_error(resp, 400, "invalid_argument", "field 'path' is required");
    error = ctx.files->mkdir(path);
    if (!error.ok()) {
        ctx.audit->record("file.mkdir", "denied", error.message, http::client_ip(*ctx.config, req), path);
        return http::write_error(resp, error);
    }
    ctx.audit->record("file.mkdir", "ok", "", http::client_ip(*ctx.config, req), path);
    hv::Json result;
    result["ok"] = true;
    result["path"] = path;
    return http::write_json(resp, 201, result);
}

int handle_rename(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    hv::Json body;
    Error error;
    if (!http::parse_json_request(req, body, error)) return http::write_error(resp, error);
    const std::string from = body.value("from", "");
    const std::string to = body.value("to", "");
    const bool overwrite = body.value("overwrite", false);
    if (from.empty() || to.empty()) {
        return http::write_error(resp, 400, "invalid_argument", "fields 'from' and 'to' are required");
    }
    error = ctx.files->rename(from, to, overwrite);
    if (!error.ok()) {
        ctx.audit->record("file.rename", "denied", error.message, http::client_ip(*ctx.config, req), from + " -> " + to);
        return http::write_error(resp, error);
    }
    ctx.audit->record("file.rename", "ok", from + " -> " + to, http::client_ip(*ctx.config, req), from);
    hv::Json result;
    result["ok"] = true;
    return http::write_json(resp, 200, result);
}

int handle_delete(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    hv::Json body;
    Error error;
    if (!http::parse_json_request(req, body, error)) return http::write_error(resp, error);
    if (!body.contains("paths") || !body["paths"].is_array()) {
        return http::write_error(resp, 400, "invalid_argument", "field 'paths' must be an array");
    }
    std::vector<std::string> paths;
    for (const auto& item : body["paths"]) {
        if (!item.is_string()) {
            return http::write_error(resp, 400, "invalid_argument", "'paths' entries must be strings");
        }
        paths.push_back(item.get<std::string>());
    }
    RemoveResult result;
    error = ctx.files->remove(paths, result);
    if (!error.ok()) {
        ctx.audit->record("file.delete", "denied", error.message, http::client_ip(*ctx.config, req), paths.front());
        return http::write_error(resp, error);
    }
    std::string detail = std::to_string(result.removed_files) + " files, " + std::to_string(result.removed_dirs) +
                         " dirs";
    ctx.audit->record("file.delete", "ok", detail, http::client_ip(*ctx.config, req), paths.front());
    hv::Json out;
    out["ok"] = true;
    out["removed_files"] = result.removed_files;
    out["removed_dirs"] = result.removed_dirs;
    return http::write_json(resp, 200, out);
}

int handle_download_head(RouteContext& ctx, HttpRequest* req, HttpResponse* resp) {
    const std::string path = http::query_string(req, "path");
    if (path.empty()) return http::write_error(resp, 400, "invalid_argument", "missing 'path' query parameter");
    FileService::ReadHandle handle;
    Error error = ctx.files->open_for_read(path, handle);
    if (!error.ok()) return http::write_error(resp, error);
    resp->headers["Content-Type"] = handle.mime;
    resp->headers["Accept-Ranges"] = "bytes";
    resp->headers["ETag"] = "\"" + std::to_string(handle.size) + "-" + std::to_string(handle.mtime) + "\"";
    resp->headers["Last-Modified"] = http::http_date(handle.mtime);
    resp->headers["Content-Disposition"] = download_disposition(basename_of(path));
    resp->content_length = handle.size;
    return 200;
}

void handle_download(const RouteContext& ctx, const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
    TransferGuard guard(ctx.transfer_gate);
    if (!guard.acquired) {
        http::write_error_to(writer, Error::make(ErrorCode::busy, "too many concurrent transfers; retry shortly"));
        return;
    }
    const std::string path = req->GetParam("path");
    if (path.empty()) {
        http::write_error_to(writer, Error::make(ErrorCode::invalid_argument, "missing 'path' query parameter"));
        return;
    }
    FileService::ReadHandle handle;
    Error error = ctx.files->open_for_read(path, handle);
    if (!error.ok()) {
        ctx.audit->record("file.download", "denied", error.message, http::client_ip(*ctx.config, req.get()), path);
        http::write_error_to(writer, error);
        return;
    }
    const std::string etag = "\"" + std::to_string(handle.size) + "-" + std::to_string(handle.mtime) + "\"";
    const std::string last_modified = http::http_date(handle.mtime);
    std::uint64_t start = 0;
    std::uint64_t end = handle.size > 0 ? handle.size - 1 : 0;
    bool partial = false;

    const std::string range_header = req->GetHeader("Range", "");
    if (!range_header.empty()) {
        bool honor = true;
        const std::string if_range = req->GetHeader("If-Range", "");
        if (!if_range.empty() && if_range != etag && if_range != last_modified) honor = false;
        if (honor) {
            ContentRange range;
            const auto range_status = parse_range_header(range_header, handle.size, range);
            if (range_status == RangeStatus::unsatisfiable) {
                writer->Begin();
                writer->WriteStatus(HTTP_STATUS_RANGE_NOT_SATISFIABLE);
                writer->WriteHeader("Content-Range", "bytes */" + std::to_string(handle.size));
                writer->WriteHeader("Content-Type", "application/json; charset=utf-8");
                writer->WriteHeader("ETag", etag);
                writer->EndHeaders();
                writer->WriteBody(R"({"ok":false,"error":"range_not_satisfiable"})");
                writer->End();
                return;
            }
            if (range_status == RangeStatus::partial) {
                start = range.start;
                end = range.end;
                partial = true;
            }
        }
    }

    const std::uint64_t length = handle.size == 0 ? 0 : end - start + 1;
    writer->Begin();
    writer->WriteStatus(partial ? HTTP_STATUS_PARTIAL_CONTENT : HTTP_STATUS_OK);
    writer->WriteHeader("Content-Type", handle.mime);
    writer->WriteHeader("Accept-Ranges", "bytes");
    writer->WriteHeader("ETag", etag.c_str());
    writer->WriteHeader("Last-Modified", last_modified.c_str());
    writer->WriteHeader("Content-Disposition", download_disposition(basename_of(path)).c_str());
    if (partial) {
        writer->WriteHeader("Content-Range", "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                                                   std::to_string(handle.size));
    }
    writer->WriteHeader("Content-Length", static_cast<std::uint64_t>(length));
    writer->EndHeaders();

    ctx.audit->record("file.download", "start", std::to_string(length) + " bytes", http::client_ip(*ctx.config, req.get()),
                      path);

    std::vector<char> buffer(kDownloadChunk);
    std::uint64_t offset = start;
    std::uint64_t remaining = length;
    int stalled_ms = 0;
    while (remaining > 0) {
        const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        std::size_t filled = 0;
        while (filled < chunk) {
            const auto read = ::pread(handle.fd.get(), buffer.data() + filled, chunk - filled,
                                      static_cast<off_t>(offset + filled));
            if (read < 0) {
                if (errno == EINTR) continue;
                writer->close();
                return;
            }
            if (read == 0) break; // File shrank underneath us: stop cleanly.
            filled += static_cast<std::size_t>(read);
        }
        if (filled == 0) break;
        if (writer->WriteBody(buffer.data(), static_cast<int>(filled)) < 0) return;
        offset += filled;
        remaining -= filled;
        // Backpressure: never let the socket write queue grow unbounded.
        while (writer->writeBufsize() > kDownloadBufferHighWater) {
            if (!writer->isOpened()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            stalled_ms += 2;
            if (stalled_ms > kDownloadStallGuardMs) {
                writer->close();
                return;
            }
        }
        stalled_ms = 0;
    }
    writer->End();
}

int handle_upload_state(RouteContext& ctx, const HttpContextPtr& http, http_parser_state state, const char* data,
                        std::size_t size) {
    switch (state) {
        case HP_HEADERS_COMPLETE: {
            auto scope = std::make_unique<UploadRequestState>();
            scope->path = http->request->GetParam("path");
            if (scope->path.empty()) {
                scope->failed = true;
                scope->error = Error::make(ErrorCode::invalid_argument, "missing 'path' query parameter");
            } else {
                scope->error = ctx.files->begin_upload(scope->path, scope->upload);
                scope->failed = !scope->error.ok();
            }
            if (scope->failed) {
                ctx.audit->record("file.upload", "denied", scope->error.message,
                                  http::client_ip(*ctx.config, http->request.get()), scope->path);
            }
            http->userdata = scope.release();
            return 0;
        }
        case HP_BODY: {
            auto* scope = static_cast<UploadRequestState*>(http->userdata);
            if (scope == nullptr || scope->failed) return 0;
            if (size == 0) return 0;
            scope->received += size;
            scope->error = scope->upload.append(data, size);
            if (!scope->error.ok()) scope->failed = true;
            return 0;
        }
        case HP_MESSAGE_COMPLETE: {
            std::unique_ptr<UploadRequestState> scope(static_cast<UploadRequestState*>(http->userdata));
            http->userdata = nullptr;
            if (scope == nullptr) {
                return http::write_error(http->response.get(), 400, "invalid_argument", "upload state missing");
            }
            if (scope->failed) {
                ctx.audit->record("file.upload", "denied", scope->error.message,
                                  http::client_ip(*ctx.config, http->request.get()), scope->path);
                return http::write_error(http->response.get(), scope->error);
            }
            FileEntry info;
            Error error = scope->upload.finish(info);
            if (!error.ok()) {
                ctx.audit->record("file.upload", "denied", error.message, http::client_ip(*ctx.config, http->request.get()),
                                  scope->path);
                return http::write_error(http->response.get(), error);
            }
            ctx.audit->record("file.upload", "ok", std::to_string(scope->received) + " bytes",
                              http::client_ip(*ctx.config, http->request.get()), scope->path);
            hv::Json body;
            body["ok"] = true;
            body["size"] = scope->received;
            body["entry"] = http::file_entry_json(info);
            return http::write_json(http->response.get(), 201, body);
        }
        case HP_ERROR: {
            auto* scope = static_cast<UploadRequestState*>(http->userdata);
            http->userdata = nullptr;
            if (scope != nullptr) {
                ctx.audit->record("file.upload", "aborted", "connection closed", http::client_ip(*ctx.config, http->request.get()),
                                  scope->path);
                delete scope;
            }
            return 0;
        }
        default:
            return 0;
    }
}

} // namespace

void register_file_routes(hv::HttpService& router, RouteContext& ctx) {
    router.GET("/api/files", [&ctx](HttpRequest* req, HttpResponse* resp) { return handle_list(ctx, req, resp); });
    router.GET("/api/files/stat",
               [&ctx](HttpRequest* req, HttpResponse* resp) { return handle_stat(ctx, req, resp); });
    router.GET("/api/files/text",
               [&ctx](HttpRequest* req, HttpResponse* resp) { return handle_read_text(ctx, req, resp); });
    router.PUT("/api/files/text",
               [&ctx](HttpRequest* req, HttpResponse* resp) { return handle_write_text(ctx, req, resp); });
    router.POST("/api/files/mkdir",
                [&ctx](HttpRequest* req, HttpResponse* resp) { return handle_mkdir(ctx, req, resp); });
    router.POST("/api/files/rename",
                [&ctx](HttpRequest* req, HttpResponse* resp) { return handle_rename(ctx, req, resp); });
    router.POST("/api/files/delete",
                [&ctx](HttpRequest* req, HttpResponse* resp) { return handle_delete(ctx, req, resp); });
    router.HEAD("/api/files/download",
                [&ctx](HttpRequest* req, HttpResponse* resp) { return handle_download_head(ctx, req, resp); });
    router.GET("/api/files/download", [&ctx](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        handle_download(ctx, req, writer);
    });
    router.PUT("/api/files/content",
               [&ctx](const HttpContextPtr& http, http_parser_state state, const char* data, std::size_t size) {
                   return handle_upload_state(ctx, http, state, data, size);
               });
}

} // namespace edgedesk
