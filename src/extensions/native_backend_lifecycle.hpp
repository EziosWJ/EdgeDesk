#pragma once

#include "core/error.hpp"
#include "extensions/manifest.hpp"
#include "extensions/native_backend.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

// Lifecycle helpers shared by the extension admin routes (SPEC-0011 section
// 6). The Supervisor IPC itself is driven by the route through
// SupervisorService so the libhv event loop never blocks; this class only
// validates local preconditions, prepares the per-app run directory and
// produces the exact service configuration the Supervisor will apply.
namespace edgedesk::extensions {

class ExtensionStore;
class PackageManager;
class NativeBackendInstaller;

// Lifecycle failures carry a stable machine-readable slug as a message
// prefix ("<slug>: <actionable text>"). This keeps the shared Error type while
// letting HTTP routes report stable codes such as backend_identity_required
// instead of a generic "conflict"/"invalid_argument". backend_error() builds
// such an Error; the two accessors split it again.
Error backend_error(ErrorCode code, std::string_view slug, std::string message);
std::string backend_error_slug(const Error& error, std::string_view fallback);
std::string backend_error_message(const Error& error);

class NativeBackendLifecycle {
public:
    NativeBackendLifecycle(ExtensionStore& store, PackageManager& packages,
                           NativeBackendInstaller& installer, std::string apps_root);

    NativeBackendLifecycle(const NativeBackendLifecycle&) = delete;
    NativeBackendLifecycle& operator=(const NativeBackendLifecycle&) = delete;

    // JSON view for the application detail and admin UI. Contains architecture,
    // program/data paths, configured identity, autostart, persisted state and
    // a `reason` when no backend is present.
    Error describe(std::string_view app_id, nlohmann::json& out) const;

    // Loads the record and verifies the executable is present and runnable.
    // Fills the Supervisor service JSON parameters used for create/update.
    Error service_params(std::string_view app_id, nlohmann::json& params) const;

    // Creates/refreshes the per-app run directory with permissions that allow
    // the configured identity to bind the socket and EdgeDesk to connect.
    // Fails rather than starting when the permissions cannot be established
    // safely. Returns the absolute socket path.
    Error prepare_runtime(std::string_view app_id, std::string& socket_path);

    // Removes stale socket/runtime files before a start. Never touches data.
    Error clean_runtime(std::string_view app_id);

    // Administrator identity changes. Empty run_user/run_group means the app is
    // not startable; `allow_root` continues to require the Supervisor's
    // explicit root confirmation and is never implied by an empty identity.
    Error set_identity(std::string_view app_id, const std::string& run_user,
                       const std::string& run_group, bool allow_root);

    // Autostart is independent from start/stop and defaults to false.
    Error set_autostart(std::string_view app_id, bool enabled);

    // Persists a state transition that belongs to the extension manager
    // (stopped/upgrading/upgrade_failed). Supervisor liveness is never stored
    // here; the UI reads it from the Supervisor.
    Error set_state(std::string_view app_id, std::string_view state, const std::string& error);

    // Supervisor service id for an app, or empty when it has no backend.
    Error service_id(std::string_view app_id, std::string& out, bool& found) const;

private:
    ExtensionStore& store_;
    PackageManager& packages_;
    NativeBackendInstaller& installer_;
    std::string apps_root_;
};

} // namespace edgedesk::extensions
