#pragma once

#include "core/error.hpp"
#include "supervisor/supervisor_engine.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

// JSON-RPC 2.0 over a Linux AF_UNIX stream socket, NDJSON framed.
// There is deliberately no TCP listener and no generic command execution:
// every method operates on registered service IDs only.

namespace edgedesk::supervisor {

struct IpcServerOptions {
    std::string socket_path;
    std::optional<gid_t> socket_gid;
    mode_t socket_mode = 0660;
    std::vector<uid_t> allowed_uids;
    std::vector<gid_t> allowed_gids;
    std::size_t max_connections = 64;
    std::size_t max_message_bytes = 1024 * 1024;
    std::size_t max_output_bytes = 1024 * 1024;
};

class IpcServer {
public:
    static std::unique_ptr<IpcServer> create(const IpcServerOptions& options,
                                             SupervisorEngine& engine, Error& error);

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    ~IpcServer();

    // Runs until should_stop() returns true. Never blocks beyond the poll
    // deadline, so SIGTERM handling stays responsive.
    Error run(const std::function<bool()>& should_stop);

    void request_stop() { stop_ = true; }
    const std::string& socket_path() const { return options_.socket_path; }

private:
    struct Client;
    struct Pending;

    IpcServer(IpcServerOptions options, SupervisorEngine& engine);
    Error open_socket();
    void accept_clients();
    void read_client(Client& client, std::int64_t now_ms);
    void write_client(Client& client);
    void close_client(int fd);
    void handle_line(Client& client, std::string line, std::int64_t now_ms);
    void dispatch(Client& client, nlohmann::json request, std::int64_t now_ms);
    void enqueue(Client& client, const nlohmann::json& message);
    void send_error(Client& client, nlohmann::json id, int code, std::string message,
                    std::string slug = {});
    void complete_requests(std::int64_t now_ms);
    void expire_requests(std::int64_t now_ms);
    bool peer_allowed(uid_t uid, gid_t gid) const;
    int poll_timeout_ms(std::int64_t now_ms) const;

    IpcServerOptions options_;
    SupervisorEngine& engine_;
    int listen_fd_ = -1;
    std::map<int, std::shared_ptr<Client>> clients_;
    std::map<std::uint64_t, Pending> pending_;
    std::uint64_t next_ticket_ = 1;
    bool stop_ = false;
    bool bound_ = false;
};

} // namespace edgedesk::supervisor
