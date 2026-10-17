#include "http/http_util.hpp"

#include "core/types.hpp"
#include "core/util.hpp"

#include <arpa/inet.h>
#include <cstring>
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

bool ip_in_spec(std::string_view address, std::string_view spec) {
    const auto slash = spec.find('/');
    const std::string network(spec.substr(0, slash));
    unsigned char address_bytes[16] = {};
    unsigned char network_bytes[16] = {};
    const int family = address.find(':') == std::string_view::npos ? AF_INET : AF_INET6;
    if (::inet_pton(family, std::string(address).c_str(), address_bytes) != 1) return false;
    if (::inet_pton(family, network.c_str(), network_bytes) != 1) return false;
    const int max_prefix = family == AF_INET ? 32 : 128;
    int prefix = max_prefix;
    if (slash != std::string_view::npos) {
        std::uint64_t parsed = 0;
        if (!parse_uint64(spec.substr(slash + 1), parsed) || parsed > static_cast<std::uint64_t>(max_prefix)) {
            return false;
        }
        prefix = static_cast<int>(parsed);
    }
    const int full_bytes = prefix / 8;
    const int remaining = prefix % 8;
    if (full_bytes > 0 && std::memcmp(address_bytes, network_bytes, static_cast<std::size_t>(full_bytes)) != 0) {
        return false;
    }
    if (remaining == 0) return true;
    const unsigned char mask = static_cast<unsigned char>(0xFFu << (8 - remaining));
    return (address_bytes[full_bytes] & mask) == (network_bytes[full_bytes] & mask);
}

bool trusted_proxy(const Config& config, std::string_view peer) {
    for (const auto& spec : config.trusted_proxies) {
        if (ip_in_spec(peer, spec)) return true;
    }
    return false;
}

bool valid_ip(std::string_view address) {
    unsigned char bytes[16] = {};
    return ::inet_pton(AF_INET, std::string(address).c_str(), bytes) == 1 ||
           ::inet_pton(AF_INET6, std::string(address).c_str(), bytes) == 1;
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

int write_json_to(const HttpResponseWriterPtr& writer, int status, const hv::Json& body,
                  const std::string& set_cookie) {
    writer->Begin();
    writer->WriteStatus(static_cast<http_status>(status));
    writer->WriteHeader("Content-Type", "application/json; charset=utf-8");
    writer->WriteHeader("Cache-Control", "no-store");
    if (!set_cookie.empty()) writer->WriteHeader("Set-Cookie", set_cookie.c_str());
    const std::string text = body.dump();
    writer->WriteHeader("Content-Length", text.size());
    writer->EndHeaders();
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
    return req->client_addr.ip;
}

std::string client_ip(const Config& config, HttpRequest* req) {
    if (req == nullptr) return {};
    const std::string peer = req->client_addr.ip;
    if (!trusted_proxy(config, peer)) return peer;
    const std::string forwarded = req->GetHeader("X-Forwarded-For", "");
    if (forwarded.empty()) return peer;
    const auto comma = forwarded.find(',');
    const auto candidate = trim(comma == std::string::npos ? forwarded : forwarded.substr(0, comma));
    return valid_ip(candidate) ? candidate : peer;
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
