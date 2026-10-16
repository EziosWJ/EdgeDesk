#include "terminal/terminal_manager.hpp"

#include "core/util.hpp"

#include <algorithm>
#include <unistd.h>

namespace edgedesk {

TerminalManager::TerminalManager(TerminalManagerOptions options) : options_(std::move(options)) {
    if (options_.max_sessions < 1) options_.max_sessions = 1;
    if (options_.backlog_bytes < 16 * 1024) options_.backlog_bytes = 16 * 1024;
    shell_available_ = ::access(options_.bash_path.c_str(), X_OK) == 0;
}

TerminalManager::~TerminalManager() {
    shutdown();
}

std::size_t TerminalManager::session_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

std::size_t TerminalManager::running_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& item : sessions_) {
        if (item.second->running()) ++count;
    }
    return count;
}

Error TerminalManager::create(const CreateRequest& request, std::shared_ptr<TerminalSession>& out) {
    if (!shell_available_) {
        return Error::make(ErrorCode::unsupported,
                           "login shell not available: " + options_.bash_path +
                               " (EdgeDesk never falls back to /bin/sh)");
    }
    std::string cwd = request.cwd;
    if (cwd.empty()) {
        cwd = options_.default_cwd;
    }
    if (!cwd.empty() && (cwd.front() != '/' || ::access(cwd.c_str(), X_OK) != 0)) {
        return Error::make(ErrorCode::invalid_argument, "working directory is not accessible: " + cwd);
    }
    if (cwd.empty()) cwd = "/";

    std::size_t created_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t running = 0;
        for (const auto& item : sessions_) {
            if (item.second->running()) ++running;
        }
        if (running >= static_cast<std::size_t>(options_.max_sessions)) {
            return Error::make(ErrorCode::limit_exceeded,
                               "terminal session limit reached (" + std::to_string(options_.max_sessions) + ")");
        }
        created_index = ++created_total_;
    }

    TerminalSession::CreateArgs args;
    args.id = random_hex(16);
    if (args.id.empty()) {
        return Error::make(ErrorCode::io_error, "cannot generate a session id (no entropy)");
    }
    args.title = request.title.empty() ? ("Bash " + std::to_string(created_index)) : request.title;
    if (args.title.size() > 64) args.title.resize(64);
    args.cwd = cwd;
    args.backlog_bytes = options_.backlog_bytes;
    args.spawn.shell = options_.bash_path;
    args.spawn.arguments = {"-l"};
    args.spawn.cwd = cwd;
    args.spawn.cols = request.cols;
    args.spawn.rows = request.rows;
    for (const auto& pair : request.extra_env) {
        args.spawn.extra_env.emplace_back(pair.first, pair.second);
    }
    args.spawn.extra_env.emplace_back("EDGEDESK_TERMINAL", "1");

    Error error;
    auto session = TerminalSession::create(std::move(args), error);
    if (!session) return error;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.emplace(session->id(), session);
    }
    out = std::move(session);
    return Error::none();
}

std::shared_ptr<TerminalSession> TerminalManager::find(std::string_view id) const {
    if (id.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(std::string(id));
    return it == sessions_.end() ? nullptr : it->second;
}

std::vector<TerminalSessionInfo> TerminalManager::list() const {
    std::vector<std::shared_ptr<TerminalSession>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.reserve(sessions_.size());
        for (const auto& item : sessions_) snapshot.push_back(item.second);
    }
    std::vector<TerminalSessionInfo> infos;
    infos.reserve(snapshot.size());
    for (const auto& session : snapshot) {
        infos.push_back(session->info());
    }
    std::sort(infos.begin(), infos.end(),
              [](const TerminalSessionInfo& a, const TerminalSessionInfo& b) { return a.created_ms < b.created_ms; });
    return infos;
}

bool TerminalManager::request_close(std::string_view id) {
    std::shared_ptr<TerminalSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(std::string(id));
        if (it == sessions_.end()) return false;
        session = it->second;
        sessions_.erase(it);
    }
    session->request_close();
    return true;
}

void TerminalManager::sweep(std::int64_t now_ms) {
    std::vector<std::shared_ptr<TerminalSession>> to_close;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            const auto info = it->second->info();
            const auto idle_ms = now_ms - info.last_activity_ms;
            if (!info.running) {
                // Keep exited sessions briefly so a reconnecting client can
                // observe the exit status, then reclaim them.
                if (idle_ms > 60 * 1000) {
                    it = sessions_.erase(it);
                    continue;
                }
            } else if (options_.idle_timeout_seconds > 0 && info.subscribers == 0 &&
                       idle_ms > static_cast<std::int64_t>(options_.idle_timeout_seconds) * 1000) {
                to_close.push_back(it->second);
                it = sessions_.erase(it);
                continue;
            }
            ++it;
        }
    }
    for (const auto& session : to_close) {
        session->request_close();
    }
}

void TerminalManager::shutdown() {
    std::vector<std::shared_ptr<TerminalSession>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : sessions_) {
            snapshot.push_back(item.second);
        }
        sessions_.clear();
    }
    for (const auto& session : snapshot) {
        session->request_close();
    }
    for (const auto& session : snapshot) {
        session->join();
    }
}

} // namespace edgedesk
