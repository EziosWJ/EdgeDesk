#pragma once

#include "core/fd.hpp"

#include <memory>
#include <string>

// Small, schema-versioned application preferences stored in settings.json.
// Deployment-level configuration is never read from or written to this file.

namespace edgedesk {

struct AppSettings {
    std::string theme = "dark";          // light | dark | system
    std::string files_view = "list";     // list | grid
    bool files_show_hidden = true;
    int terminal_font_size = 13;         // 10..24
    int dashboard_metrics_window = 300;  // 60..3600 seconds
};

class SettingsStore {
public:
    static std::unique_ptr<SettingsStore> open(const std::string& data_dir, std::string& error);

    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    const AppSettings& settings() const { return settings_; }
    const std::string& last_error() const { return last_error_; }

    // Applies and validates a complete settings object, then writes it
    // atomically. On failure the in-memory settings are unchanged.
    bool save(const AppSettings& next, std::string& error);

    static bool validate(const AppSettings& settings, std::string& error);

private:
    SettingsStore(UniqueFd dir_fd, std::string data_dir) noexcept;

    UniqueFd dir_fd_;
    std::string data_dir_;
    AppSettings settings_;
    std::string last_error_;
};

} // namespace edgedesk
