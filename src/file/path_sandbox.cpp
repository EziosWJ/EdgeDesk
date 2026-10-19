#include "file/path_sandbox.hpp"

#include "core/types.hpp"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

namespace edgedesk {
namespace {

constexpr int kMaxLinkDepth = 16;
constexpr std::size_t kMaxPathLength = 4096;

Error errno_to_error(const std::string& what) {
    switch (errno) {
        case ENOENT:
        case ENOTDIR:
            return Error::make(ErrorCode::not_found, what);
        case EACCES:
        case EPERM:
            return Error::make(ErrorCode::permission_denied, what, errno);
        case EEXIST:
            return Error::make(ErrorCode::already_exists, what, errno);
        case ELOOP:
            return Error::make(ErrorCode::permission_denied, what + " (symlink)", errno);
        case EISDIR:
            return Error::make(ErrorCode::is_a_directory, what, errno);
        case ENOSPC:
            return Error::make(ErrorCode::too_large, what + " (no space left)", errno);
        case EROFS:
            return Error::make(ErrorCode::unavailable, what + " (read-only file system)", errno);
        default:
            return Error::make(ErrorCode::io_error, what + ": " + std::strerror(errno), errno);
    }
}

std::string parent_of(const std::string& normalized) {
    const auto slash = normalized.rfind('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return normalized.substr(0, slash);
}

std::string join_logical(std::string_view parent, std::string_view child) {
    if (!child.empty() && child.front() == '/') return std::string(child);
    if (parent.empty() || parent == "/") return "/" + std::string(child);
    return std::string(parent) + "/" + std::string(child);
}

// Lexically pops ".." against already-resolved components; escaping the root
// is an error. Used only for symlink targets discovered during the walk.
bool strip_parent_components(std::string_view path, std::string& out, std::string& error_message) {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const auto next = path.find('/', pos);
        const auto end = next == std::string_view::npos ? path.size() : next;
        const auto part = path.substr(pos, end - pos);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (parts.empty()) {
                    error_message = "symlink target escapes the sandbox root";
                    return false;
                }
                parts.pop_back();
            } else {
                parts.emplace_back(part);
            }
        }
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }
    out = "/";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out.push_back('/');
        out += parts[i];
    }
    return true;
}

Error enumerate_directory(int dir_fd, const std::string& prefix,
                          std::vector<PathSandbox::TreeEntry>& out) {
    const int duplicate = duplicate_fd(dir_fd);
    if (duplicate < 0) return errno_to_error("cannot duplicate directory descriptor for enumeration");
    DIR* dir = ::fdopendir(duplicate);
    if (dir == nullptr) {
        const int saved = errno;
        ::close(duplicate);
        errno = saved;
        return errno_to_error("cannot enumerate " + (prefix.empty() ? std::string("sandbox root") : prefix));
    }
    const int listing_fd = ::dirfd(dir);
    for (;;) {
        errno = 0;
        struct dirent* entry = ::readdir(dir);
        if (entry == nullptr) {
            if (errno == 0) break;
            const int saved = errno;
            ::closedir(dir);
            errno = saved;
            return errno_to_error("cannot read directory " +
                                  (prefix.empty() ? std::string("sandbox root") : prefix));
        }
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const std::string relative = prefix.empty() ? name : prefix + "/" + name;
        struct stat info {};
        if (::fstatat(listing_fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
            const int saved = errno;
            ::closedir(dir);
            errno = saved;
            return errno_to_error("cannot stat " + relative);
        }
        PathSandbox::TreeEntry node;
        node.path = relative;
        if (S_ISREG(info.st_mode)) {
            node.kind = PathSandbox::NodeKind::regular_file;
            node.size = static_cast<std::uint64_t>(info.st_size);
        } else if (S_ISDIR(info.st_mode)) {
            node.kind = PathSandbox::NodeKind::directory;
        } else if (S_ISLNK(info.st_mode)) {
            node.kind = PathSandbox::NodeKind::symlink;
        } else {
            node.kind = PathSandbox::NodeKind::other;
        }
        out.push_back(node);
        if (node.kind != PathSandbox::NodeKind::directory) continue;

        const int child = ::openat(listing_fd, name.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) {
            const int saved = errno;
            ::closedir(dir);
            errno = saved;
            return errno_to_error("cannot open directory " + relative);
        }
        Error error = enumerate_directory(child, relative, out);
        ::close(child);
        if (!error.ok()) {
            ::closedir(dir);
            return error;
        }
    }
    if (::closedir(dir) != 0) return errno_to_error("cannot close directory listing");
    return Error::none();
}

} // namespace

std::unique_ptr<PathSandbox> PathSandbox::create(const std::string& root, bool follow_symlinks, Error& error) {
    const int fd = ::open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        error = errno_to_error("cannot open sandbox root " + root);
        return nullptr;
    }
    UniqueFd root_fd(fd);
    struct stat info {};
    if (::fstat(root_fd.get(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        error = Error::make(ErrorCode::invalid_argument, "sandbox root is not a directory: " + root);
        return nullptr;
    }
    error = Error::none();
    return std::unique_ptr<PathSandbox>(new PathSandbox(root, std::move(root_fd), follow_symlinks));
}

PathSandbox::PathSandbox(std::string root, UniqueFd root_fd, bool follow_symlinks)
    : root_(std::move(root)), root_fd_(std::move(root_fd)), follow_symlinks_(follow_symlinks) {}

PathSandbox::~PathSandbox() = default;

bool PathSandbox::normalize(std::string_view path, std::string& out, std::string& error_message) {
    if (path.empty() || path.front() != '/') {
        error_message = "path must be absolute (start with '/')";
        return false;
    }
    if (path.size() > kMaxPathLength) {
        error_message = "path too long";
        return false;
    }
    if (path.find('\0') != path.npos) {
        error_message = "path contains NUL byte";
        return false;
    }
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const auto next = path.find('/', pos);
        const auto end = next == std::string_view::npos ? path.size() : next;
        const auto part = path.substr(pos, end - pos);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (parts.empty()) {
                    error_message = "path escapes the sandbox root";
                    return false;
                }
                parts.pop_back();
            } else {
                parts.emplace_back(part);
            }
        }
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }
    out.clear();
    out.reserve(path.size());
    for (const auto& part : parts) {
        out.push_back('/');
        out += part;
    }
    if (out.empty()) out = "/";
    return true;
}

Error PathSandbox::open_root(UniqueFd& out_fd) const {
    const int fd = duplicate_fd(root_fd_.get());
    if (fd < 0) return errno_to_error("cannot duplicate sandbox root fd");
    out_fd.reset(fd);
    return Error::none();
}

Error PathSandbox::walk(const std::string& normalized, int depth, UniqueFd& out_dir,
                        std::string& out_name) const {
    if (depth > kMaxLinkDepth) {
        return Error::make(ErrorCode::permission_denied, "too many symbolic links (possible loop)");
    }
    std::vector<std::string> components;
    std::size_t pos = 1;
    while (pos < normalized.size()) {
        const auto next = normalized.find('/', pos);
        const auto end = next == std::string::npos ? normalized.size() : next;
        components.push_back(normalized.substr(pos, end - pos));
        if (next == std::string::npos) break;
        pos = next + 1;
    }

    Error error = Error::none();
    UniqueFd current;
    if ((error = open_root(current)).ok() == false) return error;
    if (components.empty()) {
        out_name = ".";
        out_dir = std::move(current);
        return Error::none();
    }

    for (std::size_t i = 0; i + 1 < components.size(); ++i) {
        const std::string& component = components[i];
        const int fd = ::openat(current.get(), component.c_str(),
                                O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOTDIR || errno == ENOENT) {
                // Could be a symlink or a non-directory. Inspect without following.
                struct stat link_info {};
                if (::fstatat(current.get(), component.c_str(), &link_info, AT_SYMLINK_NOFOLLOW) == 0 &&
                    S_ISLNK(link_info.st_mode)) {
                    if (!follow_symlinks_) {
                        return Error::make(ErrorCode::permission_denied,
                                           "symbolic links are not allowed: " + component);
                    }
                    std::string prefix = "/";
                    for (std::size_t j = 0; j < i; ++j) {
                        if (prefix.size() > 1) prefix.push_back('/');
                        prefix += components[j];
                    }
                    std::string target;
                    if (!resolve_link_target(join_logical(prefix, component), target, depth).ok()) {
                        return Error::make(ErrorCode::permission_denied, "cannot resolve symlink: " + component);
                    }
                    std::string remainder = join_logical(prefix, target);
                    for (std::size_t j = i + 1; j < components.size(); ++j) {
                        remainder += "/" + components[j];
                    }
                    std::string normalized_remainder;
                    if (!strip_parent_components(remainder, normalized_remainder, target)) {
                        return Error::make(ErrorCode::permission_denied, "symlink target escapes the sandbox root");
                    }
                    return walk(normalized_remainder, depth + 1, out_dir, out_name);
                }
            }
            if (errno == ENOENT) {
                return Error::make(ErrorCode::not_found, "not found: /" + component);
            }
            if (errno == ENOTDIR) {
                return Error::make(ErrorCode::not_a_directory, "not a directory: " + component);
            }
            return errno_to_error("cannot open " + component);
        }
        current.reset(fd);
        struct stat info {};
        if (::fstat(current.get(), &info) == 0 && S_ISLNK(info.st_mode)) {
            if (!follow_symlinks_) {
                return Error::make(ErrorCode::permission_denied,
                                   "symbolic links are not allowed: " + component);
            }
            std::string prefix = "/";
            for (std::size_t j = 0; j < i; ++j) {
                if (prefix.size() > 1) prefix.push_back('/');
                prefix += components[j];
            }
            std::string target;
            if (!resolve_link_target(join_logical(prefix, component), target, depth).ok()) {
                return Error::make(ErrorCode::permission_denied, "cannot resolve symlink: " + component);
            }
            std::string remainder = join_logical(prefix, target);
            for (std::size_t j = i + 1; j < components.size(); ++j) {
                remainder += "/" + components[j];
            }
            std::string normalized_remainder;
            std::string message;
            if (!strip_parent_components(remainder, normalized_remainder, message)) {
                return Error::make(ErrorCode::permission_denied, "symlink target escapes the sandbox root");
            }
            return walk(normalized_remainder, depth + 1, out_dir, out_name);
        }
    }

    out_name = components.back();
    out_dir = std::move(current);
    return Error::none();
}

Error PathSandbox::resolve_link_target(std::string_view logical_path, std::string& out_target_path,
                                      int depth) const {
    if (depth > kMaxLinkDepth) {
        return Error::make(ErrorCode::permission_denied, "too many symbolic links (possible loop)");
    }
    const std::string normalized = std::string(logical_path);
    UniqueFd dir;
    std::string name;
    if (!normalized.empty() && normalized != "/") {
        const auto slash = normalized.rfind('/');
        const std::string parent = slash == std::string::npos || slash == 0 ? "/" : normalized.substr(0, slash);
        name = slash == std::string::npos ? normalized : normalized.substr(slash + 1);
        // Open the directory that contains the link. walk() separates the final
        // component, so append "." to obtain the directory itself. Reading the
        // link from the wrong directory made nested links look nonexistent.
        if (parent == "/") {
            Error error = open_root(dir);
            if (!error.ok()) return error;
        } else {
            std::string parent_name;
            Error error = walk(parent + "/.", depth, dir, parent_name);
            if (!error.ok()) return error;
        }
    } else {
        Error error = open_root(dir);
        if (!error.ok()) return error;
    }
    if (name.empty()) {
        return Error::make(ErrorCode::invalid_argument, "cannot resolve link without a name");
    }
    char buffer[PATH_MAX + 1];
    const auto length = ::readlinkat(dir.get(), name.c_str(), buffer, PATH_MAX);
    if (length < 0) return errno_to_error("cannot read symlink: " + name);
    out_target_path = std::string(buffer, static_cast<std::size_t>(length));
    return Error::none();
}

Error PathSandbox::open_parent(std::string_view path, UniqueFd& out_dir, std::string& out_name) const {
    std::string normalized;
    std::string message;
    if (!normalize(path, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }
    return walk(normalized, 0, out_dir, out_name);
}

Error PathSandbox::open_dir(std::string_view path, UniqueFd& out_fd) const {
    std::string normalized;
    std::string message;
    if (!normalize(path, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }
    if (normalized == "/") {
        // Open the root with real read permission so callers can fdopendir()
        // it; O_PATH descriptors cannot be used with fdopendir().
        const int fd = ::openat(root_fd_.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return errno_to_error("cannot open sandbox root");
        out_fd.reset(fd);
        return Error::none();
    }
    return open_file_depth(normalized, O_RDONLY | O_DIRECTORY, out_fd, 0, 0);
}

Error PathSandbox::open_file_depth(std::string_view path, int flags, UniqueFd& out_fd,
                                   mode_t create_mode, int depth) const {
    std::string normalized;
    std::string message;
    if (!normalize(path, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }
    if (normalized == "/") {
        return Error::make(ErrorCode::is_a_directory, "path is a directory: /");
    }
    UniqueFd dir;
    std::string name;
    Error error = walk(normalized, depth, dir, name);
    if (!error.ok()) return error;

    // O_NOFOLLOW is always applied; when symlink following is enabled the link
    // is resolved and re-validated inside the sandbox instead of letting the
    // kernel follow it (which could escape the root).
    const int effective_flags = flags | O_CLOEXEC | O_NOFOLLOW;

    const int fd = ::openat(dir.get(), name.c_str(), effective_flags, create_mode);
    if (fd < 0) {
        if ((errno == ELOOP || errno == ENOTDIR) && follow_symlinks_) {
            std::string target;
            error = resolve_link_target(normalized, target, depth);
            if (!error.ok()) return error;
            const std::string parent = parent_of(normalized);
            std::string combined;
            std::string strip_message;
            if (!strip_parent_components(join_logical(parent, target), combined, strip_message)) {
                return Error::make(ErrorCode::permission_denied, "symlink target escapes the sandbox root");
            }
            return open_file_depth(combined, flags, out_fd, create_mode, depth + 1);
        }
        return errno_to_error("cannot open " + normalized);
    }
    out_fd.reset(fd);
    return Error::none();
}

Error PathSandbox::open_file(std::string_view path, int flags, UniqueFd& out_fd, mode_t create_mode) const {
    return open_file_depth(path, flags, out_fd, create_mode, 0);
}

Error PathSandbox::stat(std::string_view path, struct stat& out) const {
    std::string normalized;
    std::string message;
    if (!normalize(path, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }
    if (normalized == "/") {
        if (::fstat(root_fd_.get(), &out) != 0) return errno_to_error("cannot stat sandbox root");
        return Error::none();
    }
    UniqueFd dir;
    std::string name;
    Error error = open_parent(normalized, dir, name);
    if (!error.ok()) return error;
    if (::fstatat(dir.get(), name.c_str(), &out, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno_to_error("cannot stat " + normalized);
    }
    return Error::none();
}

Error PathSandbox::enumerate_tree(std::vector<TreeEntry>& out) const {
    out.clear();
    const int fd = ::openat(root_fd_.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return errno_to_error("cannot open sandbox root for enumeration");
    UniqueFd root(fd);
    return enumerate_directory(root.get(), "", out);
}

} // namespace edgedesk
