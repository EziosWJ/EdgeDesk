#include "http/http_util.hpp"

#include "core/types.hpp"
#include "core/util.hpp"

#include <ctime>

namespace edgedesk::http {
namespace {

std::string host_of_url(std::string_view url) {
    // Accepts "http://host[:port]/path" and "//host/path".
    const auto scheme = url.find("://");
    std::size_t start = 0;
    if (scheme != std::string_view::npos) {
        start = scheme + 3;
    } else if (url.rfind("//", 0) == 0) {
        start = 2;
    } else {
        return {};
    }
    const auto end = url.find('/', start);
    return std::string(url.substr(start, end == std::string_view::npos ? url.size() - start : end - start));
}

} // namespace

int write_json(HttpResponse* resp, int status, const hv::Json& body) {
    resp->status_code = static_cast<http_status>(status);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->headers["Cache-Control"] = "no-store";
    resp->body = body.dump();
    return status;
}

int write_error(HttpResponse* resp, const Error& error) {
    hv::Json body;
    body["ok"] = false;
    body["error"] = error_code_slug(error.code == ErrorCode::ok ? ErrorCode::io_error : error.code);
    body["message"] = error.message.empty() ? error_code_name(error.code) : error.message;
    if (error.system_errno != 0) body["errno"] = error.system_errno;
    return write_json(resp, http_status_for(error.code == ErrorCode::ok ? ErrorCode::io_error : error.code), body);
}

int write_error(HttpResponse* resp, int status, const std::string& code, const std::string& message) {
    hv::Json body;
    body["ok"] = false;
    body["error"] = code;
    body["message"] = message;
    return write_json(resp, status, body);
}

int write_json_to(const HttpResponseWriterPtr& writer, int status, const hv::Json& body) {
    writer->Begin();
    writer->WriteStatus(static_cast<http_status>(status));
    writer->WriteHeader("Content-Type", "application/json; charset=utf-8");
    writer->WriteHeader("Cache-Control", "no-store");
    writer->EndHeaders();
    const std::string text = body.dump();
    writer->WriteBody(text.c_str(), static_cast<int>(text.size()));
    writer->End();
    return status;
}

int write_error_to(const HttpResponseWriterPtr& writer, const Error& error) {
    hv::Json body;
    body["ok"] = false;
    body["error"] = error_code_slug(error.code == ErrorCode::ok ? ErrorCode::io_error : error.code);
    body["message"] = error.message.empty() ? error_code_name(error.code) : error.message;
    if (error.system_errno != 0) body["errno"] = error.system_errno;
    return write_json_to(writer, http_status_for(error.code == ErrorCode::ok ? ErrorCode::io_error : error.code),
                         body);
}

std::string client_ip(HttpRequest* req) {
    if (req == nullptr) return {};
    const std::string forwarded = req->GetHeader("X-Forwarded-For", "");
    if (!forwarded.empty()) {
        const auto comma = forwarded.find(',');
        return trim(comma == std::string::npos ? forwarded : forwarded.substr(0, comma));
    }
    return req->client_addr.ip;
}

std::optional<std::string> cookie_value(HttpRequest* req, std::string_view name) {
    if (req == nullptr) return std::nullopt;
    // libhv parses the Cookie header into req->cookies; fall back to raw header
    // parsing for requests built by hand (tests).
    const std::string key(name);
    const auto& cookie = req->GetCookie(key);
    if (cookie.name == key && !cookie.value.empty()) {
        return cookie.value;
    }
    const std::string raw = req->GetHeader("Cookie", "");
    if (raw.empty()) return std::nullopt;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        while (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == ';')) ++pos;
        const auto end = raw.find(';', pos);
        const auto item = raw.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        const auto equals = item.find('=');
        if (equals != std::string::npos) {
            const auto candidate = trim(item.substr(0, equals));
            if (candidate == key) {
                return trim(item.substr(equals + 1));
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return std::nullopt;
}

bool origin_matches_host(std::string_view origin, std::string_view host) {
    if (origin.empty() || host.empty()) return false;
    if (origin == "null") return false;
    const auto origin_host = host_of_url(origin);
    if (origin_host.empty()) return false;
    return origin_host == host;
}

bool referer_matches_host(std::string_view referer, std::string_view host) {
    if (referer.empty() || host.empty()) return false;
    const auto referer_host = host_of_url(referer);
    if (referer_host.empty()) return false;
    return referer_host == host;
}

bool parse_json_request(HttpRequest* req, hv::Json& out, Error& error) {
    const std::string content_type = req->GetHeader("Content-Type", "");
    if (content_type.find("application/json") == std::string::npos) {
        error = Error::make(ErrorCode::invalid_argument, "expected application/json body");
        return false;
    }
    if (req->body.empty()) {
        out = hv::Json::object();
        error = Error::none();
        return true;
    }
    try {
        out = hv::Json::parse(req->body);
    } catch (const std::exception& parse_error) {
        error = Error::make(ErrorCode::invalid_argument, std::string("invalid JSON body: ") + parse_error.what());
        return false;
    }
    if (!out.is_object()) {
        error = Error::make(ErrorCode::invalid_argument, "JSON body must be an object");
        return false;
    }
    error = Error::none();
    return true;
}

std::string query_string(HttpRequest* req, const char* key, const std::string& fallback) {
    const auto value = req->GetParam(key);
    return value.empty() ? fallback : value;
}

std::int64_t query_int(HttpRequest* req, const char* key, std::int64_t fallback, std::int64_t min_value,
                       std::int64_t max_value) {
    const auto value = req->GetParam(key);
    if (value.empty()) return fallback;
    std::uint64_t parsed = 0;
    if (!parse_uint64(value, parsed)) return fallback;
    if (static_cast<std::int64_t>(parsed) < min_value) return min_value;
    if (static_cast<std::int64_t>(parsed) > max_value) return max_value;
    return static_cast<std::int64_t>(parsed);
}

bool query_bool(HttpRequest* req, const char* key, bool fallback) {
    const auto value = to_lower_ascii(req->GetParam(key));
    if (value.empty()) return fallback;
    if (value == "1" || value == "true" || value == "yes") return true;
    if (value == "0" || value == "false" || value == "no") return false;
    return fallback;
}

std::string http_date(std::int64_t unix_seconds) {
    std::tm tm{};
    const std::time_t stamp = static_cast<std::time_t>(unix_seconds);
#if defined(_WIN32)
    gmtime_s(&tm, &stamp);
#else
    if (::gmtime_r(&stamp, &tm) == nullptr) return {};
#endif
    char buffer[64];
    const auto written = std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    if (written == 0) return {};
    return std::string(buffer, written);
}

hv::Json file_entry_json(const FileEntry& entry) {
    hv::Json json;
    json["name"] = entry.name;
    json["path"] = entry.path;
    json["type"] = entry.is_dir ? "dir" : (entry.is_symlink ? "symlink" : (entry.is_file ? "file" : "other"));
    json["size"] = entry.size;
    json["mtime"] = entry.mtime;
    json["mode"] = entry.mode;
    json["permissions"] = [&entry] {
        std::string text;
        const std::uint32_t mode = entry.mode;
        for (int group = 2; group >= 0; --group) {
            const std::uint32_t bits = (mode >> (group * 3)) & 0x7;
            text.push_back((bits & 4) != 0 ? 'r' : '-');
            text.push_back((bits & 2) != 0 ? 'w' : '-');
            text.push_back((bits & 1) != 0 ? 'x' : '-');
        }
        return text;
    }();
    json["uid"] = entry.uid;
    json["gid"] = entry.gid;
    json["hidden"] = entry.hidden;
    json["mime"] = entry.mime;
    json["text_editable"] = entry.text_editable;
    if (!entry.link_target.empty()) json["link_target"] = entry.link_target;
    return json;
}

hv::Json file_list_json(const ListResult& result) {
    hv::Json json;
    json["ok"] = true;
    json["path"] = result.path;
    json["page"] = result.page;
    json["page_size"] = result.page_size;
    json["total"] = result.total;
    json["truncated"] = result.truncated;
    hv::Json entries = hv::Json::array();
    for (const auto& entry : result.entries) {
        entries.push_back(file_entry_json(entry));
    }
    json["entries"] = std::move(entries);
    return json;
}

} // namespace edgedesk::http
