#include "security/settings_store.hpp"

#include "core/atomic_file.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <utility>

namespace edgedesk {
namespace {

constexpr const char* kSettingsFile = "settings.json";
constexpr std::size_t kMaxSettingsBytes = 16 * 1024;

bool parse_settings(const std::string& text, AppSettings& settings, std::string& error) {
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error = "settings.json is not a JSON object";
        return false;
    }
    for (const auto& item : parsed.items()) {
        const auto& key = item.key();
        if (key != "schema" && key != "theme" && key != "files_view" && key != "files_show_hidden" &&
            key != "terminal_font_size" && key != "dashboard_metrics_window") {
            error = "settings.json contains an unknown field: " + key;
            return false;
        }
    }
    if (!parsed.contains("schema") || !parsed["schema"].is_number_integer() || parsed["schema"].get<int>() != 1) {
        error = "settings.json schema is not supported";
        return false;
    }
    AppSettings next;
    if (!parsed.contains("theme") || !parsed["theme"].is_string() ||
        !parsed.contains("files_view") || !parsed["files_view"].is_string() ||
        !parsed.contains("files_show_hidden") || !parsed["files_show_hidden"].is_boolean() ||
        !parsed.contains("terminal_font_size") || !parsed["terminal_font_size"].is_number_integer() ||
        !parsed.contains("dashboard_metrics_window") || !parsed["dashboard_metrics_window"].is_number_integer()) {
        error = "settings.json fields are missing or have the wrong type";
        return false;
    }
    next.theme = parsed["theme"].get<std::string>();
    next.files_view = parsed["files_view"].get<std::string>();
    next.files_show_hidden = parsed["files_show_hidden"].get<bool>();
    next.terminal_font_size = parsed["terminal_font_size"].get<int>();
    next.dashboard_metrics_window = parsed["dashboard_metrics_window"].get<int>();
    if (!SettingsStore::validate(next, error)) return false;
    settings = std::move(next);
    error.clear();
    return true;
}

std::string serialize_settings(const AppSettings& settings) {
    nlohmann::json body;
    body["schema"] = 1;
    body["theme"] = settings.theme;
    body["files_view"] = settings.files_view;
    body["files_show_hidden"] = settings.files_show_hidden;
    body["terminal_font_size"] = settings.terminal_font_size;
    body["dashboard_metrics_window"] = settings.dashboard_metrics_window;
    return body.dump();
}

} // namespace

SettingsStore::SettingsStore(UniqueFd dir_fd, std::string data_dir) noexcept
    : dir_fd_(std::move(dir_fd)), data_dir_(std::move(data_dir)) {}

std::unique_ptr<SettingsStore> SettingsStore::open(const std::string& data_dir, std::string& error) {
    auto dir_fd = open_private_dir(data_dir, error);
    if (!dir_fd.valid()) return nullptr;
    auto store = std::unique_ptr<SettingsStore>(new SettingsStore(std::move(dir_fd), data_dir));

    std::string text;
    bool missing = false;
    if (!read_file_at(store->dir_fd_.get(), kSettingsFile, kMaxSettingsBytes, text, missing, error)) {
        store->last_error_ = error;
        return store;
    }
    if (missing) return store;
    AppSettings parsed;
    if (!parse_settings(text, parsed, error)) {
        store->last_error_ = error;
        return store;
    }
    store->settings_ = std::move(parsed);
    return store;
}

bool SettingsStore::validate(const AppSettings& settings, std::string& error) {
    if (settings.theme != "light" && settings.theme != "dark" && settings.theme != "system") {
        error = "theme must be light, dark, or system";
        return false;
    }
    if (settings.files_view != "list" && settings.files_view != "grid") {
        error = "files_view must be list or grid";
        return false;
    }
    if (settings.terminal_font_size < 10 || settings.terminal_font_size > 24) {
        error = "terminal_font_size must be between 10 and 24";
        return false;
    }
    if (settings.dashboard_metrics_window < 60 || settings.dashboard_metrics_window > 3600) {
        error = "dashboard_metrics_window must be between 60 and 3600";
        return false;
    }
    error.clear();
    return true;
}

bool SettingsStore::save(const AppSettings& next, std::string& error) {
    if (!validate(next, error)) return false;
    if (!atomic_write_file_at(dir_fd_.get(), kSettingsFile, serialize_settings(next), 0600, error)) {
        return false;
    }
    settings_ = next;
    last_error_.clear();
    return true;
}

} // namespace edgedesk
