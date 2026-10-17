#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "file/file_types.hpp"

// libhv 1.3.4 headers use stat()/fcntl() without guaranteeing the POSIX
// headers are included (they rely on libstdc++ transitive includes on some
// toolchains). Include them first so builds are not toolchain-dependent.
#include <fcntl.h>
#include <sys/stat.h>

#include "HttpMessage.h"
#include "HttpResponseWriter.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Small protocol adapters shared by every route. Keeping them here avoids
// duplicating response/JSON handling and keeps libhv types at the edge.

namespace edgedesk::http {

int write_json(HttpResponse* resp, int status, const hv::Json& body);
int write_error(HttpResponse* resp, const Error& error);
int write_error(HttpResponse* resp, int status, const std::string& code, const std::string& message);

// Streaming (async handler) variants.
int write_json_to(const HttpResponseWriterPtr& writer, int status, const hv::Json& body,
                  const std::string& set_cookie = {});
int write_error_to(const HttpResponseWriterPtr& writer, const Error& error);

// Peer address only. Use the Config overload in HTTP/WS handlers when a
// trusted reverse proxy may provide X-Forwarded-For.
std::string client_ip(HttpRequest* req);
std::string client_ip(const Config& config, HttpRequest* req);
std::optional<std::string> cookie_value(HttpRequest* req, std::string_view name);

// Origin/Referer checks for CSRF and WebSocket handshakes.
bool origin_matches_host(std::string_view origin, std::string_view host);
bool referer_matches_host(std::string_view referer, std::string_view host);

bool parse_json_request(HttpRequest* req, hv::Json& out, Error& error);

std::string query_string(HttpRequest* req, const char* key, const std::string& fallback = {});
std::int64_t query_int(HttpRequest* req, const char* key, std::int64_t fallback, std::int64_t min_value,
                       std::int64_t max_value);
bool query_bool(HttpRequest* req, const char* key, bool fallback);

std::string http_date(std::int64_t unix_seconds);

hv::Json file_entry_json(const FileEntry& entry);
hv::Json file_list_json(const ListResult& result);

} // namespace edgedesk::http
