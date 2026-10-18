#include "terminal/terminal_session.hpp"

#include "core/util.hpp"
#include "terminal/command_guard.hpp"

#include <sys/wait.h>

#include <algorithm>
#include <utility>

namespace edgedesk {
namespace {

constexpr int kReadTimeoutMs = 200;
constexpr std::size_t kReadChunk = 8192;

const char* kBacklogNoticePrefix = "\r\n\x1b[33m[edgedesk] ";
const char* kBacklogNoticeSuffix = " 字节历史输出已丢弃\x1b[0m\r\n";

} // namespace

TerminalSession::TerminalSession(CreateArgs args)
    : id_(std::move(args.id)),
      title_(std::move(args.title)),
      cwd_(std::move(args.cwd)),
      backlog_capacity_(std::max<std::size_t>(args.backlog_bytes, 16 * 1024)),
      on_exit_notify_(std::move(args.on_exit_notify)),
      cols_(args.spawn.cols),
      rows_(args.spawn.rows),
      created_ms_(now_unix_millis()),
      last_activity_ms_(created_ms_) {
    Error error;
    pty_ = PtyProcess::spawn(args.spawn, error);
    if (!pty_) {
        // The factory validates the shell beforehand; a spawn failure here is
        // reported through create() by re-checking pty_ below.
        pending_reason_ = error.message;
    }
}

std::shared_ptr<TerminalSession> TerminalSession::create(CreateArgs args, Error& error) {
    if (args.id.empty()) {
        error = Error::make(ErrorCode::invalid_argument, "session id must not be empty");
        return nullptr;
    }
    auto session = std::shared_ptr<TerminalSession>(new TerminalSession(std::move(args)));
    if (!session->pty_) {
        error = Error::make(ErrorCode::io_error, "failed to start PTY session: " + session->pending_reason_);
        return nullptr;
    }
    session->running_ = true;
    session->reader_ = std::thread([session] { session->read_loop(); });
    error = Error::none();
    return session;
}

TerminalSession::~TerminalSession() {
    request_close();
    join();
}

TerminalSessionInfo TerminalSession::info() const {
    TerminalSessionInfo info;
    std::lock_guard<std::mutex> lock(mutex_);
    info.id = id_;
    info.title = title_;
    info.cwd = cwd_;
    info.created_ms = created_ms_;
    info.last_activity_ms = last_activity_ms_;
    info.cols = cols_;
    info.rows = rows_;
    info.running = running_;
    info.exited = finalized_state_;
    info.exit_code = exit_code_;
    info.subscribers = subscribers_.size();
    info.buffered_bytes = backlog_.size();
    info.pending_confirmation = pending_confirmation_;
    return info;
}

bool TerminalSession::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

bool TerminalSession::finalized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finalized_state_;
}

void TerminalSession::append_backlog_locked(const char* data, std::size_t length) {
    backlog_.append(data, length);
    if (backlog_.size() > backlog_capacity_) {
        const auto excess = backlog_.size() - backlog_capacity_;
        backlog_.erase(0, excess);
        dropped_bytes_ += excess;
        dropped_notice_ = true;
    }
}

void TerminalSession::finalize(int exit_code) {
    std::vector<std::shared_ptr<SubscriberState>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (finalized_state_) return;
        finalized_state_ = true;
        running_ = false;
        exit_code_ = exit_code;
        last_activity_ms_ = now_unix_millis();
        for (const auto& item : subscribers_) targets.push_back(item.second);
    }
    if (pty_) pty_->terminate_and_reap();
    for (const auto& state : targets) {
        if (!state->callbacks.on_exit) continue;
        std::lock_guard<std::mutex> delivery(state->delivery_mutex);
        state->callbacks.on_exit(exit_code);
    }
    if (on_exit_notify_) on_exit_notify_(exit_code);
}

void TerminalSession::read_loop() {
    std::vector<char> buffer(kReadChunk);
    for (;;) {
        if (close_requested_.load(std::memory_order_relaxed)) {
            finalize(0);
            return;
        }
        const int ready = pty_->wait_readable(kReadTimeoutMs);
        if (ready == 0) {
            int status = 0;
            if (pty_->child_exited(status)) {
                finalize(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
                return;
            }
            continue;
        }
        if (ready < 0) {
            int status = 0;
            if (!pty_->child_exited(status)) {
                continue; // POLLHUP without child exit: retry once more
            }
            finalize(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
            return;
        }
        const long read = pty_->read_some(buffer.data(), buffer.size());
        if (read > 0) {
            const auto length = static_cast<std::size_t>(read);
            std::vector<std::shared_ptr<SubscriberState>> targets;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (finalized_state_) continue;
                append_backlog_locked(buffer.data(), length);
                last_activity_ms_ = now_unix_millis();
                targets.reserve(subscribers_.size());
                for (const auto& item : subscribers_) targets.push_back(item.second);
            }
            // Delivered outside the lock: callbacks may unsubscribe or send on
            // sockets, and must not hold up this session's reader. delivery_mutex
            // waits for any in-progress backlog replay so output stays ordered.
            for (const auto& state : targets) {
                if (!state->callbacks.on_output) continue;
                std::lock_guard<std::mutex> delivery(state->delivery_mutex);
                state->callbacks.on_output(buffer.data(), length);
            }
            continue;
        }
        if (read == 0) {
            int status = 0;
            if (pty_->child_exited(status)) {
                finalize(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
                return;
            }
            continue;
        }
        // read < 0: EAGAIN — loop again.
    }
}

TerminalSession::InputOutcome TerminalSession::submit_input(std::string_view data, std::string& guard_reason) {
    guard_reason.clear();
    if (data.empty()) return InputOutcome::sent;
    if (data.size() > 64 * 1024) {
        guard_reason = "input chunk too large";
        return InputOutcome::rejected;
    }

    std::string to_send;
    to_send.reserve(data.size());
    bool blocked = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || finalized_state_) {
            guard_reason = "session is not running";
            return InputOutcome::rejected;
        }
        for (const char raw : data) {
            const auto byte = static_cast<unsigned char>(raw);
            if (byte == '\r' || byte == '\n') {
                if (!pending_confirmation_) {
                    const auto match = CommandGuard::inspect(line_buffer_);
                    if (match.has_value()) {
                        blocked = true;
                        pending_confirmation_ = true;
                        pending_line_ = line_buffer_;
                        pending_reason_ = match->description;
                        line_buffer_.clear();
                        guard_reason = match->description;
                        break; // do not forward the newline
                    }
                }
                line_buffer_.clear();
                to_send.push_back(raw);
                continue;
            }
            if (byte == 0x7f || byte == 0x08) {
                if (!line_buffer_.empty()) line_buffer_.pop_back();
                to_send.push_back(raw);
                continue;
            }
            if (byte == 0x03 || byte == 0x15 || CommandGuard::breaks_line_tracking(byte)) {
                line_buffer_.clear();
                to_send.push_back(raw);
                continue;
            }
            if (line_buffer_.size() < kMaxTrackedLine) {
                line_buffer_.push_back(raw);
            } else {
                line_buffer_.clear(); // give up tracking very long lines
            }
            to_send.push_back(raw);
        }
    }
    if (!to_send.empty()) {
        Error error = send_raw(to_send);
        if (!error.ok()) {
            guard_reason = error.message;
            return InputOutcome::rejected;
        }
    }
    return blocked ? InputOutcome::guard_blocked : InputOutcome::sent;
}

bool TerminalSession::confirm_pending() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_confirmation_) return false;
        pending_confirmation_ = false;
        pending_line_.clear();
        pending_reason_.clear();
    }
    (void)send_raw("\r");
    return true;
}

bool TerminalSession::cancel_pending() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_confirmation_) return false;
        pending_confirmation_ = false;
        pending_line_.clear();
        pending_reason_.clear();
    }
    // Ctrl+U clears the current shell line so the blocked command is not run.
    (void)send_raw("\x15");
    return true;
}

Error TerminalSession::send_raw(std::string_view data) {
    if (data.empty()) return Error::none();
    Error error;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (finalized_state_) {
            return Error::make(ErrorCode::conflict, "session has exited");
        }
        error = pty_->write_all(data);
        if (error.ok()) last_activity_ms_ = now_unix_millis();
    }
    return error;
}

Error TerminalSession::resize(unsigned short cols, unsigned short rows) {
    if (cols == 0) cols = 80;
    if (rows == 0) rows = 24;
    Error error = pty_->resize(cols, rows);
    if (error.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        cols_ = cols;
        rows_ = rows;
    }
    return error;
}

void TerminalSession::request_close() {
    close_requested_.store(true, std::memory_order_relaxed);
}

void TerminalSession::join() {
    if (!reader_.joinable()) return;
    if (reader_.get_id() == std::this_thread::get_id()) {
        reader_.detach();
        return;
    }
    reader_.join();
}

std::uint64_t TerminalSession::subscribe(Subscriber subscriber) {
    if (!subscriber.on_output && !subscriber.on_exit) return 0;

    auto state = std::make_shared<SubscriberState>();
    state->callbacks = std::move(subscriber);

    // Hold the delivery lock across registration and replay. The reader thread
    // takes the same lock before delivering live output, so it cannot overtake
    // the replay. Never call user callbacks while mutex_ is held.
    std::lock_guard<std::mutex> delivery(state->delivery_mutex);

    std::string replay;
    std::uint64_t token = 0;
    bool finalized_at_insert = false;
    int finalized_code = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (subscribers_.size() >= kMaxSubscribers) return 0;
        token = next_token_++;
        // Snapshot the replay while registering, so no output is lost between
        // the backlog snapshot and the live subscription.
        if (state->callbacks.on_output) {
            if (dropped_notice_) {
                replay = std::string(kBacklogNoticePrefix) + std::to_string(dropped_bytes_) + kBacklogNoticeSuffix;
            }
            replay += backlog_;
        }
        finalized_at_insert = finalized_state_;
        finalized_code = exit_code_;
        subscribers_.emplace(token, state);
    }

    if (state->callbacks.on_output && !replay.empty()) {
        state->callbacks.on_output(replay.data(), replay.size());
    }
    if (finalized_at_insert && state->callbacks.on_exit) {
        state->callbacks.on_exit(finalized_code);
    }
    return token;
}

void TerminalSession::unsubscribe(std::uint64_t token) {
    if (token == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(token);
}

std::string TerminalSession::replay_buffer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dropped_notice_) return backlog_;
    return std::string(kBacklogNoticePrefix) + std::to_string(dropped_bytes_) + kBacklogNoticeSuffix + backlog_;
}

std::size_t TerminalSession::buffered_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backlog_.size();
}

} // namespace edgedesk
