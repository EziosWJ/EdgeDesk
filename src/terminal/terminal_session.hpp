#pragma once

#include "core/error.hpp"
#include "terminal/pty.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// One PTY-backed Bash session (ADR-0005).
// Lifetime is owned by TerminalManager; WebSocket connections only subscribe.

namespace edgedesk {

struct TerminalSessionInfo {
    std::string id;
    std::string title;
    std::string cwd;
    std::int64_t created_ms = 0;
    std::int64_t last_activity_ms = 0;
    unsigned short cols = 80;
    unsigned short rows = 24;
    bool running = false;
    bool exited = false;
    int exit_code = 0;
    std::size_t subscribers = 0;
    std::size_t buffered_bytes = 0;
    bool pending_confirmation = false;
};

class TerminalSession {
public:
    using OutputCallback = std::function<void(const char* data, std::size_t length)>;

    struct Subscriber {
        OutputCallback on_output;
        std::function<void(int exit_code)> on_exit;
    };

    struct CreateArgs {
        std::string id;
        std::string title;
        std::string cwd;
        PtySpawnOptions spawn;
        std::size_t backlog_bytes = 256 * 1024;
        std::function<void(int exit_code)> on_exit_notify; // manager bookkeeping
    };

    static std::shared_ptr<TerminalSession> create(CreateArgs args, Error& error);
    ~TerminalSession();

    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    const std::string& id() const { return id_; }
    const std::string& title() const { return title_; }
    TerminalSessionInfo info() const;

    enum class InputOutcome { sent, guard_blocked, rejected };

    // Sends input to the PTY. Tracks the current shell line and blocks the
    // final newline of obviously dangerous commands until confirmation.
    InputOutcome submit_input(std::string_view data, std::string& guard_reason);
    bool confirm_pending();
    bool cancel_pending();
    Error send_raw(std::string_view data);

    Error resize(unsigned short cols, unsigned short rows);
    void request_close();
    bool running() const;
    bool finalized() const;

    // Waits for the reader thread (never for a thread that is joining itself).
    void join();

    // Delivers the replay buffer and registers for live output atomically.
    // Returns 0 when the subscriber cap is reached.
    std::uint64_t subscribe(Subscriber subscriber);
    void unsubscribe(std::uint64_t token);

    std::string replay_buffer() const;
    std::size_t buffered_bytes() const;

private:
    explicit TerminalSession(CreateArgs args);
    void read_loop();
    void finalize(int exit_code);
    void append_backlog_locked(const char* data, std::size_t length);

    // Callbacks run on the reader/WebSocket threads and may re-enter this
    // session, so they are never invoked while mutex_ is held. delivery_mutex
    // is held across backlog replay so the reader thread cannot deliver live
    // output before the replay or the exit callback.
    struct SubscriberState {
        Subscriber callbacks;
        std::mutex delivery_mutex;
    };

    static constexpr std::size_t kMaxSubscribers = 8;
    static constexpr std::size_t kMaxTrackedLine = 8192;

    const std::string id_;
    std::string title_;
    std::string cwd_;
    const std::size_t backlog_capacity_;
    std::unique_ptr<PtyProcess> pty_;
    std::function<void(int)> on_exit_notify_;

    mutable std::mutex mutex_;
    std::string backlog_;
    std::size_t dropped_bytes_ = 0;
    bool dropped_notice_ = false;
    std::map<std::uint64_t, std::shared_ptr<SubscriberState>> subscribers_;
    std::uint64_t next_token_ = 1;

    // Guard tracking (all guarded by mutex_)
    std::string line_buffer_;
    std::string pending_line_;
    std::string pending_reason_;
    bool pending_confirmation_ = false;

    std::atomic<bool> close_requested_{false};
    bool running_ = true;
    bool finalized_state_ = false;
    int exit_code_ = 0;
    unsigned short cols_ = 80;
    unsigned short rows_ = 24;
    std::int64_t created_ms_ = 0;
    std::int64_t last_activity_ms_ = 0;
    std::thread reader_;
};

} // namespace edgedesk
