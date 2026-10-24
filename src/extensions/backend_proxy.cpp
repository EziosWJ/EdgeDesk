#include "extensions/backend_proxy.hpp"

#include "core/fd.hpp"
#include "extensions/extension_store.hpp"
#include "extensions/native_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

// Host-side HTTP/1.1 client over the application's Unix domain socket. Only
// the host derives the socket path; request data is bounded and parsed before
// being enqueued so a hostile or buggy front end cannot exhaust the pool.

namespace edgedesk::extensions {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxPathBytes = 2048;
constexpr std::size_t kMaxRequestJsonBytes = 1024 * 1024;
constexpr std::size_t kMaxResponseHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxResponseBodyBytes = 1024 * 1024;
constexpr std::size_t kReadChunkBytes = 32 * 1024;
constexpr int kConnectTimeoutMs = 2000;
constexpr std::int64_t kPollSliceMs = 60 * 1000;

Error invalid(std::string message) {
    return Error::make(ErrorCode::invalid_argument, std::move(message));
}

Error protocol_error(std::string detail) {
    return Error::make(ErrorCode::unavailable, "backend_protocol_error: " + std::move(detail));
}

Error too_large_body() {
    return Error::make(ErrorCode::too_large,
                       "backend_protocol_error: response body exceeds 1048576 bytes");
}

Error timeout_error(int timeout_ms) {
    return Error::make(ErrorCode::unavailable,
                       "backend_timeout: backend did not respond within " +
                           std::to_string(timeout_ms) + " ms");
}

Error unavailable(std::string detail, int system_errno = 0) {
    return Error::make(ErrorCode::unavailable, "backend_unavailable: " + std::move(detail),
                       system_errno);
}

Error not_running(std::string detail) {
    return Error::make(ErrorCode::conflict, "backend_not_running: " + std::move(detail));
}

bool valid_method(const std::string& method) {
    return method == "GET" || method == "POST" || method == "PUT" || method == "PATCH" ||
           method == "DELETE";
}

bool valid_app_id(const std::string& app_id) {
    if (app_id.empty() || app_id.size() > 128) return false;
    for (unsigned char c : app_id) {
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') return false;
    }
    return true;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes percent escapes for validation only. Returns false for a malformed
// escape; the original target string is always what gets forwarded.
bool percent_decode(std::string_view input, std::string& out) {
    out.clear();
    out.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] != '%') {
            out.push_back(input[index]);
            continue;
        }
        if (index + 2 >= input.size()) return false;
        const int high = hex_digit(input[index + 1]);
        const int low = hex_digit(input[index + 2]);
        if (high < 0 || low < 0) return false;
        out.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return true;
}

// "/", "/a", "/a/b" or a path with a normal query ("/a?x=1"). Absolute URLs,
// protocol-relative paths, fragments, traversal segments, encoded path
// separators and control bytes are rejected. The target is forwarded verbatim;
// the proxy never normalizes or re-encodes it.
Error validate_path(const std::string& target) {
    if (target.empty()) return invalid("path is required");
    if (target.size() > kMaxPathBytes) return invalid("path exceeds 2048 bytes");
    if (target.front() != '/') return invalid("path must start with '/'");
    if (target.size() > 1 && target[1] == '/') {
        return invalid("path must not be protocol-relative");
    }
    // A fragment is never part of an HTTP request target.
    if (target.find('#') != std::string::npos) {
        return invalid("path must not contain a fragment");
    }
    for (unsigned char c : target) {
        if (c < 0x20 || c == 0x7f) return invalid("path must not contain control characters");
        if (c == '\\') return invalid("path must not contain backslashes");
        if (c == ' ') return invalid("path must not contain spaces");
    }

    const std::size_t query = target.find('?');
    const std::string_view path =
        query == std::string::npos ? std::string_view(target)
                                   : std::string_view(target).substr(0, query);
    const std::string_view query_text =
        query == std::string::npos ? std::string_view()
                                   : std::string_view(target).substr(query + 1);
    if (query_text.find('\0') != std::string_view::npos) {
        return invalid("path must not contain a NUL byte");
    }

    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view raw =
            path.substr(start, slash == std::string::npos ? std::string_view::npos
                                                          : slash - start);
        std::string decoded;
        if (!percent_decode(raw, decoded)) {
            return invalid("path contains a malformed percent escape");
        }
        // Encoded path separators would let a backend re-segment the path
        // after the host validated it, so they are refused outright.
        if (decoded.find('/') != std::string::npos || decoded.find('\\') != std::string::npos) {
            return invalid("path must not contain encoded path separators");
        }
        if (decoded.find('\0') != std::string::npos) {
            return invalid("path must not contain a NUL byte");
        }
        if (decoded == "." || decoded == "..") {
            return invalid("path must not contain '.' or '..' segments");
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return Error::none();
}

struct ValidatedRequest {
    std::string method;
    std::string path;
    std::string body;
    bool has_body = false;
};

Error validate_params(const RuntimeSession& session, const nlohmann::json& params,
                      ValidatedRequest& out) {
    if (!valid_app_id(session.app_id)) {
        return invalid("runtime session has no usable application id");
    }
    if (!params.is_object()) return invalid("params must be a JSON object");
    static const std::set<std::string> kAllowed = {"method", "path", "body", "contentType"};
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (kAllowed.count(it.key()) == 0) {
            // app_id/socket/url/host/port/token are never honored. Rejecting
            // them avoids ambiguity about which value the proxy used.
            return invalid("unsupported parameter '" + it.key() + "'");
        }
    }
    if (!params.contains("method") || !params["method"].is_string()) {
        return invalid("method must be a string");
    }
    out.method = params["method"].get<std::string>();
    if (!valid_method(out.method)) {
        return invalid("method must be one of GET, POST, PUT, PATCH, DELETE");
    }
    if (!params.contains("path") || !params["path"].is_string()) {
        return invalid("path must be a string");
    }
    out.path = params["path"].get<std::string>();
    Error path_error = validate_path(out.path);
    if (!path_error.ok()) return path_error;

    if (params.contains("contentType")) {
        if (!params["contentType"].is_string() ||
            params["contentType"].get<std::string>() != "application/json") {
            return invalid("contentType must be 'application/json'");
        }
    }
    if (params.contains("body")) {
        out.has_body = true;
        try {
            out.body = params["body"].dump();
        } catch (const std::exception& error) {
            return invalid(std::string("body is not serializable JSON: ") + error.what());
        }
        if (out.body.size() > kMaxRequestJsonBytes) {
            return invalid("body exceeds 1048576 bytes");
        }
    }
    return Error::none();
}

std::string lower_ascii(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

std::string_view trim_ows(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool header_name_token(std::string_view name) {
    if (name.empty()) return false;
    for (unsigned char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '!' || c == '#' || c == '$' ||
                        c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
                        c == '-' || c == '.' || c == '^' || c == '_' || c == '`' ||
                        c == '|' || c == '~';
        if (!ok) return false;
    }
    return true;
}

bool header_value_safe(std::string_view value) {
    for (unsigned char c : value) {
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
    }
    return true;
}

bool header_visible(const std::string& lower_name) {
    return lower_name == "content-type" || lower_name.rfind("x-", 0) == 0;
}

bool parse_status_line(std::string_view line, int& status) {
    // HTTP/1.x SP 3DIGIT [SP reason]
    if (line.size() < 12) return false;
    if (line.substr(0, 7) != "HTTP/1.") return false;
    if (line[7] < '0' || line[7] > '9') return false;
    if (line[8] != ' ') return false;
    int value = 0;
    for (std::size_t index = 9; index < 12; ++index) {
        if (line[index] < '0' || line[index] > '9') return false;
        value = value * 10 + (line[index] - '0');
    }
    if (line.size() > 12 && line[12] != ' ') return false;
    if (value < 100 || value > 599) return false;
    status = value;
    return true;
}

struct Head {
    int status = 0;
    std::map<std::string, std::string> headers; // lowercase names
};

enum class ParseState { complete, incomplete, failed };

ParseState parse_head(const std::string& raw, Head& head, std::size_t& body_offset,
                      Error& error) {
    std::size_t offset = 0;
    for (;;) {
        const std::size_t head_end = raw.find("\r\n\r\n", offset);
        if (head_end == std::string::npos) {
            if (raw.size() - offset > kMaxResponseHeaderBytes) {
                error = protocol_error("response headers exceed 65536 bytes");
                return ParseState::failed;
            }
            return ParseState::incomplete;
        }
        if (head_end + 4 - offset > kMaxResponseHeaderBytes) {
            error = protocol_error("response headers exceed 65536 bytes");
            return ParseState::failed;
        }
        const std::size_t status_end = raw.find("\r\n", offset);
        if (status_end == std::string::npos || status_end > head_end) {
            error = protocol_error("malformed status line");
            return ParseState::failed;
        }
        int status = 0;
        if (!parse_status_line(
                std::string_view(raw.data() + offset, status_end - offset), status)) {
            error = protocol_error("malformed status line");
            return ParseState::failed;
        }
        if (status >= 100 && status <= 199) {
            // Interim response: skip it and look for the final head.
            offset = head_end + 4;
            continue;
        }
        Head parsed;
        parsed.status = status;
        std::size_t position = status_end + 2;
        while (position < head_end) {
            const std::size_t line_end = raw.find("\r\n", position);
            if (line_end == std::string::npos || line_end > head_end) {
                error = protocol_error("malformed response header");
                return ParseState::failed;
            }
            const std::string_view line(raw.data() + position, line_end - position);
            if (line.empty() || line.front() == ' ' || line.front() == '\t') {
                error = protocol_error("malformed response header");
                return ParseState::failed;
            }
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) {
                error = protocol_error("malformed response header");
                return ParseState::failed;
            }
            const std::string_view name = line.substr(0, colon);
            if (!header_name_token(name)) {
                error = protocol_error("malformed response header name");
                return ParseState::failed;
            }
            const std::string_view value = trim_ows(line.substr(colon + 1));
            if (!header_value_safe(value)) {
                error = protocol_error("response header contains control bytes");
                return ParseState::failed;
            }
            std::string key = lower_ascii(std::string(name));
            auto existing = parsed.headers.find(key);
            if (existing != parsed.headers.end()) {
                if (key == "content-length" &&
                    existing->second != std::string(value)) {
                    error = protocol_error("conflicting content-length headers");
                    return ParseState::failed;
                }
                existing->second += ", ";
                existing->second.append(value.data(), value.size());
            } else {
                parsed.headers.emplace(std::move(key), std::string(value));
            }
            position = line_end + 2;
        }
        head = std::move(parsed);
        body_offset = head_end + 4;
        return ParseState::complete;
    }
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

ParseState decode_chunked(const std::string& raw, std::size_t offset, std::string& out,
                          Error& error) {
    out.clear();
    std::size_t position = offset;
    for (;;) {
        const std::size_t line_end = raw.find("\r\n", position);
        if (line_end == std::string::npos) {
            if (raw.size() - position > 64) {
                error = protocol_error("malformed chunk size line");
                return ParseState::failed;
            }
            return ParseState::incomplete;
        }
        const std::string_view line(raw.data() + position, line_end - position);
        const std::size_t semicolon = line.find(';');
        const std::string_view size_text = line.substr(0, semicolon);
        if (size_text.empty() || size_text.size() > 16) {
            error = protocol_error("malformed chunk size");
            return ParseState::failed;
        }
        std::size_t size = 0;
        for (char c : size_text) {
            const int digit = hex_value(c);
            if (digit < 0) {
                error = protocol_error("malformed chunk size");
                return ParseState::failed;
            }
            size = size * 16 + static_cast<std::size_t>(digit);
            if (size > kMaxResponseBodyBytes) {
                error = too_large_body();
                return ParseState::failed;
            }
        }
        if (size == 0) {
            // Trailer section: zero or more header lines followed by an empty
            // line. Trailers are bounded and never exposed to the front end.
            std::size_t trailer = line_end + 2;
            for (;;) {
                const std::size_t trailer_end = raw.find("\r\n", trailer);
                if (trailer_end == std::string::npos) {
                    if (raw.size() - trailer > kMaxResponseHeaderBytes) {
                        error = protocol_error("chunked trailers exceed 65536 bytes");
                        return ParseState::failed;
                    }
                    return ParseState::incomplete;
                }
                if (trailer_end == trailer) return ParseState::complete;
                trailer = trailer_end + 2;
                if (trailer - offset > kMaxResponseHeaderBytes) {
                    error = protocol_error("chunked trailers exceed 65536 bytes");
                    return ParseState::failed;
                }
            }
        }
        if (out.size() + size > kMaxResponseBodyBytes) {
            error = too_large_body();
            return ParseState::failed;
        }
        const std::size_t data_start = line_end + 2;
        if (raw.size() < data_start + size + 2) return ParseState::incomplete;
        if (raw.compare(data_start + size, 2, "\r\n") != 0) {
            error = protocol_error("malformed chunk terminator");
            return ParseState::failed;
        }
        out.append(raw, data_start, size);
        position = data_start + size + 2;
    }
}

enum class WaitStatus { ready, timeout, failed };

struct WaitOutcome {
    WaitStatus status = WaitStatus::failed;
    int error = 0;
};

WaitOutcome wait_for_fd(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        const auto now = Clock::now();
        if (now >= deadline) return {WaitStatus::timeout, 0};
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        int slice = static_cast<int>(std::min<std::int64_t>(
            std::max<std::int64_t>(remaining, 1), kPollSliceMs));
        pollfd entry{};
        entry.fd = fd;
        entry.events = events;
        const int ready = ::poll(&entry, 1, slice);
        if (ready > 0) return {WaitStatus::ready, 0};
        if (ready == 0) continue; // slice elapsed; loop re-checks the deadline
        if (errno == EINTR) continue;
        return {WaitStatus::failed, errno};
    }
}

Error connect_error(int system_errno) {
    switch (system_errno) {
        case ENOENT:
        case ECONNREFUSED:
        case ENOTSOCK:
            return not_running("backend socket is not accepting connections");
        case EACCES:
        case EPERM:
            return Error::make(ErrorCode::permission_denied,
                               "permission_denied: cannot connect to backend socket",
                               system_errno);
        case ETIMEDOUT:
            return Error::make(ErrorCode::unavailable, "backend_timeout: connect timed out");
        case ENAMETOOLONG:
            return unavailable("socket path is too long");
        default:
            return unavailable(std::string("cannot connect to backend socket: ") +
                                   std::strerror(system_errno),
                               system_errno);
    }
}

std::string build_request(const std::string& method, const std::string& path,
                          const std::string& body, bool has_body) {
    std::string request;
    request.reserve(256 + body.size());
    request += method;
    request += ' ';
    request += path;
    request += " HTTP/1.1\r\n";
    // The front end can never supply headers: every hop-by-hop header is set
    // here (or omitted) by the host.
    request += "Host: localhost\r\n";
    if (has_body) {
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: ";
        request += std::to_string(body.size());
        request += "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    if (has_body) request += body;
    return request;
}

Error finish_response(const Head& head, std::string body, BackendHttpResponse& out) {
    out.status = head.status;
    out.headers = nlohmann::json::object();
    for (const auto& [name, value] : head.headers) {
        if (header_visible(name)) out.headers[name] = value;
    }
    out.raw_body = std::move(body);
    if (out.raw_body.empty()) {
        out.body = nlohmann::json();
        return Error::none();
    }
    try {
        out.body = nlohmann::json::parse(out.raw_body);
    } catch (const std::exception& error) {
        return protocol_error(std::string("response body is not valid JSON: ") + error.what());
    }
    return Error::none();
}

Error exchange(int fd, const std::string& request, Clock::time_point deadline,
               int timeout_ms, BackendHttpResponse& out) {
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t written =
            ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const WaitOutcome wait = wait_for_fd(fd, POLLOUT, deadline);
            if (wait.status == WaitStatus::timeout) return timeout_error(timeout_ms);
            if (wait.status == WaitStatus::failed) {
                return unavailable("waiting to write request failed", wait.error);
            }
            continue;
        }
        if (written < 0 && (errno == EPIPE || errno == ECONNRESET)) {
            return unavailable("backend closed the connection while reading the request",
                               errno);
        }
        return unavailable("writing request failed", errno);
    }

    std::string raw;
    raw.reserve(8192);
    Head head;
    bool have_head = false;
    std::size_t body_offset = 0;
    Error error;
    for (;;) {
        if (!have_head) {
            const ParseState state = parse_head(raw, head, body_offset, error);
            if (state == ParseState::failed) return error;
            if (state == ParseState::complete) {
                have_head = true;
            } else if (raw.size() > kMaxResponseHeaderBytes) {
                // Bounds the total bytes consumed before the final response
                // head, including any interim 1xx responses.
                return protocol_error("response headers exceed 65536 bytes");
            }
        }
        bool complete = false;
        std::string body;
        if (have_head) {
            const auto transfer = head.headers.find("transfer-encoding");
            const auto length = head.headers.find("content-length");
            const bool chunked =
                transfer != head.headers.end() && lower_ascii(transfer->second) == "chunked";
            if (transfer != head.headers.end() && !chunked) {
                return protocol_error("unsupported transfer-encoding");
            }
            if (chunked) {
                const ParseState state = decode_chunked(raw, body_offset, body, error);
                if (state == ParseState::failed) return error;
                complete = state == ParseState::complete;
            } else if (length != head.headers.end()) {
                std::size_t declared = 0;
                const std::string& text = length->second;
                if (text.empty() || text.size() > 16) {
                    return protocol_error("invalid content-length");
                }
                for (char c : text) {
                    if (c < '0' || c > '9') {
                        return protocol_error("invalid content-length");
                    }
                    declared = declared * 10 + static_cast<std::size_t>(c - '0');
                    if (declared > kMaxResponseBodyBytes) return too_large_body();
                }
                if (raw.size() - body_offset >= declared) {
                    body = raw.substr(body_offset, declared);
                    complete = true;
                }
            } else {
                // Close-delimited: the body ends at EOF.
                if (raw.size() - body_offset > kMaxResponseBodyBytes) {
                    return too_large_body();
                }
            }
            if (complete) return finish_response(head, std::move(body), out);
        }
        if (have_head && raw.size() > kMaxResponseHeaderBytes + kMaxResponseBodyBytes +
                                       kMaxResponseHeaderBytes) {
            return too_large_body();
        }

        const WaitOutcome wait = wait_for_fd(fd, POLLIN, deadline);
        if (wait.status == WaitStatus::timeout) return timeout_error(timeout_ms);
        if (wait.status == WaitStatus::failed) {
            return unavailable("waiting for backend response failed", wait.error);
        }
        char buffer[kReadChunkBytes];
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            raw.append(buffer, static_cast<std::size_t>(received));
            continue;
        }
        if (received == 0) {
            if (!have_head) return protocol_error("empty response");
            const auto transfer = head.headers.find("transfer-encoding");
            const bool chunked =
                transfer != head.headers.end() && lower_ascii(transfer->second) == "chunked";
            if (chunked || head.headers.find("content-length") != head.headers.end()) {
                return protocol_error("truncated response body");
            }
            return finish_response(head, raw.substr(body_offset), out);
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        if (errno == ECONNRESET && have_head &&
            head.headers.find("content-length") == head.headers.end() &&
            head.headers.find("transfer-encoding") == head.headers.end()) {
            // A reset after a close-delimited body is an abnormal but complete
            // end of stream; the bytes received are the body.
            return finish_response(head, raw.substr(body_offset), out);
        }
        return unavailable("reading backend response failed", errno);
    }
}

Error open_socket(const std::string& path, Clock::time_point deadline, int& out_fd) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        return unavailable("socket path exceeds the Unix domain socket limit");
    }
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';

    UniqueFd socket_fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket_fd.valid()) return unavailable("cannot create socket", errno);
    const int flags = ::fcntl(socket_fd.get(), F_GETFL, 0);
    if (flags < 0 || ::fcntl(socket_fd.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
        return unavailable("cannot configure socket", errno);
    }
    const auto connect_deadline =
        std::min(deadline, Clock::now() + std::chrono::milliseconds(kConnectTimeoutMs));
    if (::connect(socket_fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        out_fd = socket_fd.release();
        return Error::none();
    }
    if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        return connect_error(errno);
    }
    const WaitOutcome wait = wait_for_fd(socket_fd.get(), POLLOUT, connect_deadline);
    if (wait.status == WaitStatus::timeout) {
        return Error::make(ErrorCode::unavailable,
                           "backend_timeout: connect timed out after " +
                               std::to_string(kConnectTimeoutMs) + " ms");
    }
    if (wait.status == WaitStatus::failed) {
        return unavailable("waiting for backend connect failed", wait.error);
    }
    int so_error = 0;
    socklen_t length = sizeof(so_error);
    if (::getsockopt(socket_fd.get(), SOL_SOCKET, SO_ERROR, &so_error, &length) < 0) {
        return unavailable("cannot read socket error", errno);
    }
    if (so_error != 0) return connect_error(so_error);
    out_fd = socket_fd.release();
    return Error::none();
}

} // namespace

struct BackendProxy::Impl {
    struct Job {
        std::string app_id;
        std::string token;
        std::string method;
        std::string path;
        std::string body;
        bool has_body = false;
        Callback callback;
    };

    ExtensionStore& store;
    std::string apps_root;
    int timeout_ms = 10000;
    std::size_t queue_limit = 32;
    SessionValidator validator;

    mutable std::mutex mutex;
    std::condition_variable wakeup;
    std::deque<Job> queue;
    std::vector<std::thread> workers;
    std::set<int> active_fds;
    bool stopping = false;

    Impl(ExtensionStore& store_ref, std::string root, std::size_t worker_count,
         std::size_t limit, int timeout)
        : store(store_ref),
          apps_root(std::move(root)),
          timeout_ms(std::max(1, timeout)),
          queue_limit(std::max<std::size_t>(1, limit)) {
        const std::size_t count = std::max<std::size_t>(1, worker_count);
        workers.reserve(count);
        try {
            for (std::size_t index = 0; index < count; ++index) {
                workers.emplace_back([this] { run(); });
            }
        } catch (...) {
            abandon();
            throw;
        }
    }

    ~Impl() { abandon(); }

    void abandon() {
        std::vector<std::thread> joined;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            queue.clear();
            for (int fd : active_fds) ::shutdown(fd, SHUT_RDWR);
            joined.swap(workers);
        }
        wakeup.notify_all();
        for (auto& worker : joined) {
            if (worker.joinable()) worker.join();
        }
    }

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wakeup.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            Error error;
            BackendHttpResponse response;
            try {
                error = perform(job, response);
            } catch (const std::exception& failure) {
                error = unavailable(std::string("internal proxy failure: ") + failure.what());
            } catch (...) {
                error = unavailable("internal proxy failure");
            }
            if (job.callback) job.callback(std::move(error), std::move(response));
        }
    }

    Error perform(const Job& job, BackendHttpResponse& out) {
        SessionValidator validate;
        {
            std::lock_guard<std::mutex> lock(mutex);
            validate = validator;
        }
        // A request can sit in the queue while its application is disabled,
        // upgraded or uninstalled. Revalidate the exact session that submitted
        // it so an invalidated instance never reaches the backend.
        if (validate) {
            RuntimeSession current;
            const Error session_error = validate(job.token, current);
            if (!session_error.ok() || current.app_id != job.app_id) {
                return Error::make(ErrorCode::permission_denied,
                                   "runtime_session_invalid: runtime session is no longer valid");
            }
        }
        AppRecord app;
        bool app_found = false;
        Error app_error = store.get_app(job.app_id, app, app_found);
        if (!app_error.ok()) return app_error;
        if (!app_found || app.status != app_status::enabled) {
            return Error::make(ErrorCode::conflict,
                               "backend_app_disabled: the application is not enabled");
        }

        BackendRecord record;
        bool found = false;
        Error error = store.get_backend(job.app_id, record, found);
        if (!error.ok()) return error;
        if (!found) {
            return Error::make(ErrorCode::not_found,
                               "backend_not_configured: application has no native backend");
        }
        // The socket path is always host-derived from the validated session's
        // application id; no request parameter participates.
        const std::string socket_path = backend_socket_path(apps_root, job.app_id);
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        int fd = -1;
        error = open_socket(socket_path, deadline, fd);
        if (!error.ok()) return error;
        UniqueFd guard(fd);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return unavailable("proxy is shutting down");
            active_fds.insert(fd);
        }
        struct Registration {
            Impl& impl;
            int fd;
            ~Registration() {
                std::lock_guard<std::mutex> lock(impl.mutex);
                impl.active_fds.erase(fd);
            }
        } registration{*this, fd};

        const std::string request = build_request(job.method, job.path, job.body, job.has_body);
        return exchange(fd, request, deadline, timeout_ms, out);
    }

    bool enqueue(Job job) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || queue.size() >= queue_limit) return false;
            queue.push_back(std::move(job));
        }
        wakeup.notify_one();
        return true;
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }

    void set_validator(SessionValidator value) {
        std::lock_guard<std::mutex> lock(mutex);
        validator = std::move(value);
    }
};

BackendProxy::BackendProxy(ExtensionStore& store, std::string apps_root, std::size_t workers,
                           std::size_t queue_limit, int timeout_ms)
    : impl_(std::make_unique<Impl>(store, std::move(apps_root), workers, queue_limit,
                                   timeout_ms)) {}

BackendProxy::~BackendProxy() = default;

void BackendProxy::set_session_validator(SessionValidator validator) {
    impl_->set_validator(std::move(validator));
}

bool BackendProxy::submit(const RuntimeSession& session, nlohmann::json params,
                          Callback callback) {
    ValidatedRequest request;
    const Error validation = validate_params(session, params, request);
    if (!validation.ok()) {
        if (callback) callback(validation, BackendHttpResponse{});
        return true;
    }
    Impl::Job job;
    job.app_id = session.app_id;
    job.token = session.token;
    job.method = std::move(request.method);
    job.path = std::move(request.path);
    job.body = std::move(request.body);
    job.has_body = request.has_body;
    job.callback = std::move(callback);
    return impl_->enqueue(std::move(job));
}

std::size_t BackendProxy::pending() const { return impl_->pending(); }

int BackendProxy::request_timeout_ms() const { return impl_->timeout_ms; }

} // namespace edgedesk::extensions
