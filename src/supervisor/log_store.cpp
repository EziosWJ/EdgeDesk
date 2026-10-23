#include "supervisor/log_store.hpp"

#include "core/utf8.hpp"
#include "supervisor/supervisor_store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace edgedesk::supervisor {
namespace {

constexpr std::uint64_t kAbsoluteReadCap = 256 * 1024;
constexpr std::uint64_t kDefaultTail = 16 * 1024;

Error write_all(int fd, const char* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(fd, data + written, size - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            return Error::make(ErrorCode::io_error, std::string("log write failed: ") + std::strerror(errno),
                               errno);
        }
        written += static_cast<std::size_t>(result);
    }
    return Error::none();
}

} // namespace

bool LogStore::valid_id(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (const char ch : id) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

std::unique_ptr<LogStore> LogStore::open(const std::string& directory, Error& error) {
    std::string message;
    int directory_fd = -1;
    if (!ensure_private_directory(directory, message, &directory_fd)) {
        if (directory_fd >= 0) ::close(directory_fd);
        error = Error::make(ErrorCode::permission_denied, message);
        return nullptr;
    }
    error = Error::none();
    return std::unique_ptr<LogStore>(new LogStore(directory, directory_fd));
}

LogStore::~LogStore() {
    for (auto& item : files_) {
        if (item.second.fd >= 0) ::close(item.second.fd);
    }
    files_.clear();
    if (directory_fd_ >= 0) ::close(directory_fd_);
}

std::string LogStore::name_for(std::string_view id) const {
    return std::string(id) + ".log";
}

std::string LogStore::path_for(std::string_view id) const {
    return directory_ + "/" + name_for(id);
}

Error LogStore::open_append_locked(std::string_view id, OpenFile& out) {
    if (!valid_id(id)) {
        return Error::make(ErrorCode::invalid_argument, "invalid service ID for log file");
    }
    const std::string name = name_for(id);
    const std::string path = path_for(id);
    const int fd = ::openat(directory_fd_, name.c_str(),
                            O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW |
                                O_NONBLOCK,
                            0600);
    if (fd < 0) {
        return Error::make(ErrorCode::io_error,
                           "cannot open service log " + path + ": " + std::strerror(errno), errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid()) {
        const int saved = errno;
        ::close(fd);
        return Error::make(ErrorCode::io_error,
                           "service log is not a regular file owned by the supervisor: " + path +
                               (saved != 0 ? ": " + std::string(std::strerror(saved)) : std::string()));
    }
    (void)::fchmod(fd, 0600);
    out.fd = fd;
    out.size = static_cast<std::uint64_t>(info.st_size);
    return Error::none();
}

Error LogStore::append(std::string_view id, std::string_view data, std::uint64_t max_bytes) {
    if (data.empty()) return Error::none();
    std::string key(id);
    const std::uint64_t limit = max_bytes == 0 ? 4096 : max_bytes;
    OpenFile* file = nullptr;
    const auto existing = files_.find(key);
    if (existing == files_.end()) {
        OpenFile opened;
        Error error = open_append_locked(id, opened);
        if (!error.ok()) return error;
        auto inserted = files_.emplace(key, opened);
        file = &inserted.first->second;
    } else {
        file = &existing->second;
    }

    // A single chunk larger than the configured cap is reduced to its tail so
    // a misbehaving service cannot force unbounded buffering.
    if (static_cast<std::uint64_t>(data.size()) > limit) {
        data.remove_prefix(data.size() - static_cast<std::size_t>(limit));
    }

    if (file->size + data.size() > limit) {
        if (file->fd >= 0) ::close(file->fd);
        file->fd = -1;
        const std::string name = name_for(id);
        const std::string rotated = name + ".1";
        const std::string path = path_for(id);
        // renameat/unlinkat stay relative to the verified directory descriptor.
        (void)::unlinkat(directory_fd_, rotated.c_str(), 0);
        if (::renameat(directory_fd_, name.c_str(), directory_fd_, rotated.c_str()) != 0 &&
            errno != ENOENT) {
            return Error::make(ErrorCode::io_error,
                               "cannot rotate service log " + path + ": " + std::strerror(errno), errno);
        }
        OpenFile reopened;
        Error error = open_append_locked(id, reopened);
        if (!error.ok()) return error;
        *file = reopened;
    }

    // If even the truncated chunk cannot fit in an empty file, keep only the
    // newest bytes that still fit instead of blocking rotation forever.
    if (file->size + data.size() > limit) {
        const std::size_t keep = static_cast<std::size_t>(limit > file->size ? limit - file->size : 0);
        data.remove_prefix(data.size() > keep ? data.size() - keep : 0);
    }
    Error error = write_all(file->fd, data.data(), data.size());
    if (error.ok()) file->size += data.size();
    return error;
}

Error LogStore::read(std::string_view id, std::uint64_t tail_bytes, bool has_cursor,
                     std::uint64_t cursor, std::uint64_t max_bytes, LogReadResult& out) const {
    out = {};
    if (!valid_id(id)) {
        return Error::make(ErrorCode::invalid_argument, "invalid service ID for log read");
    }
    const std::string path = path_for(id);
    const std::string name = name_for(id);
    const int fd = ::openat(directory_fd_, name.c_str(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) return Error::none();
        return Error::make(ErrorCode::io_error,
                           "cannot read service log " + path + ": " + std::strerror(errno), errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        const int saved = errno;
        ::close(fd);
        return Error::make(ErrorCode::io_error,
                           "service log is not a regular file: " + path +
                               (saved != 0 ? ": " + std::string(std::strerror(saved)) : std::string()));
    }
    const std::uint64_t size = static_cast<std::uint64_t>(info.st_size);
    const std::uint64_t limit = std::min<std::uint64_t>(
        max_bytes == 0 ? kDefaultTail : max_bytes, kAbsoluteReadCap);
    std::uint64_t start = 0;
    if (has_cursor) {
        if (cursor > size) {
            out.reset = true;
            start = size > limit ? size - limit : 0;
        } else {
            start = cursor;
        }
    } else {
        const std::uint64_t tail = std::min<std::uint64_t>(tail_bytes == 0 ? kDefaultTail : tail_bytes,
                                                           kAbsoluteReadCap);
        start = size > tail ? size - tail : 0;
    }
    const std::uint64_t end = std::min<std::uint64_t>(size, start + limit);
    out.cursor = start;
    out.size = size;
    out.truncated = end < size;
    if (end > start) {
        std::string buffer(static_cast<std::size_t>(end - start), '\0');
        std::size_t filled = 0;
        while (filled < buffer.size()) {
            const ssize_t result =
                ::pread(fd, buffer.data() + filled, buffer.size() - filled,
                        static_cast<off_t>(start + filled));
            if (result < 0) {
                if (errno == EINTR) continue;
                const int saved = errno;
                ::close(fd);
                return Error::make(ErrorCode::io_error,
                                   "cannot read service log " + path + ": " + std::strerror(saved), saved);
            }
            if (result == 0) break;
            filled += static_cast<std::size_t>(result);
        }
        buffer.resize(filled);
        out.content = sanitize_utf8(buffer);
        out.next_cursor = start + filled;
    } else {
        out.next_cursor = start;
    }
    ::close(fd);
    return Error::none();
}

void LogStore::close(std::string_view id) {
    const auto found = files_.find(std::string(id));
    if (found == files_.end()) return;
    if (found->second.fd >= 0) ::close(found->second.fd);
    files_.erase(found);
}

Error LogStore::remove(std::string_view id) {
    if (!valid_id(id)) {
        return Error::make(ErrorCode::invalid_argument, "invalid service ID for log removal");
    }
    close(id);
    const std::string name = name_for(id);
    const std::string path = path_for(id);
    if (::unlinkat(directory_fd_, name.c_str(), 0) != 0 && errno != ENOENT) {
        return Error::make(ErrorCode::io_error,
                           "cannot remove service log " + path + ": " + std::strerror(errno), errno);
    }
    const std::string rotated = name + ".1";
    if (::unlinkat(directory_fd_, rotated.c_str(), 0) != 0 && errno != ENOENT) {
        return Error::make(ErrorCode::io_error,
                           "cannot remove rotated service log " + path + ": " + std::strerror(errno),
                           errno);
    }
    return Error::none();
}

} // namespace edgedesk::supervisor
