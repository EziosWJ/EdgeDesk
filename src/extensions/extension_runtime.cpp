#include "extensions/extension_runtime.hpp"

#include "core/util.hpp"
#include "core/utf8.hpp"
#include "extensions/package_manager.hpp"
#include "file/file_lock.hpp"
#include "file/path_sandbox.hpp"
#include "terminal/terminal_manager.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace edgedesk::extensions {
namespace {

bool valid_instance_id(std::string_view value) {
    if (value.size() < 8 || value.size() > 128) return false;
    for (const char ch : value) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '-';
        if (!ok) return false;
    }
    return true;
}

bool entry_less(const FileEntry& left, const FileEntry& right) {
    if (left.is_dir != right.is_dir) return left.is_dir;
    return left.name < right.name;
}

bool cursor_before(const FileEntry& entry, bool last_is_dir, const std::string& last_name) {
    if (entry.is_dir != last_is_dir) {
        // Directories sort before files. If the cursor was a directory, every
        // file is after it; if it was a file, every directory is before it.
        return !last_is_dir;
    }
    return entry.name <= last_name;
}

} // namespace

ExtensionRuntime::ExtensionRuntime(ExtensionStore& store, PackageManager& packages, FileService& files,
                                   TerminalManager* terminals)
    : store_(store), packages_(packages), files_(files), terminals_(terminals), cursor_key_(random_bytes(32)) {
    // Extension transfers keep a tiny journal so a crashed process cannot
    // leave .edgedesk-upload-*.tmp files behind after restart (ADR-0013).
    journal_directory_ = packages_.temporary_directory() + "/transfers";
    std::string journal_error;
    if (!ensure_private_directory(journal_directory_, journal_error)) {
        journal_directory_.clear();
    } else {
        recover_transfer_journal();
    }
}

ExtensionRuntime::UploadTask::~UploadTask() {
    upload.abort();
    if (!journal_path.empty()) ::unlink(journal_path.c_str());
}

Error ExtensionRuntime::write_transfer_journal(UploadTask& task, std::string_view transfer_id) const {
    if (journal_directory_.empty()) return Error::none();
    const std::string& directory = task.upload.directory_path();
    const std::string& name = task.upload.temporary_name();
    if (directory.empty() || name.empty()) return Error::none();
    const std::string journal = journal_directory_ + "/" + std::string(transfer_id) + ".journal";
    const std::string content = directory + "\n" + name + "\n";
    UniqueFd fd(::open(journal.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!fd.valid()) {
        return Error::make(ErrorCode::io_error, "cannot create transfer journal");
    }
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto written = ::write(fd.get(), content.data() + offset, content.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            ::unlink(journal.c_str());
            return Error::make(ErrorCode::io_error, "cannot write transfer journal");
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(fd.get()) != 0) {
        ::unlink(journal.c_str());
        return Error::make(ErrorCode::io_error, "cannot flush transfer journal");
    }
    task.journal_path = journal;
    return Error::none();
}

void ExtensionRuntime::recover_transfer_journal() {
    UniqueFd dir(::open(journal_directory_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!dir.valid()) return;
    DIR* raw = ::fdopendir(dir.release());
    if (raw == nullptr) return;
    std::vector<std::string> journals;
    for (;;) {
        errno = 0;
        struct dirent* entry = ::readdir(raw);
        if (entry == nullptr) break;
        const std::string name = entry->d_name;
        if (name.size() > 8 && name.substr(name.size() - 8) == ".journal") journals.push_back(name);
    }
    ::closedir(raw);
    for (const auto& name : journals) {
        const std::string journal = journal_directory_ + "/" + name;
        UniqueFd fd(::open(journal.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (!fd.valid()) continue;
        char buffer[8192];
        const auto length = ::read(fd.get(), buffer, sizeof(buffer) - 1);
        if (length > 0) {
            buffer[length] = '\0';
            std::string_view text(buffer, static_cast<std::size_t>(length));
            const auto first = text.find('\n');
            const auto second = first == std::string_view::npos ? std::string_view::npos : text.find('\n', first + 1);
            if (first != std::string_view::npos && second != std::string_view::npos) {
                const std::string directory(text.substr(0, first));
                const std::string temp_name(text.substr(first + 1, second - first - 1));
                const bool safe_name = temp_name.rfind(".edgedesk-upload-", 0) == 0 &&
                                       temp_name.size() > 4 &&
                                       temp_name.substr(temp_name.size() - 4) == ".tmp" &&
                                       temp_name.find('/') == std::string::npos;
                const bool safe_directory = directory.size() > 1 && directory.front() == '/' &&
                                            directory.find("/../") == std::string::npos &&
                                            directory.substr(directory.size() - 3) != "/..";
                if (safe_name && safe_directory) {
                    ::unlink((directory + "/" + temp_name).c_str());
                }
            }
        }
        ::unlink(journal.c_str());
    }
}

ExtensionRuntime::~ExtensionRuntime() = default;

bool ExtensionRuntime::has_permission(const RuntimeSession& session, Permission permission) {
    const std::string name = permission_name(permission);
    return std::find(session.permissions.begin(), session.permissions.end(), name) !=
           session.permissions.end();
}

Error ExtensionRuntime::require_permission(const RuntimeSession& session, Permission permission) {
    if (!has_permission(session, permission)) {
        return Error::make(ErrorCode::permission_denied,
                           std::string("permission is not granted: ") + permission_name(permission));
    }
    return Error::none();
}

Error ExtensionRuntime::create_session(std::string_view app_id, std::string_view instance_id,
                                       RuntimeSession& out) {
    out = {};
    if (!valid_instance_id(instance_id)) {
        return Error::make(ErrorCode::invalid_argument, "instance id must be a UUID-like identifier");
    }
    AppRecord app;
    bool found = false;
    Error error = store_.get_app(app_id, app, found);
    if (!error.ok()) return error;
    if (!found || app.status != app_status::enabled) {
        return Error::make(ErrorCode::permission_denied, "application is not enabled");
    }
    if (app.active_revision_id.empty()) {
        return Error::make(ErrorCode::not_found, "application has no active revision");
    }
    RevisionRecord revision;
    bool revision_found = false;
    error = store_.get_revision(app.active_revision_id, revision, revision_found);
    if (!error.ok()) return error;
    if (!revision_found || revision.status != revision_status::active ||
        revision.integrity != integrity_state::ok) {
        return Error::make(ErrorCode::permission_denied, "application revision is not runnable");
    }
    Manifest manifest;
    error = packages_.active_manifest(app_id, manifest);
    if (!error.ok()) return error;
    if (!manifest.host_api_compatible()) {
        return Error::make(ErrorCode::unsupported, "application is not compatible with this host API");
    }
    out.app_id = app.app_id;
    out.instance_id = std::string(instance_id);
    out.revision_id = app.active_revision_id;
    out.version = app.active_version;
    out.permissions = app.granted_permissions;
    if (std::find(out.permissions.begin(), out.permissions.end(), permission_name(Permission::network)) !=
        out.permissions.end()) {
        std::vector<std::string> approved;
        error = store_.list_network_origins(app_id, approved);
        if (!error.ok()) return error;
        out.origins = intersect_origins(origin_strings(manifest.origins), approved);
    }
    out.created_at = now_unix_seconds();

    std::lock_guard<std::mutex> lock(mutex_);
    for (int attempt = 0; attempt < 8; ++attempt) {
        out.token = random_hex(32);
        if (sessions_.find(out.token) == sessions_.end()) {
            sessions_[out.token] = out;
            return Error::none();
        }
    }
    return Error::make(ErrorCode::unavailable, "cannot allocate a runtime session token");
}

bool ExtensionRuntime::find_session_locked(std::string_view token, RuntimeSession& out) const {
    const auto it = sessions_.find(std::string(token));
    if (it == sessions_.end()) return false;
    out = it->second;
    return true;
}

Error ExtensionRuntime::validate(std::string_view token, RuntimeSession& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!find_session_locked(token, out)) {
        return Error::make(ErrorCode::permission_denied, "runtime session is invalid");
    }
    if (now_unix_seconds() - out.created_at > kRuntimeSessionTtlSeconds) {
        sessions_.erase(out.token);
        return Error::make(ErrorCode::permission_denied, "runtime session expired");
    }
    AppRecord app;
    bool found = false;
    Error error = store_.get_app(out.app_id, app, found);
    if (!error.ok() || !found || app.status != app_status::enabled ||
        app.active_revision_id != out.revision_id) {
        sessions_.erase(out.token);
        return Error::make(ErrorCode::permission_denied,
                           "application was disabled, upgraded or uninstalled");
    }
    RevisionRecord revision;
    bool revision_found = false;
    error = store_.get_revision(out.revision_id, revision, revision_found);
    if (!error.ok() || !revision_found || revision.status != revision_status::active ||
        revision.integrity != integrity_state::ok) {
        sessions_.erase(out.token);
        return Error::make(ErrorCode::permission_denied, "application revision is no longer runnable");
    }
    return Error::none();
}

std::vector<std::string> ExtensionRuntime::erase_session_locked(std::string_view token) {
    std::vector<std::string> closing;
    const auto it = sessions_.find(std::string(token));
    if (it != sessions_.end()) sessions_.erase(it);
    const auto terminals = terminal_sessions_.find(std::string(token));
    if (terminals != terminal_sessions_.end()) {
        closing.assign(terminals->second.begin(), terminals->second.end());
        terminal_sessions_.erase(terminals);
    }
    for (auto upload = uploads_.begin(); upload != uploads_.end();) {
        if (upload->second.token == token) upload = uploads_.erase(upload);
        else ++upload;
    }
    for (auto download = downloads_.begin(); download != downloads_.end();) {
        if (download->second.token == token) download = downloads_.erase(download);
        else ++download;
    }
    return closing;
}

void ExtensionRuntime::close_terminals(const std::vector<std::string>& session_ids) {
    if (terminals_ == nullptr) return;
    for (const auto& session_id : session_ids) terminals_->request_close(session_id);
}

void ExtensionRuntime::track_terminal(std::string_view token, std::string_view session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.find(std::string(token)) == sessions_.end()) return;
    terminal_sessions_[std::string(token)].insert(std::string(session_id));
}

void ExtensionRuntime::untrack_terminal(std::string_view token, std::string_view session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = terminal_sessions_.find(std::string(token));
    if (it == terminal_sessions_.end()) return;
    it->second.erase(std::string(session_id));
    if (it->second.empty()) terminal_sessions_.erase(it);
}

void ExtensionRuntime::remove_session(std::string_view token) {
    std::vector<std::string> closing;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing = erase_session_locked(token);
    }
    close_terminals(closing);
}

void ExtensionRuntime::invalidate_app(std::string_view app_id) {
    std::vector<std::string> closing;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> tokens;
        for (const auto& session : sessions_) {
            if (session.second.app_id == app_id) tokens.push_back(session.first);
        }
        for (const auto& token : tokens) {
            const auto ids = erase_session_locked(token);
            closing.insert(closing.end(), ids.begin(), ids.end());
        }
        for (auto it = uploads_.begin(); it != uploads_.end();) {
            if (it->second.app_id == app_id) it = uploads_.erase(it);
            else ++it;
        }
        for (auto it = downloads_.begin(); it != downloads_.end();) {
            if (it->second.app_id == app_id) it = downloads_.erase(it);
            else ++it;
        }
    }
    close_terminals(closing);
}

void ExtensionRuntime::invalidate_revision(std::string_view revision_id) {
    std::vector<std::string> closing;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> tokens;
        for (const auto& session : sessions_) {
            if (session.second.revision_id == revision_id) tokens.push_back(session.first);
        }
        for (const auto& token : tokens) {
            const auto ids = erase_session_locked(token);
            closing.insert(closing.end(), ids.begin(), ids.end());
        }
    }
    close_terminals(closing);
}

void ExtensionRuntime::sweep(std::int64_t now_ms) {
    std::vector<std::string> closing;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::int64_t now_seconds = now_unix_seconds();
        std::vector<std::string> expired;
        for (const auto& session : sessions_) {
            if (now_seconds - session.second.created_at > kRuntimeSessionTtlSeconds) {
                expired.push_back(session.first);
            }
        }
        for (const auto& token : expired) {
            const auto ids = erase_session_locked(token);
            closing.insert(closing.end(), ids.begin(), ids.end());
        }
        for (auto it = uploads_.begin(); it != uploads_.end();) {
            if (now_ms - it->second.last_activity_ms > kTransferIdleTimeoutMs) it = uploads_.erase(it);
            else ++it;
        }
        for (auto it = downloads_.begin(); it != downloads_.end();) {
            if (now_ms - it->second.last_activity_ms > kTransferIdleTimeoutMs) it = downloads_.erase(it);
            else ++it;
        }
    }
    close_terminals(closing);
}

std::string ExtensionRuntime::encode_cursor(std::string_view app_id, std::string_view kind,
                                            const std::string& payload) const {
    const std::string message = std::string(app_id) + "\x1f" + std::string(kind) + "\x1f" + payload;
    const std::string signature = hmac_sha256_hex(cursor_key_, message);
    return base64url_encode(payload) + "." + signature;
}

bool ExtensionRuntime::decode_cursor(std::string_view app_id, std::string_view kind,
                                     std::string_view cursor, std::string& payload) const {
    payload.clear();
    if (cursor.empty() || cursor.size() > 16384) return false;
    const auto dot = cursor.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string decoded;
    if (!base64_decode(cursor.substr(0, dot), decoded)) return false;
    const std::string message = std::string(app_id) + "\x1f" + std::string(kind) + "\x1f" + decoded;
    const std::string expected = hmac_sha256_hex(cursor_key_, message);
    if (!constant_time_equal(expected, cursor.substr(dot + 1))) return false;
    payload = std::move(decoded);
    return true;
}

Error ExtensionRuntime::cursor_stale() {
    return Error::make(ErrorCode::conflict, "CURSOR_STALE: directory contents changed");
}

Error ExtensionRuntime::list_directory(const RuntimeSession& session, std::string_view path,
                                       std::string_view cursor, int limit, ListPage& out) const {
    out = {};
    Error permission = require_permission(session, Permission::files_manage);
    if (!permission.ok()) return permission;
    std::string normalized;
    std::string message;
    if (!PathSandbox::normalize(path, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }
    UniqueFd dir_fd;
    Error error = files_.sandbox().open_dir(normalized, dir_fd);
    if (!error.ok()) return error;
    struct stat dir_info {};
    if (::fstat(dir_fd.get(), &dir_info) != 0) {
        return Error::make(ErrorCode::io_error, "cannot stat directory");
    }
    const int clamped = limit < 1 ? kListPageDefault : (limit > kListPageMax ? kListPageMax : limit);

    bool last_is_dir = true;
    std::string last_name;
    if (!cursor.empty()) {
        std::string payload;
        if (!decode_cursor(session.app_id, "files", cursor, payload)) {
            return cursor_stale();
        }
        const auto parsed = nlohmann::json::parse(payload, nullptr, false);
        if (!parsed.is_object() || !parsed.contains("n") || !parsed.contains("d") ||
            !parsed.contains("dev") || !parsed.contains("ino") || !parsed.contains("mtime") ||
            !parsed.contains("nsec")) {
            return cursor_stale();
        }
        const auto dev = parsed.value("dev", static_cast<std::uint64_t>(0));
        const auto ino = parsed.value("ino", static_cast<std::uint64_t>(0));
        const auto mtime = parsed.value("mtime", static_cast<std::int64_t>(0));
        const auto nsec = parsed.value("nsec", static_cast<std::int64_t>(0));
        if (dev != static_cast<std::uint64_t>(dir_info.st_dev) ||
            ino != static_cast<std::uint64_t>(dir_info.st_ino) ||
            mtime != static_cast<std::int64_t>(dir_info.st_mtim.tv_sec) ||
            nsec != static_cast<std::int64_t>(dir_info.st_mtim.tv_nsec)) {
            return cursor_stale();
        }
        last_name = parsed.value("n", "");
        last_is_dir = parsed.value("d", true);
    }

    const int fd = dir_fd.release();
    DIR* raw = ::fdopendir(fd);
    if (raw == nullptr) {
        ::close(fd);
        return Error::make(ErrorCode::io_error, "cannot read directory");
    }
    struct DirCloser {
        DIR* dir;
        ~DirCloser() { ::closedir(dir); }
    } closer{raw};
    std::vector<FileEntry> entries;
    for (;;) {
        errno = 0;
        struct dirent* item = ::readdir(raw);
        if (item == nullptr) {
            if (errno != 0) return Error::make(ErrorCode::io_error, "directory read failed");
            break;
        }
        const std::string name = item->d_name;
        if (name == "." || name == "..") continue;
        if (!is_valid_utf8(name)) {
            return Error::make(ErrorCode::unsupported,
                               "directory contains a file name that is not valid UTF-8");
        }
        struct stat info {};
        if (::fstatat(::dirfd(raw), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) continue;
        FileEntry entry;
        entry.name = name;
        entry.path = normalized == "/" ? "/" + name : normalized + "/" + name;
        entry.is_dir = S_ISDIR(info.st_mode);
        entry.is_file = S_ISREG(info.st_mode);
        entry.is_symlink = S_ISLNK(info.st_mode);
        entry.hidden = !name.empty() && name.front() == '.';
        entry.size = entry.is_file ? static_cast<std::uint64_t>(info.st_size) : 0;
        entry.mtime = static_cast<std::int64_t>(info.st_mtime);
        entry.mode = static_cast<std::uint32_t>(info.st_mode & 07777);
        entry.uid = static_cast<std::uint32_t>(info.st_uid);
        entry.gid = static_cast<std::uint32_t>(info.st_gid);
        entry.mime = entry.is_dir ? "inode/directory" : mime_for_file_name(name);
        if (entry.is_symlink) {
            char target[4096];
            const auto length = ::readlinkat(::dirfd(raw), name.c_str(), target, sizeof(target) - 1);
            if (length > 0) entry.link_target.assign(target, static_cast<std::size_t>(length));
        }
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), entry_less);

    std::size_t start = 0;
    if (!cursor.empty()) {
        while (start < entries.size() && cursor_before(entries[start], last_is_dir, last_name)) ++start;
    }
    out.path = normalized;
    out.stale = false;
    const std::size_t end = std::min(entries.size(), start + static_cast<std::size_t>(clamped));
    out.entries.assign(entries.begin() + static_cast<std::ptrdiff_t>(start),
                       entries.begin() + static_cast<std::ptrdiff_t>(end));
    if (end < entries.size() && !out.entries.empty()) {
        nlohmann::json payload;
        payload["n"] = out.entries.back().name;
        payload["d"] = out.entries.back().is_dir;
        payload["dev"] = static_cast<std::uint64_t>(dir_info.st_dev);
        payload["ino"] = static_cast<std::uint64_t>(dir_info.st_ino);
        payload["mtime"] = static_cast<std::int64_t>(dir_info.st_mtim.tv_sec);
        payload["nsec"] = static_cast<std::int64_t>(dir_info.st_mtim.tv_nsec);
        out.next_cursor = encode_cursor(session.app_id, "files", payload.dump());
    }
    return Error::none();
}

Error ExtensionRuntime::transfer_owner_check(const RuntimeSession& session, const UploadTask& task) const {
    if (task.app_id != session.app_id || task.instance_id != session.instance_id ||
        task.token != session.token) {
        return Error::make(ErrorCode::permission_denied, "transfer does not belong to this runtime instance");
    }
    return Error::none();
}

Error ExtensionRuntime::transfer_owner_check(const RuntimeSession& session, const DownloadTask& task) const {
    if (task.app_id != session.app_id || task.instance_id != session.instance_id ||
        task.token != session.token) {
        return Error::make(ErrorCode::permission_denied, "transfer does not belong to this runtime instance");
    }
    return Error::none();
}

Error ExtensionRuntime::begin_upload(const RuntimeSession& session, std::string_view path,
                                     std::uint64_t expected_size, TransferInfo& out) {
    out = {};
    Error permission = require_permission(session, Permission::files_manage);
    if (!permission.ok()) return permission;
    if (expected_size > files_.options().max_upload_size) {
        return Error::make(ErrorCode::too_large, "expected size exceeds the configured upload limit");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    UploadTask task;
    task.token = session.token;
    task.app_id = session.app_id;
    task.instance_id = session.instance_id;
    task.path = std::string(path);
    task.expected_size = expected_size;
    task.last_activity_ms = now_unix_millis();
    Error error = files_.begin_upload(task.path, task.upload);
    if (!error.ok()) return error;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::string candidate = random_hex(16);
        if (uploads_.find(candidate) == uploads_.end()) {
            out.transfer_id = candidate;
            break;
        }
    }
    if (out.transfer_id.empty()) {
        task.upload.abort();
        return Error::make(ErrorCode::unavailable, "cannot allocate a transfer id");
    }
    error = write_transfer_journal(task, out.transfer_id);
    if (!error.ok()) {
        task.upload.abort();
        return error;
    }
    uploads_[out.transfer_id] = std::move(task);
    out.chunk_size = kTransferChunkBytes;
    out.total_size = expected_size;
    return Error::none();
}

Error ExtensionRuntime::append_upload(const RuntimeSession& session, std::string_view transfer_id,
                                      std::uint64_t offset, const char* data, std::size_t length,
                                      std::uint64_t& received) {
    received = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = uploads_.find(std::string(transfer_id));
    if (it == uploads_.end()) return Error::make(ErrorCode::not_found, "upload transfer not found");
    Error owner = transfer_owner_check(session, it->second);
    if (!owner.ok()) return owner;
    if (length == 0 || length > kTransferChunkBytes) {
        return Error::make(ErrorCode::invalid_argument, "chunk must contain 1..1048576 bytes");
    }
    if (offset != it->second.received) {
        return Error::make(ErrorCode::invalid_argument, "chunk offset is not the next sequential offset");
    }
    if (it->second.received + length > it->second.expected_size) {
        return Error::make(ErrorCode::invalid_argument, "chunk exceeds the declared transfer length");
    }
    Error error = it->second.upload.append(data, length);
    if (!error.ok()) {
        uploads_.erase(it);
        return error;
    }
    it->second.received += length;
    it->second.last_activity_ms = now_unix_millis();
    received = it->second.received;
    return Error::none();
}

Error ExtensionRuntime::finish_upload(const RuntimeSession& session, std::string_view transfer_id,
                                      std::uint64_t expected_size, FileEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = uploads_.find(std::string(transfer_id));
    if (it == uploads_.end()) return Error::make(ErrorCode::not_found, "upload transfer not found");
    Error owner = transfer_owner_check(session, it->second);
    if (!owner.ok()) return owner;
    if (it->second.received != expected_size || it->second.expected_size != expected_size) {
        uploads_.erase(it);
        return Error::make(ErrorCode::invalid_argument, "upload length does not match the declared size");
    }
    Error error = it->second.upload.finish(entry);
    uploads_.erase(it);
    return error;
}

Error ExtensionRuntime::cancel_upload(const RuntimeSession& session, std::string_view transfer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = uploads_.find(std::string(transfer_id));
    if (it == uploads_.end()) return Error::none();
    Error owner = transfer_owner_check(session, it->second);
    if (!owner.ok()) return owner;
    uploads_.erase(it);
    return Error::none();
}

Error ExtensionRuntime::begin_download(const RuntimeSession& session, std::string_view path,
                                       TransferInfo& out) {
    out = {};
    Error permission = require_permission(session, Permission::files_manage);
    if (!permission.ok()) return permission;
    std::lock_guard<std::mutex> lock(mutex_);
    DownloadTask task;
    task.token = session.token;
    task.app_id = session.app_id;
    task.instance_id = session.instance_id;
    task.path = std::string(path);
    task.last_activity_ms = now_unix_millis();
    Error error = files_.open_for_read(task.path, task.handle);
    if (!error.ok()) return error;
    for (int attempt = 0; attempt < 8; ++attempt) {
        out.transfer_id = random_hex(16);
        if (downloads_.find(out.transfer_id) == downloads_.end()) {
            out.chunk_size = kTransferChunkBytes;
            out.total_size = task.handle.size;
            downloads_[out.transfer_id] = std::move(task);
            return Error::none();
        }
    }
    return Error::make(ErrorCode::unavailable, "cannot allocate a transfer id");
}

Error ExtensionRuntime::read_download(const RuntimeSession& session, std::string_view transfer_id,
                                      std::uint64_t offset, std::string& data, bool& eof) {
    data.clear();
    eof = false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = downloads_.find(std::string(transfer_id));
    if (it == downloads_.end()) return Error::make(ErrorCode::not_found, "download transfer not found");
    Error owner = transfer_owner_check(session, it->second);
    if (!owner.ok()) return owner;
    if (offset != it->second.offset) {
        return Error::make(ErrorCode::invalid_argument, "download offset is not sequential");
    }
    const std::uint64_t total = it->second.handle.size;
    const std::uint64_t remaining = offset < total ? total - offset : 0;
    const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kTransferChunkBytes));
    if (chunk == 0) {
        eof = true;
        return Error::none();
    }
    data.resize(chunk);
    std::size_t filled = 0;
    while (filled < chunk) {
        const auto read = ::pread(it->second.handle.fd.get(), data.data() + filled, chunk - filled,
                                  static_cast<off_t>(offset + filled));
        if (read < 0) {
            if (errno == EINTR) continue;
            return Error::make(ErrorCode::io_error, "cannot read download transfer");
        }
        if (read == 0) break;
        filled += static_cast<std::size_t>(read);
    }
    data.resize(filled);
    if (filled == 0 && remaining > 0) {
        return Error::make(ErrorCode::io_error, "file shrank during download");
    }
    it->second.offset += filled;
    it->second.last_activity_ms = now_unix_millis();
    eof = it->second.offset >= total;
    return Error::none();
}

Error ExtensionRuntime::finish_download(const RuntimeSession& session, std::string_view transfer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = downloads_.find(std::string(transfer_id));
    if (it == downloads_.end()) return Error::make(ErrorCode::not_found, "download transfer not found");
    Error owner = transfer_owner_check(session, it->second);
    if (!owner.ok()) return owner;
    if (it->second.offset != it->second.handle.size) {
        return Error::make(ErrorCode::invalid_argument, "download was not fully read");
    }
    downloads_.erase(it);
    return Error::none();
}

Error ExtensionRuntime::cancel_download(const RuntimeSession& session, std::string_view transfer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = downloads_.find(std::string(transfer_id));
    if (it == downloads_.end()) return Error::none();
    Error owner = transfer_owner_check(session, it->second);
    if (!owner.ok()) return owner;
    downloads_.erase(it);
    return Error::none();
}

} // namespace edgedesk::extensions
