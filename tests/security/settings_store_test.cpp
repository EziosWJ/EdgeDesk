#include "security/settings_store.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace edgedesk;

int main() {
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("edgedesk-settings-store-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    const auto data = (tmp / "data").string();

    {
        std::string error;
        auto store = SettingsStore::open(data, error);
        CHECK(store != nullptr);
        if (!store) return test::finish("settings_store_test");
        CHECK_STR_EQ(store->settings().theme, "dark");
        CHECK_EQ(store->settings().terminal_font_size, 13);

        AppSettings next = store->settings();
        next.theme = "light";
        next.files_view = "grid";
        next.files_show_hidden = false;
        next.terminal_font_size = 16;
        next.dashboard_metrics_window = 120;
        CHECK(store->save(next, error));
    }
    {
        std::string error;
        auto store = SettingsStore::open(data, error);
        CHECK(store != nullptr);
        if (!store) return test::finish("settings_store_test");
        CHECK_STR_EQ(store->settings().theme, "light");
        CHECK_STR_EQ(store->settings().files_view, "grid");
        CHECK(!store->settings().files_show_hidden);
        CHECK_EQ(store->settings().terminal_font_size, 16);
        CHECK_EQ(store->settings().dashboard_metrics_window, 120);

        AppSettings bad = store->settings();
        bad.terminal_font_size = 99;
        CHECK(!store->save(bad, error));
        CHECK_STR_EQ(store->settings().theme, "light");
    }

    {
        std::ofstream corrupt(data + "/settings.json");
        corrupt << R"({"schema":1,"theme":"neon","files_view":"list","files_show_hidden":true,"terminal_font_size":13,"dashboard_metrics_window":300})";
    }
    {
        std::string error;
        auto store = SettingsStore::open(data, error);
        CHECK(store != nullptr);
        if (!store) return test::finish("settings_store_test");
        CHECK_STR_EQ(store->settings().theme, "dark");
        CHECK(!store->last_error().empty());
    }

    {
        std::ofstream unknown(data + "/settings.json");
        unknown << R"({"schema":1,"theme":"dark","files_view":"list","files_show_hidden":true,"terminal_font_size":13,"dashboard_metrics_window":300,"secret":"x"})";
    }
    {
        std::string error;
        auto store = SettingsStore::open(data, error);
        CHECK(store != nullptr);
        if (!store) return test::finish("settings_store_test");
        CHECK(!store->last_error().empty());
    }

    std::filesystem::remove_all(tmp);
    return test::finish("settings_store_test");
}
