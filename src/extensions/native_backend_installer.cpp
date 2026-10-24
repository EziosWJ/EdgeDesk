#include "extensions/native_backend_installer.hpp"

#include "core/fd.hpp"
#include "core/util.hpp"
#include "extensions/elf_info.hpp"
#include "extensions/extension_store.hpp"
#include "file/path_sandbox.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace edgedesk::extensions {
namespace {

constexpr mode_t kExecutableMode = 0555;
// The native root is traversable but not listable by other users. The program
// directory stays owner-writable (unlink/atomic replace by the host); the
// backend identity must be a different UID, so ownership — not the mode bits —
// is the isolation boundary. The executable itself is read-only.
constexpr mode_t kNativeRootMode = 0711;
constexpr mode_t kProgramDirMode = 0755;
// Owner-writable only while an atomic replace is in progress.
constexpr mode_t kProgramDirInstallMode = 0755;
constexpr mode_t kDataDirMode = 0700;
constexpr std::size_t kCopyChunk = 64 * 1024;

Error system_error(ErrorCode code, const std::string& context) {
    return Error::make(code, context + ": " + std::string(std::strerror(errno)), errno);
}

bool valid_component(std::string_view value) {
    if (value.empty() || value == "." || value == "..") return false;
    return value.find('/') == std::string_view::npos && value.find('\0') == std::string_view::npos;
}

std::string join_path(const std::string& parent, std::string_view name) {
    if (parent.empty()) return std::string(name);
    if (parent.back() == '/') return parent + std::string(name);
    return parent + "/" + std::string(name);
}

Error open_apps_root(const std::string& apps_root, UniqueFd& out) {
    const int fd = ::open(apps_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return system_error(ErrorCode::permission_denied, "cannot open application root " + apps_root);
    out.reset(fd);
    return Error::none();
}

// Creates (or opens) one direct child directory below an already-open parent.
// O_NOFOLLOW plus O_DIRECTORY keep a planted symlink from being traversed; an
// existing directory is never replaced. Only the dedicated program directory
// has its mode tightened, because the data directory may be owned/chowned by
// the configured backend identity (APPX-03).
Error open_or_create_subdir(int parent_fd, std::string_view name, mode_t mode, bool tighten,
                            const std::string& description, UniqueFd& out) {
    if (!valid_component(name)) {
        return Error::make(ErrorCode::invalid_argument, description + " has an invalid name");
    }
    if (::mkdirat(parent_fd, std::string(name).c_str(), mode) != 0 && errno != EEXIST) {
        return system_error(ErrorCode::permission_denied, "cannot create " + description);
    }
    const int fd = ::openat(parent_fd, std::string(name).c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return system_error(ErrorCode::permission_denied, description + " is not a usable directory");
    }
    UniqueFd opened(fd);
    if (tighten && ::fchmod(opened.get(), mode) != 0) {
        return system_error(ErrorCode::permission_denied, "cannot set " + description + " permissions");
    }
    out = std::move(opened);
    return Error::none();
}

// Program files must belong to the host identity (root when EdgeDesk runs as
// root). A directory or file owned by a different account could be replaced by
// that account, so installation refuses rather than tightening someone else's
// path.
Error require_host_owned(int fd, const std::string& description) {
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        return system_error(ErrorCode::io_error, "cannot stat " + description);
    }
    if (info.st_uid != ::geteuid()) {
        if (::geteuid() == 0) {
            if (::fchown(fd, 0, 0) != 0) {
                return system_error(ErrorCode::permission_denied,
                                    "cannot take ownership of " + description);
            }
            return Error::none();
        }
        return Error::make(ErrorCode::permission_denied,
                           description + " is not owned by the host identity");
    }
    return Error::none();
}

bool write_full(int fd, const char* data, std::size_t length) {
    std::size_t done = 0;
    while (done < length) {
        const auto written = ::write(fd, data + done, length - done);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        done += static_cast<std::size_t>(written);
    }
    return true;
}

bool read_full_at(int fd, char* data, std::size_t length, std::uint64_t offset) {
    std::size_t done = 0;
    while (done < length) {
        const auto read = ::pread(fd, data + done, length - done, static_cast<off_t>(offset + done));
        if (read < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (read == 0) return false;
        done += static_cast<std::size_t>(read);
    }
    return true;
}

// Recursively removes `name` below an already-open directory descriptor. A
// symlink is unlinked, never followed. ENOENT is treated as success so callers
// stay idempotent.
bool remove_child_at(int parent_fd, const std::string& name, std::string& error) {
    struct stat info {};
    if (::fstatat(parent_fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return true;
        error = "cannot inspect " + name + ": " + std::string(std::strerror(errno));
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        if (::unlinkat(parent_fd, name.c_str(), 0) != 0 && errno != ENOENT) {
            error = "cannot remove " + name + ": " + std::string(std::strerror(errno));
            return false;
        }
        return true;
    }
    UniqueFd dir(::openat(parent_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!dir.valid()) {
        error = "cannot open directory " + name + ": " + std::string(std::strerror(errno));
        return false;
    }
    DIR* raw = ::fdopendir(dir.release());
    if (raw == nullptr) {
        error = "cannot read directory " + name + ": " + std::string(std::strerror(errno));
        return false;
    }
    const int listing_fd = ::dirfd(raw);
    // readdir() while unlinking entries can skip names, so collect the names
    // first and recurse through a duplicated descriptor after closing the
    // stream. The original fd belongs to the DIR stream.
    UniqueFd children_fd(duplicate_fd(listing_fd));
    std::vector<std::string> children;
    for (;;) {
        errno = 0;
        struct dirent* entry = ::readdir(raw);
        if (entry == nullptr) {
            if (errno != 0) {
                error = "cannot enumerate directory " + name + ": " + std::string(std::strerror(errno));
                ::closedir(raw);
                return false;
            }
            break;
        }
        const std::string child = entry->d_name;
        if (child == "." || child == "..") continue;
        children.push_back(child);
    }
    ::closedir(raw);
    if (!children_fd.valid()) {
        error = "cannot duplicate directory descriptor for " + name + ": " +
                std::string(std::strerror(errno));
        return false;
    }
    for (const auto& child : children) {
        if (!remove_child_at(children_fd.get(), child, error)) return false;
    }
    if (::unlinkat(parent_fd, name.c_str(), AT_REMOVEDIR) != 0 && errno != ENOENT) {
        error = "cannot remove directory " + name + ": " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

// Removes <container>/<app_id> below the application root; missing containers
// are not an error.
Error remove_app_child(const std::string& apps_root, const char* container, const std::string& app_id) {
    UniqueFd root;
    Error error = open_apps_root(apps_root, root);
    if (!error.ok()) return error;
    UniqueFd parent(::openat(root.get(), container, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!parent.valid()) {
        if (errno == ENOENT) return Error::none();
        return system_error(ErrorCode::permission_denied,
                            std::string("cannot open application ") + container + " directory");
    }
    std::string message;
    const bool removed = remove_child_at(parent.get(), app_id, message);
    // Always fsync the container so the rename/unlink is durable.
    ::fsync(parent.get());
    if (!removed) return Error::make(ErrorCode::io_error, message);
    return Error::none();
}

bool arch_from_name(std::string_view name, ElfArch& out) {
    if (name == "x86_64") {
        out = ElfArch::x86_64;
        return true;
    }
    if (name == "aarch64") {
        out = ElfArch::aarch64;
        return true;
    }
    if (name == "arm32") {
        out = ElfArch::arm32;
        return true;
    }
    return false;
}

// Opens the declared executable through the revision sandbox, rejects hard
// links and non-regular files, and requires the package itself to carry an
// execute bit. ZIP extraction preserves the package's Unix execute bits (it
// never invents one), so this check rejects a 0644/0600 ELF instead of
// silently making it executable during installation.
Error inspect_revision_executable(const std::string& revision_root, const std::string& executable_rel,
                                  ElfArch expected, ElfArch& actual) {
    Error sandbox_error;
    auto sandbox = PathSandbox::create(revision_root, false, sandbox_error);
    if (!sandbox) return sandbox_error;
    UniqueFd source;
    Error error = sandbox->open_file("/" + executable_rel, O_RDONLY, source);
    if (!error.ok()) {
        return Error::make(error.code, "cannot open backend executable: " + error.message);
    }
    struct stat info {};
    if (::fstat(source.get(), &info) != 0) {
        return system_error(ErrorCode::io_error, "cannot stat backend executable");
    }
    if (!S_ISREG(info.st_mode)) {
        return Error::make(ErrorCode::invalid_argument, "backend executable is not a regular file");
    }
    if (info.st_nlink > 1) {
        return Error::make(ErrorCode::invalid_argument, "backend executable has multiple hard links");
    }
    if ((info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
        return Error::make(ErrorCode::invalid_argument,
                           "backend executable is not executable in the package (set the "
                           "executable bit before packing)");
    }
    std::string inspect_error;
    if (!inspect_elf(join_path(revision_root, executable_rel), expected, actual, inspect_error)) {
        const ErrorCode code =
            (actual != ElfArch::unknown && actual != expected) ? ErrorCode::unsupported
                                                               : ErrorCode::invalid_argument;
        return Error::make(code, inspect_error);
    }
    return Error::none();
}

} // namespace

NativeBackendInstaller::NativeBackendInstaller(ExtensionStore& store, std::string apps_root)
    : store_(store), apps_root_(std::move(apps_root)) {}

Error NativeBackendInstaller::validate(const std::string& extracted_root, const Manifest& manifest,
                                       BackendRecord& out) {
    if (!manifest.backend.present) return Error::none();
    if (!valid_component(manifest.id)) {
        return Error::make(ErrorCode::invalid_argument, "application id is not a valid directory name");
    }
    ElfArch actual = ElfArch::unknown;
    Error error = inspect_revision_executable(extracted_root, manifest.backend.executable,
                                              host_elf_arch(), actual);
    if (!error.ok()) return error;
    // Only package-declared fields are populated. Identity, root permission and
    // autostart stay administrator-owned and are preserved by install().
    out.app_id = manifest.id;
    out.service_id = backend_service_id(manifest.id);
    out.executable_rel = manifest.backend.executable;
    out.args = manifest.backend.args;
    out.arch = elf_arch_name(actual);
    out.state = backend_state::stopped;
    out.error.clear();
    return Error::none();
}

Error NativeBackendInstaller::install(const std::string& revision_root, const Manifest& manifest,
                                      const BackendRecord* previous, BackendRecord& out,
                                      std::string& warning) {
    warning.clear();
    if (!manifest.backend.present) {
        return Error::make(ErrorCode::invalid_argument, "package does not declare a native backend");
    }
    if (!valid_component(manifest.id)) {
        return Error::make(ErrorCode::invalid_argument, "application id is not a valid directory name");
    }
    ElfArch actual = ElfArch::unknown;
    Error error = inspect_revision_executable(revision_root, manifest.backend.executable,
                                              host_elf_arch(), actual);
    if (!error.ok()) return error;

    UniqueFd root;
    error = open_apps_root(apps_root_, root);
    if (!error.ok()) return error;

    UniqueFd native_root;
    error = open_or_create_subdir(root.get(), "native", kNativeRootMode, true, "native program root",
                                  native_root);
    if (!error.ok()) return error;
    error = require_host_owned(native_root.get(), "native program root");
    if (!error.ok()) return error;
    // The program directory is temporarily owner-writable for the atomic
    // replace. The guard restores the final read-only mode on every exit path,
    // including failures that leave the previous executable in place.
    UniqueFd program_dir;
    error = open_or_create_subdir(native_root.get(), manifest.id, kProgramDirInstallMode, true,
                                  "application program directory", program_dir);
    if (!error.ok()) return error;
    error = require_host_owned(program_dir.get(), "application program directory");
    if (!error.ok()) return error;
    struct ProgramDirGuard {
        int fd;
        ~ProgramDirGuard() { (void)::fchmod(fd, kProgramDirMode); }
    } program_dir_guard{program_dir.get()};

    // Persistent data lives outside the program directory and survives
    // upgrades. It is created 0700 here; identity-specific ownership is applied
    // by the lifecycle layer. Existing permissions are never tightened because
    // the directory may already be owned by the configured backend user.
    UniqueFd data_root;
    error = open_or_create_subdir(root.get(), "data", kDataDirMode, false, "application data root",
                                  data_root);
    if (!error.ok()) return error;
    UniqueFd app_data;
    error = open_or_create_subdir(data_root.get(), manifest.id, kDataDirMode, false,
                                  "application data directory", app_data);
    if (!error.ok()) return error;

    Error sandbox_error;
    auto sandbox = PathSandbox::create(revision_root, false, sandbox_error);
    if (!sandbox) return sandbox_error;
    UniqueFd source;
    error = sandbox->open_file("/" + manifest.backend.executable, O_RDONLY, source);
    if (!error.ok()) return Error::make(error.code, "cannot open backend executable: " + error.message);
    struct stat source_info {};
    if (::fstat(source.get(), &source_info) != 0) {
        return system_error(ErrorCode::io_error, "cannot stat backend executable");
    }
    if (!S_ISREG(source_info.st_mode) || source_info.st_nlink > 1) {
        return Error::make(ErrorCode::invalid_argument, "backend executable is not a single regular file");
    }

    const std::string temp_name = ".backend-" + random_hex(8) + ".tmp";
    UniqueFd temp(::openat(program_dir.get(), temp_name.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!temp.valid()) {
        return system_error(ErrorCode::permission_denied, "cannot create temporary backend file");
    }
    struct TempCleanup {
        int dir_fd;
        std::string name;
        bool active = true;
        ~TempCleanup() {
            if (active) ::unlinkat(dir_fd, name.c_str(), 0);
        }
    } cleanup{program_dir.get(), temp_name};

    std::vector<char> buffer(kCopyChunk);
    std::uint64_t copied = 0;
    const std::uint64_t expected_size = static_cast<std::uint64_t>(source_info.st_size);
    while (copied < expected_size) {
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(expected_size - copied, buffer.size()));
        if (!read_full_at(source.get(), buffer.data(), chunk, copied)) {
            return Error::make(ErrorCode::io_error, "cannot read backend executable");
        }
        if (!write_full(temp.get(), buffer.data(), chunk)) {
            return system_error(ErrorCode::io_error, "cannot write backend executable");
        }
        copied += chunk;
    }
    // A source that grew after fstat is rejected: only the validated bytes are
    // ever installed.
    char probe = 0;
    const auto extra = ::pread(source.get(), &probe, 1, static_cast<off_t>(expected_size));
    if (extra != 0) {
        return Error::make(ErrorCode::io_error, "backend executable changed during installation");
    }
    if (::fchmod(temp.get(), kExecutableMode) != 0) {
        return system_error(ErrorCode::permission_denied, "cannot set backend executable permissions");
    }
    if (::fsync(temp.get()) != 0) {
        return system_error(ErrorCode::io_error, "cannot flush backend executable");
    }
    temp.reset();
    if (::renameat(program_dir.get(), temp_name.c_str(), program_dir.get(), kBackendExecutableName) != 0) {
        return system_error(ErrorCode::io_error, "cannot activate backend executable");
    }
    cleanup.active = false;
    // Restore the read-only program directory before making the rename durable.
    if (::fchmod(program_dir.get(), kProgramDirMode) != 0) {
        return system_error(ErrorCode::permission_denied,
                            "cannot restrict application program directory permissions");
    }
    if (::fsync(program_dir.get()) != 0) {
        return system_error(ErrorCode::io_error, "cannot flush application program directory");
    }

    BackendRecord record;
    record.app_id = manifest.id;
    record.service_id = backend_service_id(manifest.id);
    record.executable_rel = manifest.backend.executable;
    record.args = manifest.backend.args;
    record.arch = elf_arch_name(actual);
    record.state = backend_state::stopped;
    record.error.clear();
    record.created_at = now_unix_seconds();
    if (previous != nullptr) {
        // An upgrade preserves the administrator's identity/autostart choice
        // and the Supervisor-assigned service id. Resetting the id would make
        // the next start create a duplicate service (name conflict).
        record.run_user = previous->run_user;
        record.run_group = previous->run_group;
        record.allow_root = previous->allow_root;
        record.autostart = previous->autostart;
        if (!previous->service_id.empty()) record.service_id = previous->service_id;
        if (previous->created_at > 0) record.created_at = previous->created_at;
    } else {
        record.run_user.clear();
        record.run_group.clear();
        record.allow_root = false;
        record.autostart = false;
    }
    error = store_.upsert_backend(record);
    if (!error.ok()) {
        return Error::make(error.code, "cannot record native backend: " + error.message);
    }
    out = record;
    return Error::none();
}

Error NativeBackendInstaller::remove(std::string_view app_id, bool purge_data) {
    if (!valid_component(app_id)) {
        return Error::make(ErrorCode::invalid_argument, "application id is not a valid directory name");
    }
    // Delete the service mapping before touching files so no Supervisor service
    // can keep pointing at a program that is about to disappear.
    Error error = store_.delete_backend(app_id);
    if (!error.ok()) return error;
    const std::string id(app_id);
    error = remove_app_child(apps_root_, "native", id);
    if (!error.ok()) return error;
    error = remove_app_child(apps_root_, "run", id);
    if (!error.ok()) return error;
    if (!purge_data) return Error::none();
    return NativeBackendInstaller::purge_data(app_id);
}

Error NativeBackendInstaller::purge_data(std::string_view app_id) {
    if (!valid_component(app_id)) {
        return Error::make(ErrorCode::invalid_argument, "application id is not a valid directory name");
    }
    return remove_app_child(apps_root_, "data", std::string(app_id));
}

Error NativeBackendInstaller::verify(std::string_view app_id, BackendRecord& out) const {
    bool found = false;
    Error error = store_.get_backend(app_id, out, found);
    if (!error.ok()) return error;
    if (!found) return Error::make(ErrorCode::not_found, "application has no native backend");

    ElfArch expected = ElfArch::unknown;
    if (out.arch.empty() || !arch_from_name(out.arch, expected)) {
        return Error::make(ErrorCode::unsupported, "installed backend architecture is not recorded");
    }
    const std::string program = join_path(
        join_path(join_path(apps_root_, "native"), out.app_id), kBackendExecutableName);
    ElfArch actual = ElfArch::unknown;
    std::string inspect_error;
    if (!inspect_elf(program, expected, actual, inspect_error)) {
        return Error::make(ErrorCode::unsupported, "installed backend is not runnable: " + inspect_error);
    }
    if (std::string(elf_arch_name(actual)) != out.arch) {
        return Error::make(ErrorCode::unsupported,
                           "installed backend architecture " + std::string(elf_arch_name(actual)) +
                               " does not match recorded " + out.arch);
    }
    return Error::none();
}

} // namespace edgedesk::extensions
