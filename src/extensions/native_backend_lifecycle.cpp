#include "extensions/native_backend_lifecycle.hpp"

#include "core/fd.hpp"
#include "core/utf8.hpp"
#include "extensions/elf_info.hpp"
#include "extensions/extension_store.hpp"
#include "extensions/native_backend_installer.hpp"
#include "extensions/package_manager.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Lifecycle helpers for optional native backends (SPEC-0011 section 6).
//
// Everything here is deliberately split from the Supervisor IPC: this file
// validates local preconditions, prepares the per-application run directory
// and produces the exact service configuration. The HTTP route drives the
// Supervisor asynchronously so the libhv event loop never blocks on a socket
// or a privileged operation.
namespace edgedesk::extensions {
namespace {

// Linux sun_path is 108 bytes including the trailing NUL, so a bindable path
// is at most 107 bytes.
constexpr std::size_t kMaxUnixSocketPathBytes = 107;
constexpr std::size_t kMaxServiceNameBytes = 128;
constexpr const char* kServiceNameSuffix = " backend";

std::string trim_ascii(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    const auto is_space = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
    };
    while (begin < end && is_space(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && is_space(static_cast<unsigned char>(value[end - 1]))) --end;
    return std::string(value.substr(begin, end - begin));
}

bool parse_numeric_id(std::string_view text, unsigned long& out) {
    if (text.empty() || text.size() > 10) return false;
    unsigned long value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + static_cast<unsigned long>(ch - '0');
    }
    out = value;
    return true;
}

struct ResolvedId {
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    std::string user_name;
    std::string group_name;
    std::vector<gid_t> groups;
};

bool contains_gid(const std::vector<gid_t>& groups, gid_t gid) {
    return std::find(groups.begin(), groups.end(), gid) != groups.end();
}

Error resolve_identity(const std::string& user, const std::string& group, ResolvedId& out) {
    out = {};
    unsigned long numeric = 0;
    if (parse_numeric_id(user, numeric)) {
        if (numeric > static_cast<unsigned long>(static_cast<uid_t>(-1))) {
            return backend_error(ErrorCode::invalid_argument, "invalid_identity",
                                 "run_user UID is out of range");
        }
        out.uid = static_cast<uid_t>(numeric);
        struct passwd* entry = ::getpwuid(out.uid);
        if (entry != nullptr && entry->pw_name != nullptr) out.user_name = entry->pw_name;
    } else {
        struct passwd* entry = ::getpwnam(user.c_str());
        if (entry == nullptr) {
            return backend_error(ErrorCode::invalid_argument, "invalid_identity",
                                 "unknown run_user '" + user + "': use an existing user name or numeric UID");
        }
        out.uid = entry->pw_uid;
        out.user_name = entry->pw_name != nullptr ? entry->pw_name : user;
    }

    if (parse_numeric_id(group, numeric)) {
        if (numeric > static_cast<unsigned long>(static_cast<gid_t>(-1))) {
            return backend_error(ErrorCode::invalid_argument, "invalid_identity",
                                 "run_group GID is out of range");
        }
        out.gid = static_cast<gid_t>(numeric);
        struct group* entry = ::getgrgid(out.gid);
        if (entry != nullptr && entry->gr_name != nullptr) out.group_name = entry->gr_name;
    } else {
        struct group* entry = ::getgrnam(group.c_str());
        if (entry == nullptr) {
            return backend_error(ErrorCode::invalid_argument, "invalid_identity",
                                 "unknown run_group '" + group +
                                     "': use an existing group name or numeric GID");
        }
        out.gid = entry->gr_gid;
        out.group_name = entry->gr_name != nullptr ? entry->gr_name : group;
    }

    out.groups.push_back(out.gid);
    if (!out.user_name.empty()) {
        int count = 16;
        std::vector<gid_t> groups(static_cast<std::size_t>(count));
        if (::getgrouplist(out.user_name.c_str(), out.gid, groups.data(), &count) < 0) {
            if (count > 0) {
                groups.resize(static_cast<std::size_t>(count));
                if (::getgrouplist(out.user_name.c_str(), out.gid, groups.data(), &count) < 0) {
                    groups.clear();
                }
            }
        }
        if (!groups.empty()) {
            groups.resize(static_cast<std::size_t>(count));
            out.groups = std::move(groups);
        }
    }
    if (!contains_gid(out.groups, out.gid)) out.groups.push_back(out.gid);
    return Error::none();
}

std::vector<gid_t> current_groups() {
    std::vector<gid_t> groups;
    const int count = ::getgroups(0, nullptr);
    if (count > 0) {
        groups.resize(static_cast<std::size_t>(count));
        if (::getgroups(count, groups.data()) < 0) groups.clear();
    }
    groups.push_back(::getegid());
    return groups;
}

// appId becomes one path component. Stored ids are already validated
// reverse-domain names; this also protects path parameters from traversal.
bool valid_app_token(std::string_view app_id) {
    if (app_id.empty() || app_id.size() > 255) return false;
    if (app_id == "." || app_id == "..") return false;
    for (const char ch : app_id) {
        if (ch == '/' || ch == '\0') return false;
    }
    return true;
}

void strip_control_characters(std::string& text) {
    std::string filtered;
    filtered.reserve(text.size());
    for (const char ch : text) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (value >= 0x20 && value != 0x7f) filtered.push_back(ch);
    }
    text.swap(filtered);
}

// Cuts at a UTF-8 code point boundary so a multi-byte name is never split.
std::string truncate_utf8(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

// Supervisor service names must be <=128 bytes, valid UTF-8 and control-free.
// The stable app id hash is part of the name because display names are not
// unique and the Supervisor enforces a UNIQUE(name) constraint.
std::string service_name_for(const AppRecord& app) {
    std::string base = trim_ascii(app.display_name);
    strip_control_characters(base);
    if (base.empty() || !is_valid_utf8(base)) base = app.app_id;
    std::string unique = backend_service_id(app.app_id);
    if (unique.size() > 16) unique.resize(16);
    const std::string suffix = " (" + unique + ")" + kServiceNameSuffix;
    const std::size_t room =
        kMaxServiceNameBytes > suffix.size() ? kMaxServiceNameBytes - suffix.size() : 0;
    if (base.size() > room) base = truncate_utf8(base, room);
    std::string name = base + suffix;
    if (name.empty() || !is_valid_utf8(name)) name = "app" + suffix;
    if (name.size() > kMaxServiceNameBytes) name = truncate_utf8(name, kMaxServiceNameBytes);
    return name;
}

std::string syscall_message(const std::string& operation, const std::string& path) {
    return operation + " " + path + ": " + std::strerror(errno);
}

Error open_apps_root(const std::string& apps_root, UniqueFd& out) {
    if (apps_root.empty() || apps_root.front() != '/') {
        return backend_error(ErrorCode::unavailable, "backend_unavailable",
                             "the application root must be an absolute path; configure --apps-root");
    }
    out.reset(::open(apps_root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!out.valid()) {
        return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                             syscall_message("cannot open the application root", apps_root));
    }
    return Error::none();
}

// Safe mkdirat/openat chain: every component is opened relative to its parent
// with O_NOFOLLOW and must already be (or become) a directory.
Error ensure_directory(int parent_fd, const std::string& name, mode_t mode, UniqueFd& out) {
    if (::mkdirat(parent_fd, name.c_str(), mode) != 0 && errno != EEXIST) {
        return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                             syscall_message("cannot create directory", name));
    }
    out.reset(::openat(parent_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!out.valid()) {
        return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                             syscall_message("cannot open directory", name));
    }
    return Error::none();
}

// Traversal (--x) for other users is enough to reach a known child; listing
// stays denied. Only used on EdgeDesk-owned roots when a non-root backend
// identity must reach its run/data directory.
Error ensure_traversal(int dir_fd, const std::string& label) {
    struct stat info {};
    if (::fstat(dir_fd, &info) != 0) {
        return backend_error(ErrorCode::io_error, "backend_permission_denied",
                             syscall_message("cannot stat", label));
    }
    if ((info.st_mode & S_IXOTH) != 0) return Error::none();
    if (::fchmod(dir_fd, (info.st_mode & 07777) | S_IXOTH) != 0) {
        return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                             syscall_message("cannot grant traversal on", label));
    }
    return Error::none();
}

// True when a stream listener accepts a connection on this Unix socket. Used
// only to distinguish an abandoned socket from one owned by a live backend
// (for example during two concurrent start requests). The probe connection is
// closed immediately.
bool socket_is_live(const std::string& socket_path) {
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd.valid()) return false;
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    return ::connect(fd.get(), reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0;
}

Error remove_stale_socket(int dir_fd, const std::string& socket_path) {
    struct stat info {};
    if (::fstatat(dir_fd, kBackendSocketName, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return Error::none();
        return backend_error(ErrorCode::io_error, "backend_unavailable",
                             syscall_message("cannot inspect", socket_path));
    }
    // Only an abandoned socket may be removed. A regular file, directory or
    // symlink at this path is unexpected and must not be unlinked silently.
    if (!S_ISSOCK(info.st_mode)) {
        return backend_error(ErrorCode::conflict, "backend_unavailable",
                             "refusing to remove a non-socket file at " + socket_path);
    }
    if (socket_is_live(socket_path)) {
        return backend_error(ErrorCode::conflict, "backend_running",
                             "a process is already listening on " + socket_path +
                                 "; stop it before starting the backend again");
    }
    if (::unlinkat(dir_fd, kBackendSocketName, 0) != 0 && errno != ENOENT) {
        return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                             syscall_message("cannot remove stale socket", socket_path));
    }
    return Error::none();
}

// Verifies the stable executable without executing or mapping it. Install-time
// validation (APPX-01/02) records the architecture; here we re-check that the
// file is still a regular executable and that the recorded architecture is the
// architecture of this host.
Error verify_executable(std::string_view app_id, const BackendRecord& record,
                        const std::string& executable_path, const std::string& host_arch) {
    if (record.arch.empty() || record.arch == "unknown") {
        return backend_error(ErrorCode::conflict, "backend_arch_mismatch",
                             "the recorded backend architecture is missing; reinstall the application");
    }
    if (host_arch == "unknown") {
        return backend_error(ErrorCode::conflict, "backend_arch_mismatch",
                             "this host architecture is not supported for native backends");
    }
    if (record.arch != host_arch) {
        return backend_error(ErrorCode::conflict, "backend_arch_mismatch",
                             "the backend was installed for '" + record.arch + "' but this host is '" +
                                 host_arch + "'; install the matching package");
    }
    UniqueFd fd(::open(executable_path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
    if (!fd.valid()) {
        const std::string reason = errno == ENOENT ? "is missing" : "cannot be opened";
        return backend_error(ErrorCode::conflict, "backend_unavailable",
                             "the backend executable " + executable_path + " " + reason +
                                 "; reinstall the application");
    }
    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        return backend_error(ErrorCode::io_error, "backend_unavailable",
                             syscall_message("cannot stat the backend executable", executable_path));
    }
    if (!S_ISREG(info.st_mode)) {
        return backend_error(ErrorCode::conflict, "backend_unavailable",
                             "the backend executable is not a regular file; reinstall the application");
    }
    if ((info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
        return backend_error(ErrorCode::conflict, "backend_unavailable",
                             "the backend executable is not executable; reinstall the application");
    }
    (void)app_id;
    return Error::none();
}

} // namespace

Error backend_error(ErrorCode code, std::string_view slug, std::string message) {
    std::string text(slug);
    text += ": ";
    text += message;
    return Error::make(code, std::move(text));
}

std::string backend_error_slug(const Error& error, std::string_view fallback) {
    const auto separator = error.message.find(": ");
    if (separator == std::string::npos || separator == 0) return std::string(fallback);
    return error.message.substr(0, separator);
}

std::string backend_error_message(const Error& error) {
    const auto separator = error.message.find(": ");
    if (separator == std::string::npos || separator == 0) return error.message;
    return error.message.substr(separator + 2);
}

NativeBackendLifecycle::NativeBackendLifecycle(ExtensionStore& store, PackageManager& packages,
                                               NativeBackendInstaller& installer,
                                               std::string apps_root)
    : store_(store), packages_(packages), installer_(installer), apps_root_(std::move(apps_root)) {}

Error NativeBackendLifecycle::describe(std::string_view app_id, nlohmann::json& out) const {
    if (!valid_app_token(app_id)) {
        return backend_error(ErrorCode::invalid_argument, "invalid_argument",
                             "invalid application id");
    }
    AppRecord app;
    bool app_found = false;
    Error error = packages_.get_app(app_id, app, app_found);
    if (!error.ok()) return error;
    if (!app_found) {
        return backend_error(ErrorCode::not_found, "app_not_found", "application not found");
    }

    BackendRecord record;
    bool found = false;
    error = store_.get_backend(app_id, record, found);
    if (!error.ok()) return error;

    out = nlohmann::json::object();
    if (!found) {
        out["present"] = false;
        Manifest manifest;
        if (packages_.active_manifest(app_id, manifest).ok() && manifest.has_backend()) {
            out["reason"] =
                "the application package declares a native backend but no backend record exists; "
                "reinstall the application";
        } else {
            out["reason"] = "this application has no native backend";
        }
        return Error::none();
    }

    out["present"] = true;
    out["service_id"] =
        record.service_id.empty() ? backend_service_id(app_id) : record.service_id;
    out["arch"] = record.arch;
    out["host_arch"] = elf_arch_name(host_elf_arch());
    out["executable"] = backend_executable_path(apps_root_, app_id);
    out["program_dir"] = backend_program_dir(apps_root_, app_id);
    out["data_dir"] = backend_data_dir(apps_root_, app_id);
    out["socket_path"] = backend_socket_path(apps_root_, app_id);
    out["run_user"] = record.run_user;
    out["run_group"] = record.run_group;
    out["allow_root"] = record.allow_root;
    out["autostart"] = record.autostart;
    out["state"] = record.state;
    out["error"] = record.error;
    return Error::none();
}

Error NativeBackendLifecycle::service_params(std::string_view app_id,
                                             nlohmann::json& params) const {
    if (!valid_app_token(app_id)) {
        return backend_error(ErrorCode::invalid_argument, "invalid_argument",
                             "invalid application id");
    }
    AppRecord app;
    bool app_found = false;
    Error error = packages_.get_app(app_id, app, app_found);
    if (!error.ok()) return error;
    if (!app_found) {
        return backend_error(ErrorCode::not_found, "app_not_found", "application not found");
    }
    BackendRecord record;
    bool found = false;
    error = store_.get_backend(app_id, record, found);
    if (!error.ok()) return error;
    if (!found) {
        return backend_error(ErrorCode::conflict, "backend_not_configured",
                             "this application has no native backend");
    }
    if (record.run_user.empty() || record.run_group.empty()) {
        return backend_error(ErrorCode::invalid_argument, "backend_identity_required",
                             "configure run_user and run_group before starting the backend");
    }

    const std::string host_arch = elf_arch_name(host_elf_arch());
    const std::string executable = backend_executable_path(apps_root_, app_id);
    error = verify_executable(app_id, record, executable, host_arch);
    if (!error.ok()) return error;

    params = nlohmann::json::object();
    params["id"] = record.service_id.empty() ? backend_service_id(app_id) : record.service_id;
    params["name"] = service_name_for(app);
    params["executable"] = executable;
    params["args"] = record.args; // argv array; never a shell string
    params["cwd"] = backend_program_dir(apps_root_, app_id);
    params["env"] = {
        {kBackendSocketEnv, backend_socket_path(apps_root_, app_id)},
        {kBackendDataEnv, backend_data_dir(apps_root_, app_id)},
    };
    params["run_user"] = record.run_user;
    params["run_group"] = record.run_group;
    params["allow_root"] = record.allow_root;
    params["autostart"] = record.autostart;
    params["restart_policy"] = "on-failure";
    // Limits use the Supervisor defaults. V1 deliberately adds no health
    // check and no WebSocket/streaming proxy configuration here.
    return Error::none();
}

Error NativeBackendLifecycle::prepare_runtime(std::string_view app_id,
                                              std::string& socket_path) {
    if (!valid_app_token(app_id)) {
        return backend_error(ErrorCode::invalid_argument, "invalid_argument",
                             "invalid application id");
    }
    BackendRecord record;
    bool found = false;
    Error error = store_.get_backend(app_id, record, found);
    if (!error.ok()) return error;
    if (!found) {
        return backend_error(ErrorCode::conflict, "backend_not_configured",
                             "this application has no native backend");
    }
    if (record.run_user.empty() || record.run_group.empty()) {
        return backend_error(ErrorCode::invalid_argument, "backend_identity_required",
                             "configure run_user and run_group before starting the backend");
    }
    ResolvedId identity;
    error = resolve_identity(record.run_user, record.run_group, identity);
    if (!error.ok()) return error;

    const std::string socket = backend_socket_path(apps_root_, app_id);
    if (socket.size() > kMaxUnixSocketPathBytes) {
        return backend_error(ErrorCode::invalid_argument, "backend_unavailable",
                             "the backend socket path is " + std::to_string(socket.size()) +
                                 " bytes; Unix sockets support at most " +
                                 std::to_string(kMaxUnixSocketPathBytes) +
                                 " bytes. Use a shorter --apps-root");
    }

    UniqueFd root;
    error = open_apps_root(apps_root_, root);
    if (!error.ok()) return error;

    UniqueFd run_dir;
    error = ensure_directory(root.get(), "run", 0711, run_dir);
    if (!error.ok()) return error;
    UniqueFd app_dir;
    error = ensure_directory(run_dir.get(), std::string(app_id), 0770, app_dir);
    if (!error.ok()) return error;
    if (::fchmod(app_dir.get(), 0770) != 0) {
        return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                             syscall_message("cannot set run directory permissions",
                                             backend_run_dir(apps_root_, app_id)));
    }

    if (::geteuid() == 0) {
        // The run directory belongs to the backend identity so it can bind the
        // socket. EdgeDesk (root) can always connect.
        if (::fchown(app_dir.get(), identity.uid, identity.gid) != 0) {
            return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                                 syscall_message("cannot hand the run directory to " +
                                                     record.run_user + ":" + record.run_group,
                                                 backend_run_dir(apps_root_, app_id)));
        }
        if (identity.uid != 0) {
            error = ensure_traversal(root.get(), apps_root_);
            if (!error.ok()) return error;
            error = ensure_traversal(run_dir.get(), backend_run_dir(apps_root_, app_id));
            if (!error.ok()) return error;
        }
    } else {
        // Unprivileged EdgeDesk can only share a socket with the same uid or a
        // matching group. Report the mismatch instead of starting a backend
        // that EdgeDesk could not reach.
        const std::vector<gid_t> my_groups = current_groups();
        if (identity.uid != ::geteuid() && !contains_gid(my_groups, identity.gid)) {
            return backend_error(
                ErrorCode::permission_denied, "backend_permission_denied",
                "EdgeDesk runs as uid " + std::to_string(::geteuid()) +
                    " and is not a member of group " + std::to_string(identity.gid) +
                    "; run EdgeDesk as the backend user or as root, or choose a matching run_group");
        }
        struct stat info {};
        if (::fstat(app_dir.get(), &info) != 0) {
            return backend_error(ErrorCode::io_error, "backend_permission_denied",
                                 syscall_message("cannot stat run directory",
                                                 backend_run_dir(apps_root_, app_id)));
        }
        const bool can_write =
            (info.st_uid == identity.uid && (info.st_mode & S_IWUSR) != 0) ||
            (contains_gid(identity.groups, info.st_gid) && (info.st_mode & S_IWGRP) != 0);
        if (!can_write) {
            return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                                 "the backend user " + record.run_user +
                                     " cannot create the socket in " +
                                     backend_run_dir(apps_root_, app_id) +
                                     "; EdgeDesk must run as that user or as root");
        }
    }

    error = remove_stale_socket(app_dir.get(), socket);
    if (!error.ok()) return error;

    socket_path = socket;
    return Error::none();
}

Error NativeBackendLifecycle::clean_runtime(std::string_view app_id) {
    if (!valid_app_token(app_id)) {
        return backend_error(ErrorCode::invalid_argument, "invalid_argument",
                             "invalid application id");
    }
    UniqueFd root(::open(apps_root_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!root.valid()) {
        // A missing apps root means there is no runtime to clean.
        if (errno == ENOENT) return Error::none();
        return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                             syscall_message("cannot open the application root", apps_root_));
    }
    UniqueFd run_dir(::openat(root.get(), "run", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!run_dir.valid()) {
        if (errno == ENOENT) return Error::none();
        return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                             syscall_message("cannot open run directory",
                                             apps_root_ + "/run"));
    }
    UniqueFd app_dir(::openat(run_dir.get(), std::string(app_id).c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!app_dir.valid()) {
        if (errno == ENOENT) return Error::none();
        return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                             syscall_message("cannot open per-app run directory",
                                             backend_run_dir(apps_root_, app_id)));
    }
    return remove_stale_socket(app_dir.get(), backend_socket_path(apps_root_, app_id));
}

// Ensures the per-app data directory can be written by the configured
// identity. `applied` is false when unprivileged EdgeDesk cannot hand the
// directory to a different identity; the caller records that start will fail
// instead of pretending the identity is usable.
namespace {
Error apply_data_ownership(const std::string& apps_root, std::string_view app_id,
                           const ResolvedId& identity, bool& applied, std::string& reason) {
    applied = false;
    reason.clear();
    UniqueFd root;
    Error error = open_apps_root(apps_root, root);
    if (!error.ok()) return error;

    const bool privileged = ::geteuid() == 0;
    const mode_t parent_mode = privileged && identity.uid != 0 ? 0711 : 0700;
    UniqueFd data_dir;
    error = ensure_directory(root.get(), "data", parent_mode, data_dir);
    if (!error.ok()) return error;
    UniqueFd app_data;
    error = ensure_directory(data_dir.get(), std::string(app_id),
                             privileged ? 0750 : 0700, app_data);
    if (!error.ok()) return error;

    if (privileged) {
        if (::fchown(app_data.get(), identity.uid, identity.gid) != 0) {
            return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                                 syscall_message("cannot hand the data directory to the backend identity",
                                                 backend_data_dir(apps_root, app_id)));
        }
        if (::fchmod(app_data.get(), 0750) != 0) {
            return backend_error(ErrorCode::permission_denied, "backend_permission_denied",
                                 syscall_message("cannot set data directory permissions",
                                                 backend_data_dir(apps_root, app_id)));
        }
        if (identity.uid != 0) {
            error = ensure_traversal(root.get(), apps_root);
            if (!error.ok()) return error;
            error = ensure_traversal(data_dir.get(), apps_root + "/data");
            if (!error.ok()) return error;
        }
        applied = true;
        return Error::none();
    }

    struct stat info {};
    if (::fstat(app_data.get(), &info) != 0) {
        return backend_error(ErrorCode::io_error, "backend_permission_denied",
                             syscall_message("cannot stat data directory",
                                             backend_data_dir(apps_root, app_id)));
    }
    if (info.st_uid != ::geteuid()) {
        reason = "the data directory is owned by another user and EdgeDesk is not root";
        return Error::none();
    }
    if (identity.uid == ::geteuid()) {
        applied = true;
        return Error::none();
    }
    reason = "EdgeDesk is not root and runs as a different user, so it cannot change the "
             "data directory owner";
    return Error::none();
}
} // namespace

Error NativeBackendLifecycle::set_identity(std::string_view app_id,
                                           const std::string& run_user,
                                           const std::string& run_group, bool allow_root) {
    if (!valid_app_token(app_id)) {
        return backend_error(ErrorCode::invalid_argument, "invalid_argument",
                             "invalid application id");
    }
    BackendRecord record;
    bool found = false;
    Error error = store_.get_backend(app_id, record, found);
    if (!error.ok()) return error;
    if (!found) {
        return backend_error(ErrorCode::conflict, "backend_not_configured",
                             "this application has no native backend");
    }

    const std::string user = trim_ascii(run_user);
    const std::string group = trim_ascii(run_group);
    if (user.empty() || group.empty()) {
        return backend_error(ErrorCode::invalid_argument, "backend_identity_required",
                             "run_user and run_group are both required; the backend never "
                             "inherits the EdgeDesk identity implicitly");
    }
    ResolvedId identity;
    error = resolve_identity(user, group, identity);
    if (!error.ok()) return error;
    if ((identity.uid == 0 || identity.gid == 0) && !allow_root) {
        return backend_error(ErrorCode::permission_denied, "backend_root_not_allowed",
                             "running the backend as root requires allow_root=true; "
                             "choose a non-root user or confirm root explicitly");
    }

    bool applied = false;
    std::string reason;
    error = apply_data_ownership(apps_root_, app_id, identity, applied, reason);
    if (!error.ok()) return error;

    error = store_.set_backend_identity(app_id, user, group, allow_root);
    if (!error.ok()) return error;

    if (!applied) {
        const std::string detail =
            "backend_permission_denied: the identity was saved, but " +
            (reason.empty() ? "data directory ownership cannot be established" : reason) +
            "; starting the backend will fail until EdgeDesk runs as root or as this identity";
        (void)store_.set_backend_state(app_id, record.state, detail);
    } else if (record.error.rfind("backend_permission_denied:", 0) == 0) {
        // Clear a stale "start will fail" note after a usable identity change.
        (void)store_.set_backend_state(app_id, record.state, "");
    }
    return Error::none();
}

Error NativeBackendLifecycle::set_autostart(std::string_view app_id, bool enabled) {
    if (!valid_app_token(app_id)) {
        return backend_error(ErrorCode::invalid_argument, "invalid_argument",
                             "invalid application id");
    }
    BackendRecord record;
    bool found = false;
    Error error = store_.get_backend(app_id, record, found);
    if (!error.ok()) return error;
    if (!found) {
        return backend_error(ErrorCode::conflict, "backend_not_configured",
                             "this application has no native backend");
    }
    return store_.set_backend_autostart(app_id, enabled);
}

Error NativeBackendLifecycle::set_state(std::string_view app_id, std::string_view state,
                                        const std::string& error) {
    if (state != backend_state::stopped && state != backend_state::upgrading &&
        state != backend_state::upgrade_failed) {
        return Error::make(ErrorCode::invalid_argument,
                           "backend state must be stopped, upgrading or upgrade_failed");
    }
    if (!valid_app_token(app_id)) {
        return Error::make(ErrorCode::invalid_argument, "invalid application id");
    }
    BackendRecord record;
    bool found = false;
    Error get_error = store_.get_backend(app_id, record, found);
    if (!get_error.ok()) return get_error;
    if (!found) {
        return backend_error(ErrorCode::conflict, "backend_not_configured",
                             "this application has no native backend");
    }
    return store_.set_backend_state(app_id, state, error);
}

Error NativeBackendLifecycle::service_id(std::string_view app_id, std::string& out,
                                         bool& found) const {
    out.clear();
    found = false;
    if (!valid_app_token(app_id)) {
        return Error::make(ErrorCode::invalid_argument, "invalid application id");
    }
    BackendRecord record;
    Error error = store_.get_backend(app_id, record, found);
    if (!error.ok()) return error;
    if (!found) return Error::none();
    out = record.service_id.empty() ? backend_service_id(app_id) : record.service_id;
    return Error::none();
}

} // namespace edgedesk::extensions
