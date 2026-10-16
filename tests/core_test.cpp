#include "app/asset_provider.hpp"
#include "app/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace edgedesk;

static void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

int main() {
    const char* argv[] = {"edgedesk", "--host", "127.0.0.1", "--port", "9042"};
    auto config = parse_args(5, argv);
    require(config.host == "127.0.0.1" && config.port == 9042, "config.host == \"127.0.0.1\" && config.port == 9042");
    try {
        const char* bad[] = {"edgedesk", "--host", "0.0.0.0"};
        (void)parse_args(3, bad);
        require(false, "must reject public bind");
    } catch (const std::invalid_argument&) {
    }
    require(is_safe_asset_path("/assets/main.js"), "is_safe_asset_path(\"/assets/main.js\")");
    require(!is_safe_asset_path("/../etc/passwd"), "!is_safe_asset_path(\"/../etc/passwd\")");
    require(!is_safe_asset_path("/foo/../bar"), "!is_safe_asset_path(\"/foo/../bar\")");
    require(!is_safe_asset_path("/foo%2fbar"), "!is_safe_asset_path(\"/foo%2fbar\")");
    require(!is_safe_asset_path("/x\\y"), "!is_safe_asset_path(\"/x\\y\")");
    require(!is_safe_asset_path("x.html"), "!is_safe_asset_path(\"x.html\")");
    require(mime_for_path("/a.svg") == "image/svg+xml", "mime_for_path(\"/a.svg\") == \"image/svg+xml\"");
    require(mime_for_path("/main.js") == "text/javascript; charset=utf-8", "js mime");
#if !EDGEDESK_EMBED_ASSETS
    const auto tmp = std::filesystem::temp_directory_path() / "edgedesk-core-test";
    std::filesystem::create_directories(tmp);
    {
        std::ofstream file(tmp / "index.html", std::ios::binary);
        file << "<html>ready</html>";
    }
    AssetProvider assets(tmp.string());
    auto home = assets.read("/");
    require(home && home->body == "<html>ready</html>", "home && home->body == \"<html>ready</html>\"");
    require(home->mime == "text/html; charset=utf-8", "html mime");
    require(!assets.read("/api/health"), "!assets.read(\"/api/health\")");
    require(!assets.read("/missing"), "!assets.read(\"/missing\")");
    require(!assets.read("/../etc/passwd"), "!assets.read(\"/../etc/passwd\")");
    std::filesystem::remove_all(tmp);
#endif
    std::cout << "core_test: PASS\n";
}
