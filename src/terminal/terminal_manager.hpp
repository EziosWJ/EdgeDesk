#pragma once

#include "core/error.hpp"
#include "terminal/terminal_session.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Registry and lifecycle owner of PTY sessions (ADR-0005).
// HTTP/WS layers must not touch forkpty/kill/waitpid/ioctl directly.

namespace edgedesk {

struct TerminalManagerOptions {
    int max_sessions = 8;
    int idle_timeout_seconds = 1800;
    std::size_t backlog_bytes = 256 * 1024;
    std::string bash_path = "/bin/bash";
    std::string default_cwd;
};

class TerminalManager {
public:
    struct CreateRequest {
        std::string title;
        std::string cwd; // empty = default; must be accessible when set
        unsigned short cols = 80;
        unsigned short rows = 24;
        std::map<std::string, std::string> extra_env;
    };

    explicit TerminalManager(TerminalManagerOptions options);
    ~TerminalManager();

    TerminalManager(const TerminalManager&) = delete;
    TerminalManager& operator=(const TerminalManager&) = delete;

    Error create(const CreateRequest& request, std::shared_ptr<TerminalSession>& out);

    std::shared_ptr<TerminalSession> find(std::string_view id) const;
    std::vector<TerminalSessionInfo> list() const;

    // Marks the session for termination and removes it from the registry.
    bool request_close(std::string_view id);

    // Periodic maintenance from the HTTP event loop: drops exited sessions and
    // closes detached sessions that exceeded the idle timeout.
    void sweep(std::int64_t now_ms);

    // Terminates every session and joins the reader threads.
    void shutdown();

    std::size_t session_count() const;
    std::size_t running_count() const;

    bool shell_available() const { return shell_available_; }
    const std::string& shell() const { return options_.bash_path; }
    const TerminalManagerOptions& options() const { return options_; }

private:
    TerminalManagerOptions options_;
    bool shell_available_ = false;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<TerminalSession>> sessions_;
    std::uint64_t created_total_ = 0;
};

} // namespace edgedesk
