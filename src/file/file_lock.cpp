#include "file/file_lock.hpp"

#include "file/path_sandbox.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace edgedesk {
namespace {

std::string object_key(const struct stat& info, char prefix) {
    return std::string(1, prefix) + ":" + std::to_string(static_cast<std::uint64_t>(info.st_dev)) + ":" +
           std::to_string(static_cast<std::uint64_t>(info.st_ino));
}

} // namespace

Error FileLockManager::identity(const PathSandbox& sandbox, std::string_view path,
                                std::string& key) const {
    key.clear();
    std::string normalized;
    std::string message;
    if (!PathSandbox::normalize(path, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }
    // O_PATH follows symlinks in follow mode and uses lstat semantics otherwise;
    // the resulting descriptor gives stable alias detection for hardlinks.
    UniqueFd fd;
    Error open_error = sandbox.open_file(normalized, O_PATH | O_CLOEXEC, fd);
    if (open_error.ok()) {
        struct stat info {};
        if (::fstat(fd.get(), &info) == 0) {
            key = object_key(info, 'n');
            return Error::none();
        }
    }
    // Missing target: use the normalized parent directory identity + name.
    UniqueFd parent;
    std::string name;
    Error error = sandbox.open_parent(normalized, parent, name);
    if (!error.ok()) return error;
    struct stat parent_info {};
    if (::fstat(parent.get(), &parent_info) != 0) {
        return Error::make(ErrorCode::io_error,
                           "cannot identify lock parent: " + std::string(std::strerror(errno)), errno);
    }
    key = object_key(parent_info, 'p') + "/" + name;
    return Error::none();
}

Error FileLockManager::acquire(const PathSandbox& sandbox, std::string_view path, bool exclusive,
                               std::uint64_t& token) {
    token = 0;
    std::string key;
    Error error = identity(sandbox, path, key);
    if (!error.ok()) return error;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entry = locks_[key];
    if (exclusive) {
        if (entry.exclusive || entry.shared > 0) {
            if (entry.shared == 0 && !entry.exclusive) locks_.erase(key);
            return Error::make(ErrorCode::busy, "file is locked: " + std::string(path));
        }
        entry.exclusive = true;
        entry.exclusive_token = next_token_;
    } else {
        if (entry.exclusive) {
            if (entry.shared == 0 && !entry.exclusive) locks_.erase(key);
            return Error::make(ErrorCode::busy, "file is locked for writing: " + std::string(path));
        }
        ++entry.shared;
    }
    token = next_token_++;
    tokens_[token] = key;
    return Error::none();
}

Error FileLockManager::acquire_shared(const PathSandbox& sandbox, std::string_view path,
                                      std::uint64_t& token) {
    return acquire(sandbox, path, false, token);
}

Error FileLockManager::acquire_exclusive(const PathSandbox& sandbox, std::string_view path,
                                         std::uint64_t& token) {
    return acquire(sandbox, path, true, token);
}

Error FileLockManager::acquire_exclusive_many(const PathSandbox& sandbox,
                                              const std::vector<std::string>& paths,
                                              std::vector<std::uint64_t>& tokens) {
    tokens.clear();
    std::vector<std::pair<std::string, std::string>> keys; // (identity, path)
    keys.reserve(paths.size());
    for (const auto& path : paths) {
        std::string key;
        Error error = identity(sandbox, path, key);
        if (!error.ok()) return error;
        keys.emplace_back(std::move(key), path);
    }
    std::sort(keys.begin(), keys.end());
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (keys[i].first == keys[i - 1].first) {
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(i));
            --i;
        }
    }
    for (const auto& item : keys) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = locks_[item.first];
        if (entry.exclusive || entry.shared > 0) {
            if (entry.shared == 0 && !entry.exclusive) locks_.erase(item.first);
            for (const auto token : tokens) release_locked(token);
            tokens.clear();
            return Error::make(ErrorCode::busy, "file is locked: " + item.second);
        }
        entry.exclusive = true;
        entry.exclusive_token = next_token_;
        const std::uint64_t token = next_token_++;
        tokens_[token] = item.first;
        tokens.push_back(token);
    }
    return Error::none();
}

void FileLockManager::release_locked(std::uint64_t token) {
    const auto token_it = tokens_.find(token);
    if (token_it == tokens_.end()) return;
    const auto lock_it = locks_.find(token_it->second);
    if (lock_it != locks_.end()) {
        if (lock_it->second.exclusive && lock_it->second.exclusive_token == token) {
            lock_it->second.exclusive = false;
            lock_it->second.exclusive_token = 0;
        } else if (lock_it->second.shared > 0) {
            --lock_it->second.shared;
        }
        if (!lock_it->second.exclusive && lock_it->second.shared == 0) locks_.erase(lock_it);
    }
    tokens_.erase(token_it);
}

void FileLockManager::release(std::uint64_t token) {
    if (token == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    release_locked(token);
}

std::size_t FileLockManager::active_locks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tokens_.size();
}

} // namespace edgedesk
