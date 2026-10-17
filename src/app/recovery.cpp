#include "app/recovery.hpp"

#include "security/audit.hpp"
#include "security/auth_store.hpp"

#include <iostream>

namespace edgedesk {

int run_reset_admin_password(const Config& config) {
    std::string error;
    auto store = AuthStore::open(config.data_dir, true, error);
    if (!store) {
        std::cerr << "Reset failed: " << error << "\n";
        return 1;
    }
    if (!store->reset_for_recovery(error)) {
        std::cerr << "Reset failed: " << error << "\n";
        return 1;
    }

    // Best-effort audit entry. Recovery must still work when the runtime audit
    // path is unavailable.
    std::string state_error;
    if (ensure_state_dir(config.state_dir, state_error)) {
        AuditOptions options;
        options.path = config.audit_log_path;
        options.max_bytes = config.audit_max_bytes;
        options.max_files = config.audit_max_files;
        options.enabled = config.audit_enabled;
        Error audit_error;
        auto audit = AuditLog::create(options, audit_error);
        if (audit) audit->record("auth.reset", "ok", "local recovery", "local", config.data_dir);
    }

    std::cout << "EdgeDesk admin authentication was reset.\n"
              << "  data dir: " << config.data_dir << "\n"
              << "  settings, audit log and business files were preserved.\n"
              << "  start the service again and set a new administrator password in the web UI.\n";
    return 0;
}

} // namespace edgedesk
