#pragma once

#include "core/error.hpp"
#include "extensions/manifest.hpp"
#include "extensions/native_backend.hpp"

#include <string>
#include <string_view>

// Install-time file operations for native application packages (SPEC-0011
// section 5 and 8). This class never starts a process and never executes
// package content. Validation happens before any metadata commit; the stable
// program path is replaced only after an upgrade has fully validated and the
// old backend has been stopped by the lifecycle layer.
namespace edgedesk::extensions {

class ExtensionStore;

class NativeBackendInstaller {
public:
    NativeBackendInstaller(ExtensionStore& store, std::string apps_root);

    NativeBackendInstaller(const NativeBackendInstaller&) = delete;
    NativeBackendInstaller& operator=(const NativeBackendInstaller&) = delete;

    // Validates the declared backend inside the freshly extracted revision:
    // relative path only, regular executable ELF for the host architecture.
    // Fills executable_rel/args/arch and service_id; administrator-owned
    // fields are left untouched. No filesystem change outside `extracted_root`.
    // A manifest without `backend` is not an error: the method returns ok and
    // leaves `out` untouched.
    Error validate(const std::string& extracted_root, const Manifest& manifest, BackendRecord& out);

    // Copies the validated executable into the stable program directory and
    // upserts the backend row in `stopped` state. `previous` preserves the
    // administrator identity/autostart across an upgrade; it may be null for a
    // first install. Must be called only after the revision is committed.
    // Without a declared backend this returns invalid_argument.
    Error install(const std::string& revision_root, const Manifest& manifest,
                  const BackendRecord* previous, BackendRecord& out, std::string& warning);

    // Removes the Supervisor service mapping row, program directory, run
    // directory and (with `purge_data`) the app data directory. Never follows a
    // symlink out of the EdgeDesk-owned roots; caller must have stopped the
    // backend first and confirmed it is not running.
    Error remove(std::string_view app_id, bool purge_data);

    // Independently confirmed data deletion for an uninstalled/present app.
    // Touches only this app's dedicated data directory.
    Error purge_data(std::string_view app_id);

    // Re-derives absolute paths and re-checks that the stable executable still
    // exists, is executable and matches the recorded architecture.
    Error verify(std::string_view app_id, BackendRecord& out) const;

private:
    ExtensionStore& store_;
    std::string apps_root_;
};

} // namespace edgedesk::extensions
