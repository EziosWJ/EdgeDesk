#include "file/file_service.hpp"

#include "core/types.hpp"
#include "core/util.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace edgedesk {
namespace {

std::string join_path(std::string_view parent, std::string_view name) {
    if (name.empty()) return std::string(parent);
    if (!parent.empty() && parent.back() == '/') return std::string(parent) + std::string(name);
    return std::string(parent) + "/" + std::string(name);
}

std::string base_name(std::string_view path) {
    const auto slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

Error errno_error(const std::string& what) {
    switch (errno) {
        case ENOENT:
        case ENOTDIR:
            return Error::make(ErrorCode::not_found, what);
        case EEXIST:
            return Error::make(ErrorCode::already_exists, what, errno);
        case EACCES:
        case EPERM:
        case EROFS:
            return Error::make(ErrorCode::permission_denied, what + ": " + std::strerror(errno), errno);
        case ENOSPC:
        case EDQUOT:
            return Error::make(ErrorCode::too_large, what + " (no space left)", errno);
        case EISDIR:
            return Error::make(ErrorCode::is_a_directory, what, errno);
        case ENOTEMPTY:
            return Error::make(ErrorCode::conflict, what + " (directory not empty)", errno);
        case EXDEV:
            return Error::make(ErrorCode::conflict, what + " (cross-device rename not supported)", errno);
        case ELOOP:
            return Error::make(ErrorCode::permission_denied, what + " (symlink loop)", errno);
        default:
            return Error::make(ErrorCode::io_error, what + ": " + std::strerror(errno), errno);
    }
}

struct ScopedDir {
    DIR* dir = nullptr;
    ~ScopedDir() {
        if (dir != nullptr) ::closedir(dir);
    }
};

bool text_candidate(std::string_view mime, std::uint64_t size, std::uint64_t limit) {
    return size <= limit && is_text_mime(mime);
}

} // namespace

FileService::FileService(PathSandbox& sandbox) : sandbox_(sandbox) {}
FileService::FileService(PathSandbox& sandbox, FileServiceOptions options)
    : sandbox_(sandbox), options_(options) {}

Error FileService::entry_from_stat(const std::string& name, const std::string& logical_path,
                                   const struct stat& info, FileEntry& entry) const {
    entry.name = name;
    entry.path = logical_path;
    entry.is_dir = S_ISDIR(info.st_mode);
    entry.is_file = S_ISREG(info.st_mode);
    entry.is_symlink = S_ISLNK(info.st_mode);
    entry.hidden = !name.empty() && name.front() == '.';
    entry.size = static_cast<std::uint64_t>(S_ISREG(info.st_mode) ? info.st_size : 0);
    entry.mtime = static_cast<std::int64_t>(info.st_mtime);
    entry.mode = static_cast<std::uint32_t>(info.st_mode & 07777);
    entry.uid = static_cast<std::uint32_t>(info.st_uid);
    entry.gid = static_cast<std::uint32_t>(info.st_gid);
    entry.mime = entry.is_dir ? "inode/directory" : mime_for_file_name(name);
    if (entry.is_file) {
        entry.size = static_cast<std::uint64_t>(info.st_size);
        entry.text_editable = text_candidate(entry.mime, entry.size, options_.max_text_edit_size);
    }
    if (entry.is_symlink) {
        UniqueFd parent;
        std::string final_name;
        if (sandbox_.open_parent(logical_path, parent, final_name).ok()) {
            char buffer[4096];
            const auto length = ::readlinkat(parent.get(), final_name.c_str(), buffer, sizeof(buffer) - 1);
            if (length > 0) entry.link_target.assign(buffer, static_cast<std::size_t>(length));
        }
    }
    return Error::none();
}

Error FileService::list(const ListOptions& options, ListResult& result) const {
    std::string normalized;
    std::string message;
    if (!PathSandbox::normalize(options.path, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }

    UniqueFd dir_fd;
    Error error = sandbox_.open_dir(normalized, dir_fd);
    if (!error.ok()) return error;

    // Transfer ownership of the descriptor to fdopendir.
    ScopedDir dir;
    dir.dir = ::fdopendir(dir_fd.release());
    if (dir.dir == nullptr) {
        return Error::make(ErrorCode::io_error, "cannot read directory: " + normalized);
    }

    result.path = normalized;
    result.entries.clear();
    result.truncated = false;

    const int page_size = std::clamp(options.page_size, 1, 1000);
    const int page = std::max(options.page, 1);
    std::uint64_t seen = 0;

    errno = 0;
    while (struct dirent* item = ::readdir(dir.dir)) {
        const std::string name = item->d_name;
        if (is_reserved_dir_name(name)) continue;
        if (!options.show_hidden && !name.empty() && name.front() == '.') continue;
        if (result.entries.size() >= options_.max_list_entries) {
            result.truncated = true;
            break;
        }
        const std::string logical = join_path(normalized, name);
        struct stat info {};
        if (::fstatat(dirfd(dir.dir), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
            continue; // Entry vanished or is unreadable: skip instead of failing the page.
        }
        FileEntry entry;
        error = entry_from_stat(name, logical, info, entry);
        if (!error.ok()) continue;
        if (options.dirs_only && !entry.is_dir) continue;
        result.entries.push_back(std::move(entry));
        ++seen;
    }
    if (errno != 0 && !result.truncated) {
        return Error::make(ErrorCode::io_error, "directory read failed: " + normalized);
    }

    const std::string sort = to_lower_ascii(options.sort);
    const bool desc = options.desc;
    auto compare = [&sort, desc](const FileEntry& a, const FileEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir; // directories first
        int order = 0;
        if (sort == "size") {
            order = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
        } else if (sort == "mtime") {
            order = a.mtime < b.mtime ? -1 : (a.mtime > b.mtime ? 1 : 0);
        } else if (sort == "type") {
            const auto ext = [](const std::string& name) {
                const auto dot = name.rfind('.');
                return dot == std::string::npos ? std::string() : to_lower_ascii(name.substr(dot));
            };
            order = ext(a.name).compare(ext(b.name));
        }
        if (order == 0) order = a.name.compare(b.name);
        return desc ? order > 0 : order < 0;
    };
    std::stable_sort(result.entries.begin(), result.entries.end(), compare);

    result.page = page;
    result.page_size = page_size;
    // `seen` counts every scanned entry after the hidden-file filter.
    result.total = result.entries.size();
    const std::size_t offset = static_cast<std::size_t>(page - 1) * static_cast<std::size_t>(page_size);
    if (offset >= result.entries.size()) {
        result.entries.clear();
    } else {
        const std::size_t end = std::min<std::size_t>(result.entries.size(), offset + page_size);
        std::vector<FileEntry> page_entries(result.entries.begin() + static_cast<std::ptrdiff_t>(offset),
                                            result.entries.begin() + static_cast<std::ptrdiff_t>(end));
        result.entries.swap(page_entries);
    }
    return Error::none();
}

Error FileService::stat(std::string_view path, FileEntry& entry) const {
    std::string normalized;
    std::string message;
    if (!PathSandbox::normalize(path, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }
    struct stat info {};
    Error error = sandbox_.stat(normalized, info);
    if (!error.ok()) return error;
    return entry_from_stat(base_name(normalized), normalized, info, entry);
}

Error FileService::mkdir(std::string_view path) const {
    std::string normalized;
    std::string message;
    if (!PathSandbox::normalize(path, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }
    if (normalized == "/") {
        return Error::make(ErrorCode::already_exists, "directory already exists: /");
    }
    UniqueFd dir;
    std::string name;
    Error error = sandbox_.open_parent(normalized, dir, name);
    if (!error.ok()) return error;
    if (::mkdirat(dir.get(), name.c_str(), 0755) != 0) {
        return errno_error("cannot create directory " + normalized);
    }
    return Error::none();
}

Error FileService::rename(std::string_view from, std::string_view to, bool overwrite) const {
    std::string normalized_from;
    std::string normalized_to;
    std::string message;
    if (!PathSandbox::normalize(from, normalized_from, message)) {
        return Error::make(ErrorCode::invalid_argument, "from: " + message);
    }
    if (!PathSandbox::normalize(to, normalized_to, message)) {
        return Error::make(ErrorCode::invalid_argument, "to: " + message);
    }
    if (normalized_from == "/" || normalized_to == "/") {
        return Error::make(ErrorCode::permission_denied, "cannot rename the sandbox root");
    }
    UniqueFd from_dir;
    UniqueFd to_dir;
    std::string from_name;
    std::string to_name;
    Error error = sandbox_.open_parent(normalized_from, from_dir, from_name);
    if (!error.ok()) return error;
    error = sandbox_.open_parent(normalized_to, to_dir, to_name);
    if (!error.ok()) return error;

    if (overwrite) {
        if (::renameat(from_dir.get(), from_name.c_str(), to_dir.get(), to_name.c_str()) != 0) {
            return errno_error("cannot rename " + normalized_from + " -> " + normalized_to);
        }
        return Error::none();
    }
#if defined(RENAME_NOREPLACE)
    if (::renameat2(from_dir.get(), from_name.c_str(), to_dir.get(), to_name.c_str(), RENAME_NOREPLACE) == 0) {
        return Error::none();
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return errno_error("cannot rename " + normalized_from + " -> " + normalized_to);
    }
    errno = 0;
#endif
    struct stat existing {};
    if (::fstatat(to_dir.get(), to_name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        return Error::make(ErrorCode::already_exists, "destination exists: " + normalized_to);
    }
    if (::renameat(from_dir.get(), from_name.c_str(), to_dir.get(), to_name.c_str()) != 0) {
        return errno_error("cannot rename " + normalized_from + " -> " + normalized_to);
    }
    return Error::none();
}

Error FileService::remove_tree(int dir_fd, const std::string& name, const std::string& logical_path, int depth,
                               std::uint32_t& files, std::uint32_t& dirs) const {
    if (depth > options_.max_delete_depth) {
        return Error::make(ErrorCode::limit_exceeded, "delete depth limit exceeded at " + logical_path);
    }
    UniqueFd child;
    const int fd = ::openat(dir_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOTDIR || errno == ELOOP) {
            if (::unlinkat(dir_fd, name.c_str(), 0) != 0) {
                return errno_error("cannot remove " + logical_path);
            }
            ++files;
            return Error::none();
        }
        return errno_error("cannot open directory for removal: " + logical_path);
    }
    child.reset(fd);

    ScopedDir dir;
    dir.dir = ::fdopendir(duplicate_fd(child.get()));
    if (dir.dir == nullptr) {
        return Error::make(ErrorCode::io_error, "cannot read directory: " + logical_path);
    }
    errno = 0;
    while (struct dirent* item = ::readdir(dir.dir)) {
        const std::string entry_name = item->d_name;
        if (is_reserved_dir_name(entry_name)) continue;
        if (files + dirs >= options_.max_delete_entries) {
            return Error::make(ErrorCode::limit_exceeded, "delete entry budget exhausted at " + logical_path);
        }
        const std::string entry_path = join_path(logical_path, entry_name);
        struct stat info {};
        if (::fstatat(child.get(), entry_name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
            return errno_error("cannot stat " + entry_path);
        }
        if (S_ISDIR(info.st_mode)) {
            Error error = remove_tree(child.get(), entry_name, entry_path, depth + 1, files, dirs);
            if (!error.ok()) return error;
        } else {
            if (::unlinkat(child.get(), entry_name.c_str(), 0) != 0) {
                return errno_error("cannot remove " + entry_path);
            }
            ++files;
        }
    }
    if (errno != 0) {
        return Error::make(ErrorCode::io_error, "directory read failed during delete: " + logical_path);
    }
    ::closedir(dir.dir);
    dir.dir = nullptr;
    if (::unlinkat(dir_fd, name.c_str(), AT_REMOVEDIR) != 0) {
        return errno_error("cannot remove directory " + logical_path);
    }
    ++dirs;
    return Error::none();
}

Error FileService::remove(const std::vector<std::string>& paths, RemoveResult& result) const {
    if (paths.empty()) {
        return Error::make(ErrorCode::invalid_argument, "no paths supplied");
    }
    if (paths.size() > 256) {
        return Error::make(ErrorCode::limit_exceeded, "too many paths in one delete request (max 256)");
    }
    result = RemoveResult{};
    for (const auto& path : paths) {
        std::string normalized;
        std::string message;
        if (!PathSandbox::normalize(path, normalized, message)) {
            return Error::make(ErrorCode::invalid_argument, message);
        }
        if (normalized == "/") {
            return Error::make(ErrorCode::permission_denied, "cannot delete the sandbox root");
        }
        UniqueFd dir;
        std::string name;
        Error error = sandbox_.open_parent(normalized, dir, name);
        if (!error.ok()) return error;
        struct stat info {};
        if (::fstatat(dir.get(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
            return errno_error("cannot stat " + normalized);
        }
        if (S_ISDIR(info.st_mode)) {
            error = remove_tree(dir.get(), name, normalized, 0, result.removed_files, result.removed_dirs);
        } else {
            if (::unlinkat(dir.get(), name.c_str(), 0) != 0) {
                error = errno_error("cannot remove " + normalized);
            } else {
                ++result.removed_files;
            }
        }
        if (!error.ok()) return error;
    }
    return Error::none();
}

Error FileService::read_text(std::string_view path, std::string& content, FileEntry& info) const {
    UniqueFd fd;
    Error error = sandbox_.open_file(path, O_RDONLY, fd);
    if (!error.ok()) return error;
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        return errno_error("cannot stat file");
    }
    if (S_ISDIR(st.st_mode)) {
        return Error::make(ErrorCode::is_a_directory, "path is a directory");
    }
    if (!S_ISREG(st.st_mode)) {
        return Error::make(ErrorCode::unsupported, "only regular files can be read as text");
    }
    const auto size = static_cast<std::uint64_t>(st.st_size);
    if (size > options_.max_text_edit_size) {
        return Error::make(ErrorCode::too_large,
                           "file exceeds text edit limit (" + std::to_string(options_.max_text_edit_size) + " bytes)");
    }
    std::string buffer;
    buffer.resize(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        const auto chunk = ::pread(fd.get(), buffer.data() + offset, buffer.size() - offset,
                                   static_cast<off_t>(offset));
        if (chunk < 0) {
            if (errno == EINTR) continue;
            return errno_error("cannot read file");
        }
        if (chunk == 0) break;
        offset += static_cast<std::size_t>(chunk);
    }
    buffer.resize(offset);

    const auto mime = mime_for_file_name(base_name(path));
    if (!is_text_mime(mime) && !looks_like_utf8_text(buffer.substr(0, std::min<std::size_t>(buffer.size(), 8192)))) {
        return Error::make(ErrorCode::unsupported, "file does not look like UTF-8 text");
    }
    if (!buffer.empty() && !looks_like_utf8_text(buffer.substr(0, std::min<std::size_t>(buffer.size(), 65536)))) {
        return Error::make(ErrorCode::unsupported, "file does not look like UTF-8 text");
    }
    content = std::move(buffer);
    std::string normalized;
    std::string message;
    PathSandbox::normalize(path, normalized, message);
    FileEntry entry;
    error = entry_from_stat(base_name(normalized), normalized, st, entry);
    if (!error.ok()) return error;
    entry.size = static_cast<std::uint64_t>(st.st_size);
    info = std::move(entry);
    return Error::none();
}

Error FileService::write_text(std::string_view path, std::string_view content, bool create_if_missing,
                              FileEntry& info) const {
    if (content.size() > options_.max_text_edit_size) {
        return Error::make(ErrorCode::too_large, "content exceeds text edit limit");
    }
    std::string normalized;
    std::string message;
    if (!PathSandbox::normalize(path, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }
    if (normalized == "/") {
        return Error::make(ErrorCode::permission_denied, "cannot write the sandbox root");
    }
    UniqueFd dir;
    std::string name;
    Error error = sandbox_.open_parent(normalized, dir, name);
    if (!error.ok()) return error;

    std::uint32_t final_mode = 0644;
    struct stat existing {};
    bool exists = ::fstatat(dir.get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0;
    if (exists) {
        if (S_ISDIR(existing.st_mode)) {
            return Error::make(ErrorCode::is_a_directory, "path is a directory: " + normalized);
        }
        if (S_ISLNK(existing.st_mode)) {
            return Error::make(ErrorCode::permission_denied, "refusing to write through a symbolic link: " + normalized);
        }
        final_mode = static_cast<std::uint32_t>(existing.st_mode & 07777);
    } else if (!create_if_missing) {
        return Error::make(ErrorCode::not_found, "file not found: " + normalized);
    }

    // Atomic save: temp file in the same directory, fsync, rename over target.
    UniqueFd temp;
    std::string temp_name;
    for (int attempt = 0; attempt < 8 && !temp.valid(); ++attempt) {
        temp_name = ".edgedesk-save-" + random_hex(8) + ".tmp";
        const int fd = ::openat(dir.get(), temp_name.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            temp.reset(fd);
            break;
        }
        if (errno != EEXIST) return errno_error("cannot create temporary file for " + normalized);
    }
    if (!temp.valid()) {
        return Error::make(ErrorCode::io_error, "cannot create temporary file for " + normalized);
    }

    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto chunk = ::write(temp.get(), content.data() + offset, content.size() - offset);
        if (chunk < 0) {
            if (errno == EINTR) continue;
            ::unlinkat(dir.get(), temp_name.c_str(), 0);
            return errno_error("cannot write " + normalized);
        }
        offset += static_cast<std::size_t>(chunk);
    }
    if (::fchmod(temp.get(), static_cast<mode_t>(final_mode)) != 0) {
        ::unlinkat(dir.get(), temp_name.c_str(), 0);
        return errno_error("cannot set permissions on temporary file");
    }
    if (::fsync(temp.get()) != 0) {
        ::unlinkat(dir.get(), temp_name.c_str(), 0);
        return errno_error("cannot fsync temporary file");
    }
    if (::renameat(dir.get(), temp_name.c_str(), dir.get(), name.c_str()) != 0) {
        const auto rename_error = errno_error("cannot replace " + normalized);
        ::unlinkat(dir.get(), temp_name.c_str(), 0);
        return rename_error;
    }
    const int dir_dup = duplicate_fd(dir.get());
    if (dir_dup >= 0) {
        ::fsync(dir_dup); // best effort: makes the rename durable
        ::close(dir_dup);
    }
    struct stat st {};
    if (::fstatat(dir.get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno_error("cannot stat after save");
    }
    return entry_from_stat(base_name(normalized), normalized, st, info);
}

Error FileService::open_for_read(std::string_view path, ReadHandle& handle) const {
    std::string normalized;
    std::string message;
    if (!PathSandbox::normalize(path, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }
    UniqueFd fd;
    Error error = sandbox_.open_file(normalized, O_RDONLY, fd);
    if (!error.ok()) return error;
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) return errno_error("cannot stat " + normalized);
    if (S_ISDIR(st.st_mode)) {
        return Error::make(ErrorCode::is_a_directory, "use the listing API for directories");
    }
    if (!S_ISREG(st.st_mode)) {
        return Error::make(ErrorCode::unsupported, "only regular files can be downloaded");
    }
    handle.fd = std::move(fd);
    handle.size = static_cast<std::uint64_t>(st.st_size);
    handle.mtime = static_cast<std::int64_t>(st.st_mtime);
    handle.mode = static_cast<std::uint32_t>(st.st_mode & 07777);
    handle.mime = mime_for_file_name(base_name(normalized));
    return Error::none();
}

FileService::Upload::~Upload() {
    if (!finished_) abort();
}

Error FileService::begin_upload(std::string_view path, Upload& upload) const {
    std::string normalized;
    std::string message;
    if (!PathSandbox::normalize(path, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }
    if (normalized == "/") {
        return Error::make(ErrorCode::permission_denied, "cannot upload over the sandbox root");
    }
    UniqueFd dir;
    std::string name;
    Error error = sandbox_.open_parent(normalized, dir, name);
    if (!error.ok()) return error;

    struct stat existing {};
    if (::fstatat(dir.get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISDIR(existing.st_mode)) {
            return Error::make(ErrorCode::is_a_directory, "destination is a directory: " + normalized);
        }
        if (S_ISLNK(existing.st_mode)) {
            return Error::make(ErrorCode::permission_denied,
                               "refusing to upload through a symbolic link: " + normalized);
        }
        upload.final_mode_ = static_cast<std::uint32_t>(existing.st_mode & 07777);
    } else {
        upload.final_mode_ = 0644;
    }

    UniqueFd temp;
    std::string temp_name;
    for (int attempt = 0; attempt < 8 && !temp.valid(); ++attempt) {
        temp_name = ".edgedesk-upload-" + random_hex(8) + ".tmp";
        const int fd = ::openat(dir.get(), temp_name.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            temp.reset(fd);
            break;
        }
        if (errno != EEXIST) return errno_error("cannot create temporary upload file for " + normalized);
    }
    if (!temp.valid()) {
        return Error::make(ErrorCode::io_error, "cannot create temporary upload file for " + normalized);
    }

    upload.dir_fd_ = std::move(dir);
    upload.fd_ = std::move(temp);
    upload.temp_name_ = std::move(temp_name);
    upload.final_name_ = name;
    upload.destination_ = normalized;
    upload.written_ = 0;
    upload.limit_ = options_.max_upload_size;
    upload.text_limit_ = options_.max_text_edit_size;
    upload.finished_ = false;
    return Error::none();
}

Error FileService::Upload::append(const char* data, std::size_t length) {
    if (!fd_.valid()) {
        return Error::make(ErrorCode::conflict, "upload is not active");
    }
    if (written_ + length > limit_) {
        return Error::make(ErrorCode::too_large,
                           "upload exceeds limit of " + std::to_string(limit_) + " bytes");
    }
    std::size_t offset = 0;
    while (offset < length) {
        const auto chunk = ::write(fd_.get(), data + offset, length - offset);
        if (chunk < 0) {
            if (errno == EINTR) continue;
            return errno_error("cannot write upload data");
        }
        offset += static_cast<std::size_t>(chunk);
    }
    written_ += length;
    return Error::none();
}

Error FileService::Upload::finish(FileEntry& info) {
    if (!fd_.valid()) {
        return Error::make(ErrorCode::conflict, "upload is not active");
    }
    if (::fchmod(fd_.get(), static_cast<mode_t>(final_mode_)) != 0) {
        abort();
        return errno_error("cannot set upload permissions");
    }
    if (::fsync(fd_.get()) != 0) {
        abort();
        return errno_error("cannot fsync upload");
    }
    fd_.reset();
    if (::renameat(dir_fd_.get(), temp_name_.c_str(), dir_fd_.get(), final_name_.c_str()) != 0) {
        const auto rename_error = errno_error("cannot finalize upload");
        ::unlinkat(dir_fd_.get(), temp_name_.c_str(), 0);
        return rename_error;
    }
    finished_ = true;
    const int dir_dup = duplicate_fd(dir_fd_.get());
    if (dir_dup >= 0) {
        ::fsync(dir_dup);
        ::close(dir_dup);
    }
    struct stat st {};
    if (::fstatat(dir_fd_.get(), final_name_.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno_error("cannot stat uploaded file");
    }
    // entry_from_stat is a FileService member; the caller re-stats via its own
    // service instance, so only basic data is returned here.
    info.name = final_name_;
    info.path = destination_;
    info.is_file = S_ISREG(st.st_mode);
    info.is_dir = S_ISDIR(st.st_mode);
    info.is_symlink = S_ISLNK(st.st_mode);
    info.hidden = !final_name_.empty() && final_name_.front() == '.';
    info.size = static_cast<std::uint64_t>(st.st_size);
    info.mtime = static_cast<std::int64_t>(st.st_mtime);
    info.mode = static_cast<std::uint32_t>(st.st_mode & 07777);
    info.uid = static_cast<std::uint32_t>(st.st_uid);
    info.gid = static_cast<std::uint32_t>(st.st_gid);
    info.mime = mime_for_file_name(final_name_);
    info.text_editable = text_candidate(info.mime, info.size, text_limit_);
    return Error::none();
}

void FileService::Upload::abort() {
    fd_.reset();
    if (!temp_name_.empty() && dir_fd_.valid()) {
        ::unlinkat(dir_fd_.get(), temp_name_.c_str(), 0);
    }
    temp_name_.clear();
    finished_ = true;
}

} // namespace edgedesk
