#include "supervisor/ipc_server.hpp"

#include "core/util.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace edgedesk::supervisor {
namespace {

constexpr std::size_t kReadChunk = 16 * 1024;
constexpr std::size_t kMaxLineWithoutNewline = 1024 * 1024;

int app_error_code(ErrorCode code) {
    switch (code) {
        case ErrorCode::invalid_argument: return -32602;
        case ErrorCode::not_found: return -32002;
        case ErrorCode::conflict: return -32003;
        case ErrorCode::permission_denied: return -32004;
        case ErrorCode::unsupported: return -32601;
        case ErrorCode::ok: return 0;
        default: return -32005;
    }
}

nlohmann::json response_error(nlohmann::json id, int code, const std::string& message,
                              const std::string& slug) {
    nlohmann::json body;
    body["jsonrpc"] = "2.0";
    body["id"] = std::move(id);
    nlohmann::json error;
    error["code"] = code;
    error["message"] = message;
    if (!slug.empty()) error["data"]["error"] = slug;
    body["error"] = std::move(error);
    return body;
}

} // namespace

struct IpcServer::Client {
    int fd = -1;
    uid_t uid = 0;
    gid_t gid = 0;
    std::string input;
    std::string output;
    bool closing = false;
    bool closed = false;
};

struct IpcServer::Pending {
    std::shared_ptr<Client> client;
    nlohmann::json id;
    std::int64_t deadline_ms = 0;
};

IpcServer::IpcServer(IpcServerOptions options, SupervisorEngine& engine)
    : options_(std::move(options)), engine_(engine) {}

std::unique_ptr<IpcServer> IpcServer::create(const IpcServerOptions& options,
                                             SupervisorEngine& engine, Error& error) {
    if (options.socket_path.empty() || options.socket_path.front() != '/') {
        error = Error::make(ErrorCode::invalid_argument, "IPC socket path must be absolute");
        return nullptr;
    }
    if (options.socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        error = Error::make(ErrorCode::invalid_argument, "IPC socket path is too long");
        return nullptr;
    }
    if ((options.socket_mode & 0007) != 0) {
        error = Error::make(ErrorCode::invalid_argument,
                            "IPC socket must not be accessible to other users");
        return nullptr;
    }
    if (options.allowed_uids.empty() && options.allowed_gids.empty()) {
        error = Error::make(ErrorCode::invalid_argument,
                            "at least one allowed IPC UID or GID must be configured");
        return nullptr;
    }
    if (options.max_connections == 0 || options.max_connections > 4096 ||
        options.max_message_bytes == 0 || options.max_message_bytes > 16 * 1024 * 1024 ||
        options.max_output_bytes == 0 || options.max_output_bytes > 16 * 1024 * 1024) {
        error = Error::make(ErrorCode::invalid_argument, "invalid IPC limits");
        return nullptr;
    }
    std::unique_ptr<IpcServer> server(new IpcServer(options, engine));
    error = server->open_socket();
    if (!error.ok()) return nullptr;
    return server;
}

IpcServer::~IpcServer() {
    for (auto& item : clients_) {
        if (item.second->fd >= 0) ::close(item.second->fd);
    }
    clients_.clear();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (bound_) {
        (void)::unlink(options_.socket_path.c_str());
    }
}

Error IpcServer::open_socket() {
    const auto slash = options_.socket_path.rfind('/');
    const std::string directory = slash == 0 ? "/" : options_.socket_path.substr(0, slash);
    struct stat dir_info {};
    if (::lstat(directory.c_str(), &dir_info) != 0) {
        return Error::make(ErrorCode::io_error,
                           "cannot inspect socket directory " + directory + ": " +
                               std::strerror(errno), errno);
    }
    if (!S_ISDIR(dir_info.st_mode)) {
        return Error::make(ErrorCode::invalid_argument,
                           "socket directory is not a directory: " + directory);
    }
    if (S_ISLNK(dir_info.st_mode)) {
        return Error::make(ErrorCode::permission_denied,
                           "refusing symlinked socket directory: " + directory);
    }
    if (dir_info.st_uid != ::geteuid()) {
        return Error::make(ErrorCode::permission_denied,
                           "socket directory is not owned by the supervisor user: " + directory);
    }
    if ((dir_info.st_mode & 0022) != 0) {
        return Error::make(ErrorCode::permission_denied,
                           "socket directory must not be writable by group or other users: " +
                               directory);
    }

    struct stat existing {};
    if (::lstat(options_.socket_path.c_str(), &existing) == 0) {
        if (S_ISLNK(existing.st_mode)) {
            return Error::make(ErrorCode::permission_denied,
                               "refusing symlinked supervisor socket path");
        }
        if (!S_ISSOCK(existing.st_mode)) {
            return Error::make(ErrorCode::permission_denied,
                               "supervisor socket path exists and is not a socket");
        }
        if (existing.st_uid != ::geteuid()) {
            return Error::make(ErrorCode::permission_denied,
                               "supervisor socket is owned by another user");
        }
        if (::unlink(options_.socket_path.c_str()) != 0 && errno != ENOENT) {
            return Error::make(ErrorCode::io_error,
                               "cannot remove stale supervisor socket: " +
                                   std::string(std::strerror(errno)), errno);
        }
    } else if (errno != ENOENT) {
        return Error::make(ErrorCode::io_error,
                           "cannot inspect supervisor socket path: " +
                               std::string(std::strerror(errno)), errno);
    }

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        return Error::make(ErrorCode::io_error,
                           std::string("cannot create Unix socket: ") + std::strerror(errno),
                           errno);
    }
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, options_.socket_path.c_str(),
                options_.socket_path.size() + 1);
    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        const int saved = errno;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return Error::make(ErrorCode::io_error,
                           "cannot bind supervisor socket: " + std::string(std::strerror(saved)),
                           saved);
    }
    bound_ = true;
    if (::chmod(options_.socket_path.c_str(), options_.socket_mode) != 0) {
        return Error::make(ErrorCode::permission_denied,
                           "cannot set supervisor socket permissions: " +
                               std::string(std::strerror(errno)), errno);
    }
    if (options_.socket_gid.has_value()) {
        if (::chown(options_.socket_path.c_str(), static_cast<uid_t>(-1),
                    *options_.socket_gid) != 0) {
            return Error::make(ErrorCode::permission_denied,
                               "cannot set supervisor socket group: " +
                                   std::string(std::strerror(errno)), errno);
        }
    }
    if (::listen(listen_fd_, 32) != 0) {
        return Error::make(ErrorCode::io_error,
                           std::string("cannot listen on supervisor socket: ") +
                               std::strerror(errno), errno);
    }
    return Error::none();
}

bool IpcServer::peer_allowed(uid_t uid, gid_t gid) const {
    if (std::find(options_.allowed_uids.begin(), options_.allowed_uids.end(), uid) !=
        options_.allowed_uids.end()) {
        return true;
    }
    return std::find(options_.allowed_gids.begin(), options_.allowed_gids.end(), gid) !=
           options_.allowed_gids.end();
}

void IpcServer::accept_clients() {
    for (;;) {
        const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;
        }
        struct ucred credentials {};
        socklen_t length = sizeof(credentials);
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 ||
            length != sizeof(credentials)) {
            ::close(fd);
            continue;
        }
        if (!peer_allowed(credentials.uid, credentials.gid)) {
            std::fprintf(stderr,
                         "edgedesk-supervisor: rejected IPC peer uid=%u gid=%u\n",
                         static_cast<unsigned>(credentials.uid),
                         static_cast<unsigned>(credentials.gid));
            ::close(fd);
            continue;
        }
        if (clients_.size() >= options_.max_connections) {
            ::close(fd);
            continue;
        }
        auto client = std::make_shared<Client>();
        client->fd = fd;
        client->uid = credentials.uid;
        client->gid = credentials.gid;
        clients_.emplace(fd, std::move(client));
    }
}

void IpcServer::close_client(int fd) {
    const auto found = clients_.find(fd);
    if (found == clients_.end()) return;
    std::shared_ptr<Client> client = found->second;
    clients_.erase(found);
    client->closed = true;
    if (client->fd >= 0) {
        ::close(client->fd);
        client->fd = -1;
    }
    std::vector<std::uint64_t> remove;
    for (const auto& item : pending_) {
        if (item.second.client == client) remove.push_back(item.first);
    }
    for (const auto ticket : remove) pending_.erase(ticket);
}

void IpcServer::enqueue(Client& client, const nlohmann::json& message) {
    if (client.closed || client.closing) return;
    std::string line;
    try {
        line = message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "edgedesk-supervisor: cannot serialize IPC response: %s\n",
                     error.what());
        client.closing = true;
        client.output.clear();
        return;
    }
    line.push_back('\n');
    if (client.output.size() + line.size() > options_.max_output_bytes) {
        // A client that does not read responses must not grow the daemon.
        client.closing = true;
        client.output.clear();
        return;
    }
    client.output += line;
}

void IpcServer::send_error(Client& client, nlohmann::json id, int code, std::string message,
                           std::string slug) {
    enqueue(client, response_error(std::move(id), code, message, slug));
}

void IpcServer::dispatch(Client& client, nlohmann::json request, std::int64_t now_ms) {
    if (!request.is_object()) {
        send_error(client, nullptr, -32600, "invalid request");
        return;
    }
    if (!request.contains("jsonrpc") || request["jsonrpc"] != "2.0") {
        send_error(client, nullptr, -32600, "jsonrpc must be '2.0'");
        return;
    }
    if (!request.contains("id") || request["id"].is_null() ||
        (!request["id"].is_string() && !request["id"].is_number())) {
        send_error(client, nullptr, -32600, "every request must carry a string or numeric id");
        return;
    }
    nlohmann::json id = request["id"];
    if (!request.contains("method") || !request["method"].is_string()) {
        send_error(client, std::move(id), -32600, "method must be a string");
        return;
    }
    const std::string method = request["method"].get<std::string>();
    nlohmann::json params = nlohmann::json::object();
    if (request.contains("params")) {
        if (!request["params"].is_object() && !request["params"].is_null()) {
            send_error(client, std::move(id), -32602, "params must be an object or null");
            return;
        }
        params = request["params"];
    }

    const std::uint64_t ticket = next_ticket_++;
    RpcResult result;
    try {
        result = engine_.rpc(method, params, ticket);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "edgedesk-supervisor: IPC method %s failed: %s\n", method.c_str(),
                     error.what());
        send_error(client, std::move(id), -32603, "internal supervisor error", "internal_error");
        return;
    }
    if (result.async) {
        Pending pending;
        const auto owner = clients_.find(client.fd);
        if (owner == clients_.end()) return;
        pending.client = owner->second;
        pending.id = std::move(id);
        const std::int64_t timeout =
            result.async_timeout_ms > 0 ? result.async_timeout_ms : 60000;
        pending.deadline_ms = now_ms + timeout;
        pending_.emplace(ticket, std::move(pending));
        return;
    }
    if (result.ok) {
        nlohmann::json response;
        response["jsonrpc"] = "2.0";
        response["id"] = std::move(id);
        response["result"] = std::move(result.result);
        enqueue(client, response);
    } else {
        send_error(client, std::move(id), app_error_code(result.error.code), result.error.message,
                   result.slug);
    }
}

void IpcServer::handle_line(Client& client, std::string line, std::int64_t now_ms) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) return;
    if (line.size() > options_.max_message_bytes) {
        send_error(client, nullptr, -32600, "message exceeds the IPC size limit");
        client.closing = true;
        return;
    }
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(line);
    } catch (const std::exception&) {
        send_error(client, nullptr, -32700, "parse error");
        return;
    }
    dispatch(client, std::move(request), now_ms);
}

void IpcServer::read_client(Client& client, std::int64_t now_ms) {
    char buffer[kReadChunk];
    for (;;) {
        if (client.closing || client.closed) return;
        const ssize_t count = ::read(client.fd, buffer, sizeof(buffer));
        if (count > 0) {
            client.input.append(buffer, static_cast<std::size_t>(count));
            std::size_t start = 0;
            for (;;) {
                const auto newline = client.input.find('\n', start);
                if (newline == std::string::npos) break;
                std::string line = client.input.substr(start, newline - start);
                start = newline + 1;
                handle_line(client, std::move(line), now_ms);
                if (client.closing) break;
            }
            if (start > 0) client.input.erase(0, start);
            if (client.closing) return;
            if (client.input.size() > kMaxLineWithoutNewline) {
                send_error(client, nullptr, -32600, "message exceeds the IPC size limit");
                client.closing = true;
                client.input.clear();
                return;
            }
            continue;
        }
        if (count == 0) {
            close_client(client.fd);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        close_client(client.fd);
        return;
    }
}

void IpcServer::write_client(Client& client) {
    while (!client.output.empty()) {
        const ssize_t count =
            ::send(client.fd, client.output.data(), client.output.size(), MSG_NOSIGNAL);
        if (count > 0) {
            client.output.erase(0, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        close_client(client.fd);
        return;
    }
    if (client.closing) close_client(client.fd);
}

void IpcServer::complete_requests(std::int64_t now_ms) {
    std::vector<RpcCompletion> completions = engine_.poll(now_ms);
    for (auto& completion : completions) {
        const auto found = pending_.find(completion.ticket);
        if (found == pending_.end()) continue;
        Pending pending = std::move(found->second);
        pending_.erase(found);
        if (pending.client->closed) continue;
        if (completion.ok) {
            nlohmann::json response;
            response["jsonrpc"] = "2.0";
            response["id"] = std::move(pending.id);
            response["result"] = std::move(completion.result);
            enqueue(*pending.client, response);
        } else {
            send_error(*pending.client, std::move(pending.id),
                       app_error_code(completion.error.code), completion.error.message,
                       completion.slug);
        }
    }
}

void IpcServer::expire_requests(std::int64_t now_ms) {
    std::vector<std::uint64_t> expired;
    for (const auto& item : pending_) {
        if (now_ms >= item.second.deadline_ms) expired.push_back(item.first);
    }
    for (const auto ticket : expired) {
        auto found = pending_.find(ticket);
        if (found == pending_.end()) continue;
        Pending pending = std::move(found->second);
        pending_.erase(found);
        if (!pending.client->closed) {
            send_error(*pending.client, std::move(pending.id), -32005,
                       "supervisor operation timed out", "operation_timeout");
        }
    }
}

int IpcServer::poll_timeout_ms(std::int64_t now_ms) const {
    std::int64_t next = now_ms + engine_.poll_timeout_ms(now_ms);
    next = std::min<std::int64_t>(next, now_ms + 500);
    for (const auto& item : pending_) {
        next = std::min(next, item.second.deadline_ms);
    }
    if (next <= now_ms) return 0;
    return static_cast<int>(std::min<std::int64_t>(next - now_ms, 500));
}

Error IpcServer::run(const std::function<bool()>& should_stop) {
    while (!stop_ && !should_stop()) {
        const std::int64_t now = steady_millis();
        complete_requests(now);
        expire_requests(now);
        if (stop_ || should_stop()) break;

        std::vector<struct pollfd> fds;
        fds.reserve(clients_.size() + 1);
        fds.push_back({listen_fd_, POLLIN, 0});
        for (auto& item : clients_) {
            short events = POLLIN;
            if (!item.second->output.empty()) events |= POLLOUT;
            fds.push_back({item.first, events, 0});
        }
        const int timeout = poll_timeout_ms(now);
        const int ready = ::poll(fds.data(), fds.size(), timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return Error::make(ErrorCode::io_error,
                               std::string("supervisor IPC poll failed: ") +
                                   std::strerror(errno), errno);
        }
        if (ready == 0) continue;

        if ((fds[0].revents & POLLIN) != 0) {
            accept_clients();
        } else if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return Error::make(ErrorCode::io_error, "supervisor IPC listener failed");
        }

        for (std::size_t index = 1; index < fds.size(); ++index) {
            const int fd = fds[index].fd;
            const short revents = fds[index].revents;
            if (revents == 0) continue;
            const auto found = clients_.find(fd);
            if (found == clients_.end()) continue;
            std::shared_ptr<Client> client = found->second;
            if ((revents & (POLLERR | POLLNVAL)) != 0) {
                close_client(fd);
                continue;
            }
            if ((revents & POLLIN) != 0) {
                read_client(*client, steady_millis());
            }
            if (client->closed) continue;
            if ((revents & POLLOUT) != 0 || !client->output.empty()) {
                write_client(*client);
            } else if ((revents & POLLHUP) != 0) {
                close_client(fd);
            }
        }
    }
    return Error::none();
}

} // namespace edgedesk::supervisor
