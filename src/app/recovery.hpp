#pragma once

#include "core/config.hpp"

namespace edgedesk {

// Handles `edgedesk --reset-admin-password`: resets only the admin credential,
// preserves settings/audit/business data and exits without starting the HTTP
// server. A new administrator password is set from the web UI afterwards.
int run_reset_admin_password(const Config& config);

} // namespace edgedesk
