#include "core/fd.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace edgedesk {

int duplicate_fd(int fd) {
    if (fd < 0) return -1;
    const int copy = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    return copy;
}

} // namespace edgedesk
