#pragma once

#include "core/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Host-side model for one optional native backend per extension application
// (SPEC-0011). Paths are derived from the application root; the manifest can
// never supply them. Program files, persistent data and runtime state live in
// three separate directories.
namespace edgedesk::extensions {

// Passed to the backend by the host. A manifest cannot override these names.
inline constexpr const char* kBackendSocketEnv = "EDGEDESK_APP_SOCKET";
inline constexpr const char* kBackendDataEnv = "EDGEDESK_APP_DATA_DIR";

// Fixed basename inside the stable program directory. Upgrades replace this
// file; no per-version backend binaries are retained.
inline constexpr const char* kBackendExecutableName = "backend";
inline constexpr const char* kBackendSocketName = "backend.sock";

namespace backend_state {
// Lifecycle states persisted by the extension manager. Process liveness and
// restart/backoff states belong to the Process Supervisor and are never copied
// into this record.
inline constexpr const char* stopped = "stopped";
inline constexpr const char* upgrading = "upgrading";
inline constexpr const char* upgrade_failed = "upgrade_failed";
} // namespace backend_state

struct BackendRecord {
    std::string app_id;
    // Stable Supervisor service id ("app-<hash>"); never derived from user
    // input at request time.
    std::string service_id;
    // Path declared by the manifest, relative to the revision root.
    std::string executable_rel;
    std::vector<std::string> args;
    // Architecture recorded at install time ("x86_64", "aarch64", "arm32").
    std::string arch;
    // Administrator-controlled identity. Empty means "not configured"; the
    // backend cannot start until both fields are set.
    std::string run_user;
    std::string run_group;
    bool allow_root = false;
    bool autostart = false;
    std::string state = backend_state::stopped;
    std::string error;
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
};

// Pure path derivations under the EdgeDesk application root. Callers that touch
// the filesystem must use directory-FD based helpers rather than these strings
// alone (SPEC-0010 path safety).
std::string backend_program_dir(std::string_view apps_root, std::string_view app_id);
std::string backend_executable_path(std::string_view apps_root, std::string_view app_id);
std::string backend_data_dir(std::string_view apps_root, std::string_view app_id);
std::string backend_run_dir(std::string_view apps_root, std::string_view app_id);
std::string backend_socket_path(std::string_view apps_root, std::string_view app_id);

// "app-" + first 32 hex characters of SHA-256(appId): stable, collision-safe
// for practical purposes and inside the Supervisor's 64-character id limit.
std::string backend_service_id(std::string_view app_id);

} // namespace edgedesk::extensions
