#include "core/atomic_file.hpp"

#include "core/util.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace edgedesk {
namespace {

bool valid_file_name(const std::string& name, std::string& error) {
    if (name.empty() || name == "." || name == "..") {
        error = "invalid file name";
        return false;
    }
    if (name.find('/') != std::string::npos || name.find('\0') != std::string::npos) {
        error = "file name must not contain '/' or NUL";
        return false;
    }
    return true;
}

bool write_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool fsync_directory(int dir_fd) {
    // Some filesystems return EINVAL/EOPNOTSUPP for fsync on a directory; only
    // report genuinely fatal errors.
    if (::fsync(dir_fd) == 0) return true;
    return errno == EINVAL || errno == EOPNOTSUPP;
}

} // namespace

UniqueFd open_private_dir(const std::string& path, std::string& error) {
    if (path.empty() || path.front() != '/') {
        error = "data directory must be an absolute path";
        return {};
    }
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        if (errno != ENOENT) {
            error = "cannot inspect data directory " + path + ": " + std::strerror(errno);
            return {};
        }
        if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
            error = "cannot create data directory " + path + ": " + std::strerror(errno);
            return {};
        }
    }
    if (::stat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        error = "data path exists but is not a directory: " + path;
        return {};
    }
    if (info.st_uid != ::geteuid()) {
        error = "data directory is not owned by the service user: " + path;
        return {};
    }
    if ((info.st_mode & 0777) != 0700 && ::chmod(path.c_str(), 0700) != 0) {
        error = "cannot restrict data directory permissions: " + path + ": " + std::strerror(errno);
        return {};
    }

    UniqueFd fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!fd.valid()) {
        error = "cannot open data directory " + path + ": " + std::strerror(errno);
    }
    return fd;
}

bool read_file_at(int dir_fd, const std::string& name, std::size_t max_bytes, std::string& out,
                  bool& missing, std::string& error) {
    missing = false;
    out.clear();
    if (!valid_file_name(name, error)) return false;
    UniqueFd fd(::openat(dir_fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!fd.valid()) {
        if (errno == ENOENT) {
            missing = true;
            return true;
        }
        error = "cannot open " + name + ": " + std::strerror(errno);
        return false;
    }
    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        error = "cannot stat " + name + ": " + std::strerror(errno);
        return false;
    }
    if (!S_ISREG(info.st_mode)) {
        error = name + " is not a regular file";
        return false;
    }
    if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > max_bytes) {
        error = name + " exceeds the allowed size";
        return false;
    }
    out.resize(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0;
    while (offset < out.size()) {
        const auto read = ::read(fd.get(), out.data() + offset, out.size() - offset);
        if (read < 0) {
            if (errno == EINTR) continue;
            error = "cannot read " + name + ": " + std::strerror(errno);
            return false;
        }
        if (read == 0) break;
        offset += static_cast<std::size_t>(read);
    }
    out.resize(offset);
    return true;
}

bool atomic_write_file_at(int dir_fd, const std::string& name, std::string_view data, mode_t mode,
                          std::string& error) {
    if (!valid_file_name(name, error)) return false;
    const std::string temp_name = "." + name + ".tmp." + std::to_string(::getpid()) + "." + random_hex(8);
    UniqueFd fd(::openat(dir_fd, temp_name.c_str(),
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode));
    if (!fd.valid()) {
        error = "cannot create temporary file for " + name + ": " + std::strerror(errno);
        return false;
    }
    bool ok = write_all(fd.get(), data);
    if (!ok) error = "cannot write temporary file for " + name + ": " + std::strerror(errno);
    if (ok && ::fchmod(fd.get(), mode) != 0) {
        ok = false;
        error = "cannot set permissions on " + name + ": " + std::strerror(errno);
    }
    if (ok && ::fsync(fd.get()) != 0) {
        ok = false;
        error = "cannot fsync " + name + ": " + std::strerror(errno);
    }
    if (ok && ::renameat(dir_fd, temp_name.c_str(), dir_fd, name.c_str()) != 0) {
        ok = false;
        error = "cannot replace " + name + ": " + std::strerror(errno);
    }
    if (ok && !fsync_directory(dir_fd)) {
        ok = false;
        error = "cannot fsync data directory after writing " + name;
    }
    if (!ok) ::unlinkat(dir_fd, temp_name.c_str(), 0);
    return ok;
}

bool remove_file_at(int dir_fd, const std::string& name, std::string& error) {
    if (!valid_file_name(name, error)) return false;
    if (::unlinkat(dir_fd, name.c_str(), 0) == 0) return true;
    if (errno == ENOENT) return true;
    error = "cannot remove " + name + ": " + std::strerror(errno);
    return false;
}

} // namespace edgedesk
