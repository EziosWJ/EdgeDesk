#pragma once

#include <cerrno>
#include <unistd.h>
#include <utility>

// Minimal RAII file-descriptor owner used by file/terminal/security modules.
// No libhv types are needed here.

namespace edgedesk {

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            // close(2) can be interrupted; the fd is still closed on EINTR for
            // Linux, so a single retry loop would risk closing a reused fd.
            while (::close(fd_) == -1 && errno == EINTR) {
            }
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

// Duplicates a descriptor with CLOEXEC. Returns -1 on failure.
int duplicate_fd(int fd);

} // namespace edgedesk
