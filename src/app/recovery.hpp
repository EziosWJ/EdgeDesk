#pragma once

#include "core/config.hpp"

namespace edgedesk {

// Handles `edgedesk --reset-admin-password`: resets only the admin credential,
// preserves settings/audit/business data, prints the new one-time code and
// exits without starting the HTTP server.
int run_reset_admin_password(const Config& config);

} // namespace edgedesk
