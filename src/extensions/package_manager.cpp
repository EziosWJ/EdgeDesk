#include "extensions/package_manager.hpp"

#include "core/atomic_file.hpp"
#include "core/fd.hpp"
#include "core/util.hpp"
#include "extensions/zip_reader.hpp"
#include "file/file_types.hpp"
#include "file/path_sandbox.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace edgedesk::extensions {
namespace {

constexpr std::size_t kMaxManifestBytes = 4 * 1024 * 1024;

std::string join_path(const std::string& parent, std::string_view name) {
    if (parent.empty()) return std::string(name);
    if (parent.back() == '/') return parent + std::string(name);
    return parent + "/" + std::string(name);
}

std::string base_name(std::string_view path) {
    const auto slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

bool has_extension(std::string_view path) {
    const auto slash = path.rfind('/');
    const auto dot = path.rfind('.');
    return dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash);
}

std::vector<std::string> permission_names(const std::vector<Permission>& permissions) {
    std::vector<std::string> out;
    out.reserve(permissions.size());
    for (const auto permission : permissions) out.emplace_back(permission_name(permission));
    std::sort(out.begin(), out.end());
    return out;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<std::string> intersect(const std::vector<std::string>& left,
                                   const std::vector<std::string>& right) {
    std::vector<std::string> out;
    for (const auto& value : left) {
        if (contains(right, value)) out.push_back(value);
    }
    std::sort(out.begin(), out.end());
    return out;
}

Error read_small_file(const PathSandbox& sandbox, const std::string& path, std::size_t max_bytes,
                      std::string& out) {
    UniqueFd fd;
    Error error = sandbox.open_file(path, O_RDONLY, fd);
    if (!error.ok()) return error;
    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        return Error::make(ErrorCode::io_error, "cannot stat " + path);
    }
    if (!S_ISREG(info.st_mode)) {
        return Error::make(ErrorCode::invalid_argument, path + " is not a regular file");
    }
    if (static_cast<std::uint64_t>(info.st_size) > max_bytes) {
        return Error::make(ErrorCode::too_large, path + " exceeds the allowed metadata size");
    }
    out.resize(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0;
    while (offset < out.size()) {
        const auto read = ::pread(fd.get(), out.data() + offset, out.size() - offset,
                                  static_cast<off_t>(offset));
        if (read < 0) {
            if (errno == EINTR) continue;
            return Error::make(ErrorCode::io_error, "cannot read " + path);
        }
        if (read == 0) break;
        offset += static_cast<std::size_t>(read);
    }
    out.resize(offset);
    return Error::none();
}

// Opens an already-normalized logical path as a regular file and returns its
// size. Directories and other special files are rejected (the package writer
// only ever creates regular files, but a tampered revision must not block).
Error open_regular_file(const PathSandbox& sandbox, const std::string& path, UniqueFd& fd,
                        std::uint64_t& size) {
    Error error = sandbox.open_file(path, O_RDONLY, fd);
    if (!error.ok()) return error;
    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        fd.reset();
        return Error::make(ErrorCode::io_error, "cannot stat " + path);
    }
    if (!S_ISREG(info.st_mode)) {
        fd.reset();
        return Error::make(ErrorCode::invalid_argument, path + " is not a regular file");
    }
    size = static_cast<std::uint64_t>(info.st_size);
    return Error::none();
}

bool path_is_within(const std::string& candidate, const std::string& parent) {
    if (candidate.size() < parent.size()) return false;
    if (candidate.compare(0, parent.size(), parent) != 0) return false;
    return candidate.size() == parent.size() || parent.back() == '/' ||
           candidate[parent.size()] == '/';
}

void remove_path_best_effort(const std::string& path) {
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) return;
    if (S_ISDIR(info.st_mode)) {
        std::string ignored;
        remove_tree_at(path, ignored);
    } else {
        ::unlink(path.c_str());
    }
}

} // namespace

bool remove_tree_at(const std::string& path, std::string& error) {
    UniqueFd dir(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!dir.valid()) {
        if (errno == ENOENT) return true;
        error = "cannot open directory " + path + ": " + std::strerror(errno);
        return false;
    }
    DIR* raw = ::fdopendir(dir.release());
    if (raw == nullptr) {
        error = "cannot read directory " + path + ": " + std::strerror(errno);
        return false;
    }
    struct DirCloser {
        DIR* dir;
        ~DirCloser() { ::closedir(dir); }
    } closer{raw};
    const int fd = ::dirfd(raw);
    for (;;) {
        errno = 0;
        struct dirent* entry = ::readdir(raw);
        if (entry == nullptr) {
            if (errno != 0) {
                error = "cannot enumerate " + path + ": " + std::strerror(errno);
                return false;
            }
            break;
        }
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        struct stat info {};
        if (::fstatat(fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
            error = "cannot stat " + name + " in " + path + ": " + std::strerror(errno);
            return false;
        }
        if (S_ISDIR(info.st_mode)) {
            const std::string child = join_path(path, name);
            if (!remove_tree_at(child, error)) return false;
        } else if (::unlinkat(fd, name.c_str(), 0) != 0) {
            error = "cannot remove " + name + " in " + path + ": " + std::strerror(errno);
            return false;
        }
    }
    closer.dir = nullptr;
    ::closedir(raw);
    if (::unlinkat(AT_FDCWD, path.c_str(), AT_REMOVEDIR) != 0) {
        error = "cannot remove directory " + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

std::unique_ptr<PackageManager> PackageManager::create(const std::string& root, Error& error) {
    auto store = ExtensionStore::open(root, error);
    if (!store) return nullptr;
    std::unique_ptr<PackageManager> manager(new PackageManager(
        std::move(store), root, join_path(root, "tmp"), join_path(root, "revisions"),
        join_path(root, "data")));
    error = manager->prepare_directories();
    if (!error.ok()) return nullptr;
    error = manager->recover();
    if (!error.ok()) return nullptr;
    error = Error::none();
    return manager;
}

PackageManager::PackageManager(std::unique_ptr<ExtensionStore> store, std::string root,
                               std::string tmp_dir, std::string revisions_dir,
                               std::string data_dir)
    : store_(std::move(store)),
      root_(std::move(root)),
      tmp_dir_(std::move(tmp_dir)),
      revisions_dir_(std::move(revisions_dir)),
      data_dir_(std::move(data_dir)) {}

Error PackageManager::prepare_directories() {
    std::string error;
    if (!ensure_private_directory(tmp_dir_, error) || !ensure_private_directory(revisions_dir_, error) ||
        !ensure_private_directory(data_dir_, error)) {
        return Error::make(ErrorCode::permission_denied, error);
    }
    return Error::none();
}

std::string PackageManager::revision_path(std::string_view app_id, std::string_view revision_id) const {
    return revisions_dir_ + "/" + std::string(app_id) + "/" + std::string(revision_id);
}

Error PackageManager::move_into_place(const std::string& source, const std::string& destination) {
    if (::rename(source.c_str(), destination.c_str()) != 0) {
        return Error::make(ErrorCode::io_error,
                           "cannot activate revision: " + std::string(std::strerror(errno)), errno);
    }
    return Error::none();
}

Error PackageManager::remove_revision_directory(const std::string& app_id,
                                                const std::string& revision_id) {
    std::string error;
    if (!remove_tree_at(revision_path(app_id, revision_id), error)) {
        return Error::make(ErrorCode::io_error, error);
    }
    return Error::none();
}

Error PackageManager::remove_data_directory(const std::string& app_id) {
    std::string error;
    if (!remove_tree_at(data_dir_ + "/" + app_id, error)) {
        return Error::make(ErrorCode::io_error, error);
    }
    return Error::none();
}

Error PackageManager::read_extracted_manifest(const std::string& root, nlohmann::json& document) {
    Error sandbox_error;
    auto sandbox = PathSandbox::create(root, false, sandbox_error);
    if (!sandbox) return sandbox_error;
    std::string text;
    Error error = read_small_file(*sandbox, "/manifest.json", kMaxManifestBytes, text);
    if (!error.ok()) {
        return Error::make(error.code, "manifest.json is missing or unreadable: " + error.message);
    }
    document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return Error::make(ErrorCode::invalid_argument, "manifest.json is not a valid JSON object");
    }
    return Error::none();
}

Error PackageManager::fail_operation(const std::string& operation_id, const std::string& temp_path,
                                     const std::string& revision_path_value, const std::string& message) {
    if (!temp_path.empty() && path_is_within(temp_path, tmp_dir_)) {
        std::string ignored;
        remove_tree_at(temp_path, ignored);
    }
    if (!revision_path_value.empty() && path_is_within(revision_path_value, revisions_dir_)) {
        std::string ignored;
        remove_tree_at(revision_path_value, ignored);
    }
    return store_->update_operation(operation_id, "failed", "", message);
}

Error PackageManager::install(const std::string& zip_path, const InstallOptions& options,
                              InstallResult& result) {
    result = {};
    OperationRecord operation;
    operation.type = options.expected_app_id.empty() ? "install" : "upgrade";
    operation.phase = "staging";
    operation.temp_path = tmp_dir_ + "/op-" + random_hex(16);
    Error error = store_->create_operation(operation);
    if (!error.ok()) return error;
    const std::string operation_id = operation.operation_id;
    const std::string temp_path = operation.temp_path;

    std::string directory_error;
    if (!ensure_private_directory(temp_path, directory_error)) {
        store_->update_operation(operation_id, "failed", "", directory_error);
        return Error::make(ErrorCode::permission_denied, directory_error);
    }

    Error zip_error;
    auto zip = ZipReader::open(zip_path, zip_error);
    if (!zip) {
        std::string ignored;
        remove_tree_at(temp_path, ignored);
        store_->update_operation(operation_id, "failed", "", zip_error.message);
        return zip_error;
    }

    UniqueFd temp_fd;
    std::string open_error;
    temp_fd = open_private_dir(temp_path, open_error);
    if (!temp_fd.valid()) {
        std::string ignored;
        remove_tree_at(temp_path, ignored);
        store_->update_operation(operation_id, "failed", "", open_error);
        return Error::make(ErrorCode::permission_denied, open_error);
    }

    std::vector<IntegrityEntry> integrity;
    std::uint64_t total_bytes = 0;
    error = zip->extract_all(temp_fd.get(), integrity, total_bytes);
    temp_fd.reset();
    if (!error.ok()) {
        std::string ignored;
        remove_tree_at(temp_path, ignored);
        store_->update_operation(operation_id, "failed", "", error.message);
        return error;
    }
    store_->update_operation(operation_id, "extracted", "", "");

    nlohmann::json document;
    error = read_extracted_manifest(temp_path, document);
    if (!error.ok()) {
        fail_operation(operation_id, temp_path, "", error.message);
        return error;
    }
    Manifest manifest;
    std::vector<std::string> warnings;
    std::string manifest_error;
    if (!parse_manifest(document, manifest, manifest_error, warnings)) {
        fail_operation(operation_id, temp_path, "", manifest_error);
        return Error::make(ErrorCode::invalid_argument, manifest_error);
    }
    // The validated manifest is kept in `manifest.raw`, so the separately
    // parsed document is no longer needed. Dump once and release the DOM to
    // avoid carrying several full copies of the manifest into the commit.
    std::string manifest_json = manifest.raw.dump();
    document = nlohmann::json();
    if (!options.expected_app_id.empty() && options.expected_app_id != manifest.id) {
        const std::string message = "package application id does not match the upgrade target";
        fail_operation(operation_id, temp_path, "", message);
        return Error::make(ErrorCode::invalid_argument, message);
    }
    if (!manifest.host_api_compatible()) {
        const std::string message = "application requires host API " + manifest.min_host_api.to_string() +
                                    " .. " + manifest.max_host_api.to_string() + "; host provides " +
                                    kHostApiVersion;
        fail_operation(operation_id, temp_path, "", message);
        return Error::make(ErrorCode::unsupported, message);
    }
    bool has_index = false;
    bool has_manifest = false;
    for (const auto& file : integrity) {
        if (file.path == "index.html") has_index = true;
        if (file.path == "manifest.json") has_manifest = true;
    }
    if (!has_index || !has_manifest) {
        const std::string message = "package must contain manifest.json and index.html at its root";
        fail_operation(operation_id, temp_path, "", message);
        return Error::make(ErrorCode::invalid_argument, message);
    }
    store_->set_operation_app(operation_id, manifest.id);

    AppRecord existing;
    bool found = false;
    error = store_->get_app(manifest.id, existing, found);
    if (!error.ok()) {
        fail_operation(operation_id, temp_path, "", error.message);
        return error;
    }
    const bool reinstall = found && existing.status == app_status::uninstalled;
    const bool upgrading = found && !reinstall;

    if (upgrading && !existing.active_version.empty()) {
        SemVer previous;
        std::string parse_error;
        if (SemVer::parse(existing.active_version, previous, parse_error)) {
            const int comparison = compare(manifest.version_semver, previous);
            if (comparison == 0 && !options.overwrite_same_version) {
                const std::string message =
                    "the same version is already installed; explicit overwrite confirmation is required";
                fail_operation(operation_id, temp_path, "", message);
                return Error::make(ErrorCode::conflict, message);
            }
            if (comparison < 0 && !options.allow_downgrade) {
                const std::string message =
                    "the package is older than the active revision; explicit downgrade confirmation is required";
                fail_operation(operation_id, temp_path, "", message);
                return Error::make(ErrorCode::conflict, message);
            }
        }
    }

    bool restored = false;
    const bool retained = found && existing.data_retained;
    if (retained && reinstall) {
        restored = options.restore_retained_data;
        if (!restored) {
            error = store_->kv_delete_all(manifest.id);
            if (!error.ok()) {
                fail_operation(operation_id, temp_path, "", error.message);
                return error;
            }
        }
    } else if (found && existing.status == app_status::uninstalled) {
        // Stale retained rows without the flag are not restorable.
        error = store_->kv_delete_all(manifest.id);
        if (!error.ok()) {
            fail_operation(operation_id, temp_path, "", error.message);
            return error;
        }
    }

    std::string revision_id;
    for (int attempt = 0; attempt < 8; ++attempt) {
        revision_id = random_hex(16);
        RevisionRecord probe;
        bool probe_found = false;
        error = store_->get_revision(revision_id, probe, probe_found);
        if (!error.ok()) {
            fail_operation(operation_id, temp_path, "", error.message);
            return error;
        }
        if (!probe_found) break;
        revision_id.clear();
    }
    if (revision_id.empty()) {
        const std::string message = "cannot allocate a unique revision id";
        fail_operation(operation_id, temp_path, "", message);
        return Error::make(ErrorCode::unavailable, message);
    }

    const std::string app_revisions = revisions_dir_ + "/" + manifest.id;
    if (!ensure_private_directory(app_revisions, directory_error)) {
        fail_operation(operation_id, temp_path, "", directory_error);
        return Error::make(ErrorCode::permission_denied, directory_error);
    }
    const std::string destination = revision_path(manifest.id, revision_id);
    error = move_into_place(temp_path, destination);
    if (!error.ok()) {
        fail_operation(operation_id, temp_path, "", error.message);
        return error;
    }
    store_->update_operation(operation_id, "committed", revision_id, "");

    RevisionRecord revision;
    revision.revision_id = revision_id;
    revision.app_id = manifest.id;
    revision.version = manifest.version;
    revision.manifest_json = std::move(manifest_json);
    revision.created_at = now_unix_seconds();
    revision.file_count = integrity.size();
    revision.total_bytes = total_bytes;
    revision.status = revision_status::active;
    revision.integrity = integrity_state::ok;

    AppRecord app = existing;
    app.app_id = manifest.id;
    app.display_name = manifest.name;
    app.active_revision_id = revision_id;
    app.active_version = manifest.version;
    app.data_retained = false;
    if (!found || reinstall) {
        app.status = app_status::pending_review;
        app.created_at = found ? existing.created_at : revision.created_at;
    } else if (existing.status == app_status::isolated) {
        app.status = app_status::pending_review;
    }
    if (app.created_at == 0) app.created_at = revision.created_at;

    const auto requested = permission_names(manifest.permissions);
    const std::string previous_active = upgrading ? existing.active_revision_id : "";
    const auto retained_grants = upgrading ? intersect(existing.granted_permissions, requested)
                                           : std::vector<std::string>{};
    const bool requires_fresh_approval = !found || reinstall || existing.status == app_status::isolated;

    error = store_->run_transaction([&]() -> Error {
        if (!previous_active.empty()) {
            RevisionRecord previous;
            bool previous_found = false;
            Error status_error = store_->get_revision(previous_active, previous, previous_found);
            if (!status_error.ok()) return status_error;
            if (previous_found && previous.status != revision_status::isolated) {
                status_error = store_->set_revision_status(previous_active, revision_status::historical);
                if (!status_error.ok()) return status_error;
            }
        }
        Error transaction_error = store_->upsert_app(app);
        if (!transaction_error.ok()) return transaction_error;
        transaction_error = store_->insert_revision(revision);
        if (!transaction_error.ok()) return transaction_error;
        transaction_error = store_->replace_integrity_entries(revision_id, integrity);
        if (!transaction_error.ok()) return transaction_error;
        transaction_error = store_->replace_permission_requests(manifest.id, requested);
        if (!transaction_error.ok()) return transaction_error;
        if (requires_fresh_approval) {
            transaction_error = store_->replace_permission_grants(manifest.id, {});
        } else {
            transaction_error = store_->replace_permission_grants(manifest.id, retained_grants);
        }
        return transaction_error;
    });
    if (!error.ok()) {
        fail_operation(operation_id, temp_path, destination, error.message);
        return error;
    }

    if (!ensure_private_directory(data_dir_ + "/" + manifest.id, directory_error)) {
        // The revision is committed; data scaffolding failure is non-fatal.
        warnings.push_back("application data directory could not be created: " + directory_error);
    }
    store_->update_operation(operation_id, "done", revision_id, "");

    std::vector<std::string> removed;
    const Error cleanup_error = cleanup_revisions(manifest.id, kDefaultHistoryRevisions, removed);
    if (!cleanup_error.ok()) {
        warnings.push_back("old revision cleanup failed: " + cleanup_error.message);
    }

    error = store_->get_app(manifest.id, result.app, found);
    if (!error.ok()) return error;
    result.revision = std::move(revision);
    result.manifest = std::move(manifest);
    result.operation_id = operation_id;
    result.publisher_verified = false;
    result.approval_required = result.app.status == app_status::pending_review;
    result.retained_data_restored = restored;
    result.warnings = std::move(warnings);
    result.warnings.push_back("publisher identity is not verified: packages are not digitally signed");
    return Error::none();
}

Error PackageManager::get_app(std::string_view app_id, AppRecord& app, bool& found) const {
    return store_->get_app(app_id, app, found);
}

Error PackageManager::get_revision(std::string_view revision_id, RevisionRecord& revision,
                                   bool& found) const {
    return store_->get_revision(revision_id, revision, found);
}

Error PackageManager::list_revisions(std::string_view app_id,
                                     std::vector<RevisionRecord>& revisions) const {
    return store_->list_revisions(app_id, revisions);
}

Error PackageManager::active_manifest(std::string_view app_id, Manifest& manifest) const {
    AppRecord app;
    bool found = false;
    Error error = store_->get_app(app_id, app, found);
    if (!error.ok()) return error;
    if (!found) return Error::make(ErrorCode::not_found, "application not found");
    if (app.active_revision_id.empty()) return Error::make(ErrorCode::not_found, "application has no revision");
    RevisionRecord revision;
    bool revision_found = false;
    error = store_->get_revision(app.active_revision_id, revision, revision_found);
    if (!error.ok()) return error;
    if (!revision_found) return Error::make(ErrorCode::not_found, "active revision not found");
    if (revision.status != revision_status::active || revision.integrity != integrity_state::ok) {
        return Error::make(ErrorCode::conflict, "active revision is not runnable");
    }
    const auto document = nlohmann::json::parse(revision.manifest_json, nullptr, false);
    std::vector<std::string> warnings;
    std::string manifest_error;
    if (!parse_manifest(document, manifest, manifest_error, warnings)) {
        return Error::make(ErrorCode::io_error, "stored manifest is invalid: " + manifest_error);
    }
    return Error::none();
}

Error PackageManager::approve(std::string_view app_id, const std::vector<std::string>& permissions,
                              bool enable, AppRecord& app) {
    bool found = false;
    Error error = store_->get_app(app_id, app, found);
    if (!error.ok()) return error;
    if (!found || app.status == app_status::uninstalled) {
        return Error::make(ErrorCode::not_found, "application not found");
    }
    std::vector<std::string> unique;
    for (const auto& permission : permissions) {
        if (!contains(app.requested_permissions, permission)) {
            return Error::make(ErrorCode::invalid_argument,
                               "permission was not requested by the application: " + permission);
        }
        if (!contains(unique, permission)) unique.push_back(permission);
    }
    std::sort(unique.begin(), unique.end());
    if (enable) {
        Manifest manifest;
        Error manifest_error = active_manifest(app_id, manifest);
        if (!manifest_error.ok()) return manifest_error;
        if (app.status == app_status::isolated) {
            return Error::make(ErrorCode::conflict,
                               "application is isolated after an integrity failure; reinstall it first");
        }
    }
    error = store_->run_transaction([&]() -> Error {
        Error transaction_error = store_->replace_permission_grants(app_id, unique);
        if (!transaction_error.ok()) return transaction_error;
        if (enable) return store_->set_app_status(app_id, app_status::enabled);
        if (app.status == app_status::pending_review) return store_->set_app_status(app_id, app_status::disabled);
        return Error::none();
    });
    if (!error.ok()) return error;
    return store_->get_app(app_id, app, found);
}

Error PackageManager::set_enabled(std::string_view app_id, bool enabled, AppRecord& app) {
    bool found = false;
    Error error = store_->get_app(app_id, app, found);
    if (!error.ok()) return error;
    if (!found || app.status == app_status::uninstalled) {
        return Error::make(ErrorCode::not_found, "application not found");
    }
    if (!enabled) {
        error = store_->set_app_status(app_id, app_status::disabled);
        if (!error.ok()) return error;
        return store_->get_app(app_id, app, found);
    }
    if (app.status == app_status::isolated) {
        return Error::make(ErrorCode::conflict,
                           "application is isolated after an integrity failure; reinstall it first");
    }
    if (app.status == app_status::pending_review) {
        return Error::make(ErrorCode::permission_denied,
                           "administrator permission review is required before this application can start");
    }
    Manifest manifest;
    error = active_manifest(app_id, manifest);
    if (!error.ok()) return error;
    if (!manifest.host_api_compatible()) {
        return Error::make(ErrorCode::unsupported, "application is not compatible with this host API version");
    }
    error = store_->set_app_status(app_id, app_status::enabled);
    if (!error.ok()) return error;
    return store_->get_app(app_id, app, found);
}

Error PackageManager::update_permissions(std::string_view app_id,
                                         const std::vector<std::string>& permissions,
                                         AppRecord& app) {
    bool found = false;
    Error error = store_->get_app(app_id, app, found);
    if (!error.ok()) return error;
    if (!found || app.status == app_status::uninstalled) {
        return Error::make(ErrorCode::not_found, "application not found");
    }
    std::vector<std::string> unique;
    for (const auto& permission : permissions) {
        if (!contains(app.requested_permissions, permission)) {
            return Error::make(ErrorCode::invalid_argument,
                               "permission was not requested by the application: " + permission);
        }
        if (!contains(unique, permission)) unique.push_back(permission);
    }
    std::sort(unique.begin(), unique.end());
    error = store_->replace_permission_grants(app_id, unique);
    if (!error.ok()) return error;
    return store_->get_app(app_id, app, found);
}

Error PackageManager::uninstall(std::string_view app_id, bool keep_data) {
    AppRecord app;
    bool found = false;
    Error error = store_->get_app(app_id, app, found);
    if (!error.ok()) return error;
    if (!found || app.status == app_status::uninstalled) {
        return Error::make(ErrorCode::not_found, "application not found");
    }
    std::vector<RevisionRecord> revisions;
    error = store_->list_revisions(app_id, revisions);
    if (!error.ok()) return error;

    const std::string app_id_string(app_id);
    error = store_->run_transaction([&]() -> Error {
        Error transaction_error = store_->clear_permission_grants(app_id);
        if (!transaction_error.ok()) return transaction_error;
        transaction_error = store_->replace_permission_requests(app_id, {});
        if (!transaction_error.ok()) return transaction_error;
        for (const auto& revision : revisions) {
            transaction_error = store_->delete_revision(revision.revision_id);
            if (!transaction_error.ok()) return transaction_error;
        }
        if (keep_data) {
            app.status = app_status::uninstalled;
            app.active_revision_id.clear();
            app.active_version.clear();
            app.data_retained = true;
            return store_->upsert_app(app);
        }
        transaction_error = store_->kv_delete_all(app_id);
        if (!transaction_error.ok()) return transaction_error;
        return store_->delete_app_record(app_id);
    });
    if (!error.ok()) return error;

    for (const auto& revision : revisions) {
        std::string ignored;
        remove_tree_at(revision_path(app_id, revision.revision_id), ignored);
    }
    std::string ignored;
    remove_tree_at(revisions_dir_ + "/" + app_id_string, ignored);
    if (!keep_data) remove_tree_at(data_dir_ + "/" + app_id_string, ignored);
    {
        std::lock_guard<std::mutex> lock(sandbox_mutex_);
        for (const auto& revision : revisions) sandboxes_.erase(revision.revision_id);
    }
    return Error::none();
}

Error PackageManager::activate_revision(std::string_view app_id, std::string_view revision_id,
                                        bool confirmed, AppRecord& app) {
    if (!confirmed) {
        return Error::make(ErrorCode::invalid_argument,
                           "explicit confirmation is required to activate another revision");
    }
    bool found = false;
    Error error = store_->get_app(app_id, app, found);
    if (!error.ok()) return error;
    if (!found || app.status == app_status::uninstalled) {
        return Error::make(ErrorCode::not_found, "application not found");
    }
    RevisionRecord revision;
    bool revision_found = false;
    error = store_->get_revision(revision_id, revision, revision_found);
    if (!error.ok()) return error;
    if (!revision_found || revision.app_id != app_id) {
        return Error::make(ErrorCode::not_found, "revision not found for this application");
    }
    if (revision.integrity != integrity_state::ok) {
        return Error::make(ErrorCode::conflict, "revision is isolated after an integrity failure");
    }
    if (revision.status == revision_status::active) {
        return store_->get_app(app_id, app, found);
    }
    const auto document = nlohmann::json::parse(revision.manifest_json, nullptr, false);
    Manifest manifest;
    std::vector<std::string> warnings;
    std::string manifest_error;
    if (!parse_manifest(document, manifest, manifest_error, warnings)) {
        return Error::make(ErrorCode::io_error, "stored manifest is invalid: " + manifest_error);
    }
    if (!manifest.host_api_compatible()) {
        return Error::make(ErrorCode::unsupported, "revision is not compatible with this host API version");
    }
    const std::string previous_active = app.active_revision_id;
    const auto requested = permission_names(manifest.permissions);
    const auto grants = intersect(app.granted_permissions, requested);
    const bool was_isolated = app.status == app_status::isolated;
    error = store_->run_transaction([&]() -> Error {
        if (!previous_active.empty()) {
            RevisionRecord previous;
            bool previous_found = false;
            Error transaction_error = store_->get_revision(previous_active, previous, previous_found);
            if (!transaction_error.ok()) return transaction_error;
            if (previous_found && previous.status != revision_status::isolated) {
                transaction_error = store_->set_revision_status(previous_active, revision_status::historical);
                if (!transaction_error.ok()) return transaction_error;
            }
        }
        Error transaction_error = store_->set_revision_status(revision_id, revision_status::active);
        if (!transaction_error.ok()) return transaction_error;
        transaction_error = store_->set_app_active_revision(app_id, revision_id, revision.version);
        if (!transaction_error.ok()) return transaction_error;
        transaction_error = store_->replace_permission_requests(app_id, requested);
        if (!transaction_error.ok()) return transaction_error;
        transaction_error = store_->replace_permission_grants(app_id, grants);
        if (!transaction_error.ok()) return transaction_error;
        if (was_isolated) {
            transaction_error = store_->set_app_status(app_id, app_status::disabled);
        }
        return transaction_error;
    });
    if (!error.ok()) return error;
    std::vector<std::string> removed;
    cleanup_revisions(app_id, kDefaultHistoryRevisions, removed);
    return store_->get_app(app_id, app, found);
}

Error PackageManager::pin_revision(std::string_view app_id, std::string_view revision_id,
                                   bool pinned) {
    RevisionRecord revision;
    bool found = false;
    Error error = store_->get_revision(revision_id, revision, found);
    if (!error.ok()) return error;
    if (!found || revision.app_id != app_id) {
        return Error::make(ErrorCode::not_found, "revision not found for this application");
    }
    return store_->set_revision_pinned(revision_id, pinned);
}

Error PackageManager::cleanup_revisions(std::string_view app_id, int history_revisions,
                                        std::vector<std::string>& removed) {
    removed.clear();
    std::vector<RevisionRecord> revisions;
    Error error = store_->list_revisions(app_id, revisions);
    if (!error.ok()) return error;
    AppRecord app;
    bool found = false;
    error = store_->get_app(app_id, app, found);
    if (!error.ok()) return error;
    if (!found) return Error::make(ErrorCode::not_found, "application not found");
    const int keep_history = std::max(0, history_revisions);
    int history_kept = 0;
    for (const auto& revision : revisions) {
        if (revision.revision_id == app.active_revision_id) continue;
        if (revision.pinned || revision.status == revision_status::isolated) continue;
        if (revision.status == revision_status::active && revision.revision_id != app.active_revision_id) {
            // Coherence is repaired by recover(); never delete the app's
            // declared active revision.
            continue;
        }
        if (history_kept < keep_history) {
            ++history_kept;
            continue;
        }
        error = store_->delete_revision(revision.revision_id);
        if (!error.ok()) return error;
        std::string ignored;
        remove_tree_at(revision_path(std::string(app_id), revision.revision_id), ignored);
        {
            std::lock_guard<std::mutex> lock(sandbox_mutex_);
            sandboxes_.erase(revision.revision_id);
        }
        removed.push_back(revision.revision_id);
    }
    return Error::none();
}

Error PackageManager::check_integrity(std::string_view app_id, std::string_view revision_id,
                                      IntegrityResult& result) {
    result = {};
    AppRecord app;
    bool found = false;
    Error error = store_->get_app(app_id, app, found);
    if (!error.ok()) return error;
    if (!found || app.status == app_status::uninstalled) {
        return Error::make(ErrorCode::not_found, "application not found");
    }
    std::string target = std::string(revision_id);
    if (target.empty()) target = app.active_revision_id;
    if (target.empty()) return Error::make(ErrorCode::not_found, "application has no revision to check");
    RevisionRecord revision;
    bool revision_found = false;
    error = store_->get_revision(target, revision, revision_found);
    if (!error.ok()) return error;
    if (!revision_found || revision.app_id != app_id) {
        return Error::make(ErrorCode::not_found, "revision not found for this application");
    }
    result.revision_id = target;

    std::vector<IntegrityEntry> entries;
    error = store_->list_integrity_entries(target, entries);
    if (!error.ok()) return error;
    std::string mismatch;
    if (entries.empty()) {
        mismatch = "<empty integrity manifest>";
    } else {
        Error verify_error = ZipReader::verify_revision(
            revision_path(std::string(app_id), target), entries, mismatch);
        if (!verify_error.ok() && mismatch.empty()) mismatch = verify_error.message;
    }
    if (mismatch.empty()) {
        result.ok = true;
        return Error::none();
    }
    result.ok = false;
    result.mismatch_path = mismatch;
    const bool active = target == app.active_revision_id;
    error = store_->run_transaction([&]() -> Error {
        Error transaction_error = store_->set_revision_status(target, revision_status::isolated);
        if (!transaction_error.ok()) return transaction_error;
        transaction_error = store_->set_revision_integrity(target, integrity_state::mismatch);
        if (!transaction_error.ok()) return transaction_error;
        if (active) {
            transaction_error = store_->set_app_status(app_id, app_status::isolated);
            if (!transaction_error.ok()) return transaction_error;
            transaction_error = store_->clear_permission_grants(app_id);
            if (!transaction_error.ok()) return transaction_error;
            result.app_isolated = true;
        }
        return transaction_error;
    });
    return error;
}

std::shared_ptr<PathSandbox> PackageManager::static_sandbox(std::string_view app_id,
                                                            std::string_view revision_id) const {
    const std::string key = std::string(app_id) + "/" + std::string(revision_id);
    std::lock_guard<std::mutex> lock(sandbox_mutex_);
    const auto existing = sandboxes_.find(key);
    if (existing != sandboxes_.end()) return existing->second;
    Error error;
    auto sandbox = PathSandbox::create(revision_path(app_id, revision_id), false, error);
    if (!sandbox) return nullptr;
    auto shared = std::shared_ptr<PathSandbox>(std::move(sandbox));
    sandboxes_[key] = shared;
    return shared;
}

Error PackageManager::open_static(std::string_view app_id, std::string_view version,
                                  std::string_view revision_id, std::string_view relative,
                                  StaticAssetHandle& asset) const {
    asset = {};
    AppRecord app;
    bool found = false;
    Error error = store_->get_app(app_id, app, found);
    if (!error.ok()) return error;
    if (!found || app.status != app_status::enabled) {
        return Error::make(ErrorCode::not_found, "application is not installed and enabled");
    }
    if (app.active_revision_id != revision_id) {
        return Error::make(ErrorCode::not_found, "revision is not active");
    }
    RevisionRecord revision;
    bool revision_found = false;
    error = store_->get_revision(revision_id, revision, revision_found);
    if (!error.ok()) return error;
    if (!revision_found || revision.app_id != app_id || revision.version != version ||
        revision.status != revision_status::active || revision.integrity != integrity_state::ok) {
        return Error::make(ErrorCode::not_found, "revision is not available");
    }
    auto sandbox = static_sandbox(app_id, revision_id);
    if (!sandbox) return Error::make(ErrorCode::io_error, "revision files are unavailable");

    std::string requested(relative);
    if (requested.empty()) requested = "index.html";
    std::string logical = "/" + requested;
    std::string normalized;
    std::string message;
    if (!PathSandbox::normalize(logical, normalized, message)) {
        return Error::make(ErrorCode::invalid_argument, message);
    }
    std::string effective = normalized;
    UniqueFd fd;
    std::uint64_t size = 0;
    error = open_regular_file(*sandbox, normalized, fd, size);
    if (!error.ok() && !has_extension(normalized)) {
        effective = "/index.html";
        error = open_regular_file(*sandbox, effective, fd, size);
    }
    if (!error.ok()) return error;

    std::vector<IntegrityEntry> entries;
    error = store_->list_integrity_entries(revision_id, entries);
    if (!error.ok()) return error;
    std::string digest;
    for (const auto& entry : entries) {
        if ("/" + entry.path == effective) {
            digest = entry.sha256;
            break;
        }
    }
    asset.fd = std::move(fd);
    asset.size = size;
    asset.mime = mime_for_file_name(base_name(effective));
    asset.etag = digest.empty() ? std::string() : "\"" + digest + "\"";
    asset.immutable = true;
    Manifest manifest;
    const auto document = nlohmann::json::parse(revision.manifest_json, nullptr, false);
    std::vector<std::string> warnings;
    std::string manifest_error;
    if (parse_manifest(document, manifest, manifest_error, warnings)) {
        asset.csp = manifest.csp_policy();
    }
    return Error::none();
}

Error PackageManager::read_static(std::string_view app_id, std::string_view version,
                                  std::string_view revision_id, std::string_view relative,
                                  StaticAsset& asset) const {
    asset = {};
    StaticAssetHandle handle;
    Error error = open_static(app_id, version, revision_id, relative, handle);
    if (!error.ok()) return error;
    if (handle.size > std::numeric_limits<std::size_t>::max()) {
        return Error::make(ErrorCode::too_large, "static asset is too large to buffer");
    }
    asset.body.resize(static_cast<std::size_t>(handle.size));
    std::size_t offset = 0;
    while (offset < asset.body.size()) {
        const auto read = ::pread(handle.fd.get(), asset.body.data() + offset, asset.body.size() - offset,
                                  static_cast<off_t>(offset));
        if (read < 0) {
            if (errno == EINTR) continue;
            return Error::make(ErrorCode::io_error, "cannot read static asset");
        }
        if (read == 0) break;
        offset += static_cast<std::size_t>(read);
    }
    asset.body.resize(offset);
    asset.mime = std::move(handle.mime);
    asset.etag = std::move(handle.etag);
    asset.csp = std::move(handle.csp);
    asset.immutable = handle.immutable;
    return Error::none();
}

Error PackageManager::revision_directory(std::string_view app_id, std::string_view revision_id,
                                         std::string& path) const {
    RevisionRecord revision;
    bool found = false;
    Error error = store_->get_revision(revision_id, revision, found);
    if (!error.ok()) return error;
    if (!found || revision.app_id != app_id) {
        return Error::make(ErrorCode::not_found, "revision not found for this application");
    }
    path = revision_path(app_id, revision_id);
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        return Error::make(ErrorCode::not_found, "revision files are unavailable");
    }
    return Error::none();
}

Error PackageManager::recover() {
    std::vector<OperationRecord> pending;
    Error error = store_->list_pending_operations(pending);
    if (!error.ok()) return error;
    for (const auto& operation : pending) {
        if (!operation.temp_path.empty() && path_is_within(operation.temp_path, tmp_dir_)) {
            std::string ignored;
            remove_tree_at(operation.temp_path, ignored);
        }
        if (!operation.revision_id.empty() && !operation.app_id.empty()) {
            RevisionRecord revision;
            bool found = false;
            Error revision_error = store_->get_revision(operation.revision_id, revision, found);
            if (!revision_error.ok()) return revision_error;
            if (!found || revision.status == revision_status::pending) {
                std::string ignored;
                remove_tree_at(revision_path(operation.app_id, operation.revision_id), ignored);
            }
        }
        store_->update_operation(operation.operation_id, "failed", "", "interrupted by service restart");
    }

    // Remove orphan temporary directories left before an operation row existed.
    {
        UniqueFd tmp(::open(tmp_dir_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (tmp.valid()) {
            DIR* raw = ::fdopendir(tmp.release());
            if (raw != nullptr) {
                std::vector<std::string> orphans;
                for (;;) {
                    errno = 0;
                    struct dirent* entry = ::readdir(raw);
                    if (entry == nullptr) break;
                    const std::string name = entry->d_name;
                    if (name == "." || name == "..") continue;
                    if (name.rfind("op-", 0) == 0 || name.rfind("upload-", 0) == 0) orphans.push_back(name);
                }
                ::closedir(raw);
                for (const auto& name : orphans) {
                    remove_path_best_effort(join_path(tmp_dir_, name));
                }
            }
        }
    }

    // Drop revision directories that have no database row and repair active
    // revision pointers.
    std::vector<AppRecord> apps;
    error = store_->list_apps(apps, true);
    if (!error.ok()) return error;
    std::set<std::string> known_apps;
    for (const auto& app : apps) known_apps.insert(app.app_id);
    {
        UniqueFd root(::open(revisions_dir_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (root.valid()) {
            DIR* raw = ::fdopendir(root.release());
            if (raw != nullptr) {
                std::vector<std::string> orphan_apps;
                for (;;) {
                    errno = 0;
                    struct dirent* entry = ::readdir(raw);
                    if (entry == nullptr) break;
                    const std::string name = entry->d_name;
                    if (name == "." || name == "..") continue;
                    if (known_apps.find(name) == known_apps.end()) orphan_apps.push_back(name);
                }
                ::closedir(raw);
                for (const auto& name : orphan_apps) {
                    remove_path_best_effort(join_path(revisions_dir_, name));
                }
            }
        }
    }
    std::set<std::string> known_revisions;
    for (const auto& app : apps) {
        std::vector<RevisionRecord> revisions;
        error = store_->list_revisions(app.app_id, revisions);
        if (!error.ok()) return error;
        for (const auto& revision : revisions) known_revisions.insert(revision.revision_id);
        const std::string app_dir = revisions_dir_ + "/" + app.app_id;
        UniqueFd dir(::open(app_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (dir.valid()) {
            DIR* raw = ::fdopendir(dir.release());
            if (raw != nullptr) {
                std::vector<std::string> orphan_dirs;
                for (;;) {
                    errno = 0;
                    struct dirent* entry = ::readdir(raw);
                    if (entry == nullptr) break;
                    const std::string name = entry->d_name;
                    if (name == "." || name == "..") continue;
                    if (known_revisions.find(name) == known_revisions.end()) orphan_dirs.push_back(name);
                }
                ::closedir(raw);
                for (const auto& name : orphan_dirs) {
                    remove_path_best_effort(join_path(app_dir, name));
                }
            }
        }
        if (app.status == app_status::uninstalled) continue;
        bool active_found = false;
        bool active_isolated = false;
        for (const auto& revision : revisions) {
            if (revision.revision_id == app.active_revision_id) {
                active_found = true;
                if (revision.status == revision_status::isolated) active_isolated = true;
                if (revision.status != revision_status::active &&
                    revision.status != revision_status::isolated) {
                    store_->set_revision_status(revision.revision_id, revision_status::active);
                }
            } else if (revision.status == revision_status::active) {
                store_->set_revision_status(revision.revision_id, revision_status::historical);
            }
        }
        if (active_isolated) {
            store_->set_app_status(app.app_id, app_status::isolated);
            store_->clear_permission_grants(app.app_id);
            continue;
        }
        if (!app.active_revision_id.empty() && !active_found) {
            AppRecord repaired = app;
            repaired.status = app_status::disabled;
            repaired.active_revision_id.clear();
            repaired.active_version.clear();
            store_->upsert_app(repaired);
            continue;
        }
        if (app.status == app_status::enabled && app.manifest.is_object()) {
            Manifest manifest;
            std::vector<std::string> warnings;
            std::string manifest_error;
            if (!parse_manifest(app.manifest, manifest, manifest_error, warnings) ||
                !manifest.host_api_compatible()) {
                store_->set_app_status(app.app_id, app_status::disabled);
            }
        }
    }
    return Error::none();
}

} // namespace edgedesk::extensions
