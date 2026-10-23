#pragma once

#include "core/error.hpp"
#include "core/fd.hpp"
#include "extensions/extension_store.hpp"
#include "extensions/manifest.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace edgedesk {
class PathSandbox;
}

namespace edgedesk::extensions {

class NativeBackendInstaller;

struct InstallOptions {
    // When set, the manifest id must match (used by /apps/{id}/upgrade).
    std::string expected_app_id;
    bool allow_downgrade = false;
    bool overwrite_same_version = false;
    bool restore_retained_data = false;
    bool require_enable_after_approval = false;
};

struct InstallResult {
    AppRecord app;
    RevisionRecord revision;
    Manifest manifest;
    std::vector<std::string> warnings;
    bool publisher_verified = false;
    bool approval_required = true;
    bool retained_data_restored = false;
    std::string operation_id;
};

struct IntegrityResult {
    bool ok = true;
    std::string revision_id;
    std::string mismatch_path;
    bool app_isolated = false;
};

struct StaticAsset {
    std::string body;
    std::string mime;
    std::string etag;
    std::string csp;
    bool immutable = true;
};

// Bounded-memory view of a registered static asset. The file stays open for
// the caller to stream; no content is read into memory here.
struct StaticAssetHandle {
    UniqueFd fd;
    std::uint64_t size = 0;
    std::string mime;
    std::string etag;
    std::string csp;
    bool immutable = true;
};

class PackageManager {
public:
    static std::unique_ptr<PackageManager> create(const std::string& root, Error& error);

    PackageManager(const PackageManager&) = delete;
    PackageManager& operator=(const PackageManager&) = delete;

    ExtensionStore& store() { return *store_; }
    const std::string& root() const { return root_; }
    const std::string& temporary_directory() const { return tmp_dir_; }

    // Installed by the process-level assembly (Application). Null keeps the
    // package manager fully Web-only: a package that declares `backend` is then
    // rejected instead of silently installing an unused executable.
    void set_backend_installer(NativeBackendInstaller* installer) {
        backend_installer_ = installer;
    }
    NativeBackendInstaller* backend_installer() const { return backend_installer_; }

    // Bounded, synchronous stop used by upgrade before the executable is
    // replaced. It is installed only when a Supervisor is configured and must
    // return an error rather than overwrite a possibly running program.
    using BackendStopHook = std::function<Error(std::string_view app_id)>;
    void set_backend_stop_hook(BackendStopHook hook) { backend_stop_hook_ = std::move(hook); }

    // Cleans unfinished operations and orphan revision directories. Idempotent;
    // runs once at startup and is safe to call again.
    Error recover();

    // Installs or upgrades from a validated ZIP file. The file must already be
    // on local disk (streamed uploads finish before this call).
    Error install(const std::string& zip_path, const InstallOptions& options, InstallResult& result);

    Error approve(std::string_view app_id, const std::vector<std::string>& permissions, bool enable,
                  AppRecord& app);
    Error set_enabled(std::string_view app_id, bool enabled, AppRecord& app);
    Error update_permissions(std::string_view app_id, const std::vector<std::string>& permissions,
                             AppRecord& app);
    Error uninstall(std::string_view app_id, bool keep_data);
    Error activate_revision(std::string_view app_id, std::string_view revision_id, bool confirmed,
                            AppRecord& app);
    Error pin_revision(std::string_view app_id, std::string_view revision_id, bool pinned);
    Error cleanup_revisions(std::string_view app_id, int history_revisions,
                            std::vector<std::string>& removed);
    Error check_integrity(std::string_view app_id, std::string_view revision_id, IntegrityResult& result);

    Error get_app(std::string_view app_id, AppRecord& app, bool& found) const;
    Error get_revision(std::string_view revision_id, RevisionRecord& revision, bool& found) const;
    Error list_revisions(std::string_view app_id, std::vector<RevisionRecord>& revisions) const;
    Error active_manifest(std::string_view app_id, Manifest& manifest) const;

    // Serves a path from the active immutable revision URL. `relative` must be
    // empty or a normalized relative path.
    Error read_static(std::string_view app_id, std::string_view version, std::string_view revision_id,
                      std::string_view relative, StaticAsset& asset) const;

    // Resolves a registered active-revision asset and opens it through the
    // revision PathSandbox. Used by the HTTP layer to stream without buffering
    // the whole file. All path and revision checks live here.
    Error open_static(std::string_view app_id, std::string_view version, std::string_view revision_id,
                      std::string_view relative, StaticAssetHandle& asset) const;

    // Resolves a revision directory for transfer/audit code. appId and
    // revisionId must be known and belong together.
    Error revision_directory(std::string_view app_id, std::string_view revision_id,
                             std::string& path) const;

private:
    PackageManager(std::unique_ptr<ExtensionStore> store, std::string root, std::string tmp_dir,
                   std::string revisions_dir, std::string data_dir);

    Error prepare_directories();
    // Repairs backend rows after a crash: validates that each row still points
    // at the stable program file of the active revision, and marks rows that
    // cannot be started as upgrade_failed. Idempotent.
    Error recover_backends();
    Error fail_operation(const std::string& operation_id, const std::string& temp_path,
                         const std::string& revision_path, const std::string& message);
    Error move_into_place(const std::string& source, const std::string& destination);
    Error remove_revision_directory(const std::string& app_id, const std::string& revision_id);
    Error remove_data_directory(const std::string& app_id);
    Error read_extracted_manifest(const std::string& root, nlohmann::json& document);
    std::string revision_path(std::string_view app_id, std::string_view revision_id) const;
    std::shared_ptr<PathSandbox> static_sandbox(std::string_view app_id,
                                                std::string_view revision_id) const;

    std::unique_ptr<ExtensionStore> store_;
    std::string root_;
    std::string tmp_dir_;
    std::string revisions_dir_;
    std::string data_dir_;
    mutable std::mutex sandbox_mutex_;
    mutable std::unordered_map<std::string, std::shared_ptr<PathSandbox>> sandboxes_;
    NativeBackendInstaller* backend_installer_ = nullptr;
    BackendStopHook backend_stop_hook_;
};

// Recursively removes a directory without following symlinks.
bool remove_tree_at(const std::string& path, std::string& error);

} // namespace edgedesk::extensions
