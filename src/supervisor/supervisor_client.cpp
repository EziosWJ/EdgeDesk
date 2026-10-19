#include "supervisor/supervisor_client.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace edgedesk::supervisor {
namespace {

std::int64_t monotonic_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int remaining_ms(std::int64_t deadline_ms) {
    const std::int64_t now = monotonic_ms();
    if (now >= deadline_ms) return 0;
    const std::int64_t remaining = deadline_ms - now;
    return remaining > 1000 ? 1000 : static_cast<int>(remaining);
}

Error wait_for(int fd, short events, std::int64_t deadline_ms, const char* stage) {
    for (;;) {
        const int timeout = remaining_ms(deadline_ms);
        if (timeout <= 0) {
            return Error::make(ErrorCode::unavailable,
                               std::string("supervisor IPC ") + stage + " timed out");
        }
        struct pollfd item {};
        item.fd = fd;
        item.events = events;
        const int ready = ::poll(&item, 1, timeout);
        if (ready > 0) return Error::none();
        if (ready == 0) {
            return Error::make(ErrorCode::unavailable,
                               std::string("supervisor IPC ") + stage + " timed out");
        }
        if (errno != EINTR) {
            return Error::make(ErrorCode::io_error,
                               std::string("supervisor IPC ") + stage + " failed: " +
                                   std::strerror(errno), errno);
        }
    }
}

int connect_socket(const std::string& path, int timeout_ms, Error& error) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = Error::make(ErrorCode::unavailable,
                            std::string("cannot create supervisor IPC socket: ") +
                                std::strerror(errno), errno);
        return -1;
    }
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const std::int64_t deadline = monotonic_ms() + timeout_ms;
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) {
            error = Error::make(ErrorCode::unavailable,
                                "supervisor is unavailable: " + std::string(std::strerror(errno)),
                                errno);
            ::close(fd);
            return -1;
        }
        error = wait_for(fd, POLLOUT, deadline, "connect");
        if (!error.ok()) {
            ::close(fd);
            return -1;
        }
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 ||
            socket_error != 0) {
            const int saved = socket_error != 0 ? socket_error : errno;
            error = Error::make(ErrorCode::unavailable,
                                "supervisor is unavailable: " + std::string(std::strerror(saved)),
                                saved);
            ::close(fd);
            return -1;
        }
    }
    error = Error::none();
    return fd;
}

Error send_line(int fd, const std::string& data, std::int64_t deadline_ms) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        Error error = wait_for(fd, POLLOUT, deadline_ms, "send");
        if (!error.ok()) return error;
        const ssize_t count =
            ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else {
            return Error::make(ErrorCode::io_error,
                               std::string("cannot write supervisor IPC request: ") +
                                   std::strerror(errno), errno);
        }
    }
    return Error::none();
}

Error read_line(int fd, std::size_t max_bytes, std::int64_t deadline_ms, std::string& line) {
    std::string buffer;
    for (;;) {
        const auto newline = buffer.find('\n');
        if (newline != std::string::npos) {
            line = buffer.substr(0, newline);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return Error::none();
        }
        if (buffer.size() > max_bytes) {
            return Error::make(ErrorCode::io_error,
                               "supervisor IPC response exceeds the configured limit");
        }
        Error error = wait_for(fd, POLLIN, deadline_ms, "read");
        if (!error.ok()) return error;
        char chunk[8192];
        const ssize_t count = ::read(fd, chunk, sizeof(chunk));
        if (count > 0) {
            buffer.append(chunk, static_cast<std::size_t>(count));
        } else if (count == 0) {
            return Error::make(ErrorCode::io_error,
                               "supervisor closed the IPC connection before responding");
        } else if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        } else {
            return Error::make(ErrorCode::io_error,
                               std::string("cannot read supervisor IPC response: ") +
                                   std::strerror(errno), errno);
        }
    }
}

} // namespace

std::unique_ptr<SupervisorClient> SupervisorClient::create(
    const SupervisorClientOptions& options, Error& error) {
    if (options.socket_path.empty() || options.socket_path.front() != '/') {
        error = Error::make(ErrorCode::invalid_argument,
                            "supervisor socket path must be absolute");
        return nullptr;
    }
    if (options.socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        error = Error::make(ErrorCode::invalid_argument,
                            "supervisor socket path is too long");
        return nullptr;
    }
    if (options.connect_timeout_ms < 100 || options.connect_timeout_ms > 30000 ||
        options.request_timeout_ms < 1000 || options.request_timeout_ms > 15 * 60 * 1000) {
        error = Error::make(ErrorCode::invalid_argument, "invalid supervisor IPC timeouts");
        return nullptr;
    }
    if (options.max_message_bytes < 4096 ||
        options.max_message_bytes > 16 * 1024 * 1024) {
        error = Error::make(ErrorCode::invalid_argument,
                            "invalid supervisor IPC message limit");
        return nullptr;
    }
    error = Error::none();
    return std::unique_ptr<SupervisorClient>(new SupervisorClient(options));
}

SupervisorClient::SupervisorClient(SupervisorClientOptions options)
    : options_(std::move(options)) {}

SupervisorClient::~SupervisorClient() = default;

Error SupervisorClient::call(std::string_view method, const nlohmann::json& params,
                             nlohmann::json& result, SupervisorRpcError& rpc_error) const {
    result = nlohmann::json();
    rpc_error = {};
    if (method.empty() || method.size() > 128) {
        return Error::make(ErrorCode::invalid_argument, "invalid supervisor method name");
    }
    if (!params.is_object() && !params.is_null()) {
        return Error::make(ErrorCode::invalid_argument,
                           "supervisor params must be a JSON object");
    }

    Error error;
    const int fd = connect_socket(options_.socket_path, options_.connect_timeout_ms, error);
    if (fd < 0) return error;

    const std::uint64_t id = next_id_.fetch_add(1);
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = id;
    request["method"] = method;
    request["params"] = params.is_null() ? nlohmann::json::object() : params;
    std::string line =
        request.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    line.push_back('\n');
    if (line.size() > options_.max_message_bytes) {
        ::close(fd);
        return Error::make(ErrorCode::invalid_argument,
                           "supervisor IPC request exceeds the configured limit");
    }

    const std::int64_t deadline = monotonic_ms() + options_.request_timeout_ms;
    error = send_line(fd, line, deadline);
    if (!error.ok()) {
        ::close(fd);
        return error;
    }
    std::string response_line;
    error = read_line(fd, options_.max_message_bytes, deadline, response_line);
    ::close(fd);
    if (!error.ok()) return error;

    nlohmann::json response;
    try {
        response = nlohmann::json::parse(response_line);
    } catch (const std::exception&) {
        return Error::make(ErrorCode::io_error,
                           "supervisor returned an invalid JSON-RPC response");
    }
    if (!response.is_object() || !response.contains("id") ||
        response["id"] != nlohmann::json(id)) {
        return Error::make(ErrorCode::io_error,
                           "supervisor returned a mismatched JSON-RPC response");
    }
    if (response.contains("error") && response["error"].is_object()) {
        const auto& body = response["error"];
        rpc_error.code = body.value("code", 0);
        rpc_error.message = body.value("message", std::string("supervisor request failed"));
        if (body.contains("data") && body["data"].is_object()) {
            rpc_error.slug = body["data"].value("error", std::string());
        }
        return Error::none();
    }
    if (!response.contains("result")) {
        return Error::make(ErrorCode::io_error,
                           "supervisor response carries neither result nor error");
    }
    result = std::move(response["result"]);
    return Error::none();
}

} // namespace edgedesk::supervisor
