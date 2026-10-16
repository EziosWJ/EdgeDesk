#include "web/web_root.hpp"

#include "web/embedded_assets.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace edgedesk {
namespace {

constexpr std::uintmax_t kMaxDiskAssetSize = 16u * 1024u * 1024u;

bool unsafe_segment(std::string_view path) {
    std::size_t pos = 0;
    while (pos < path.size()) {
        const auto next = path.find('/', pos);
        const auto part = path.substr(pos, next == std::string_view::npos ? next : next - pos);
        if (part == "." || part == "..") return true;
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }
    return false;
}

bool hashed_bundle(std::string_view path) { return path.rfind("/assets/", 0) == 0; }

} // namespace

bool is_safe_asset_path(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.size() > 4096) return false;
    if (path.find('\\') != path.npos || path.find('%') != path.npos ||
        path.find('\0') != path.npos || path.find('?') != path.npos ||
        path.find('#') != path.npos) return false;
    return !unsafe_segment(path);
}

std::string mime_for_path(std::string_view path) {
    const auto end = path.rfind('.');
    const auto ext = end == path.npos ? std::string_view{} : path.substr(end);
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".json") return "application/json";
    if (ext == ".webmanifest") return "application/manifest+json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

WebRoot::WebRoot(std::string disk_root) : disk_root_(std::move(disk_root)) {
#if EDGEDESK_EMBED_ASSETS
    embedded_ = true;
#else
    embedded_ = false;
#endif
}

std::optional<WebAsset> WebRoot::read(std::string_view url_path) const {
    if (!is_safe_asset_path(url_path)) return std::nullopt;
    if (url_path == "/") url_path = "/index.html";
    if (url_path.rfind("/api/", 0) == 0 || url_path.rfind("/ws/", 0) == 0) return std::nullopt;
#if EDGEDESK_EMBED_ASSETS
    const auto* item = find_embedded_asset(url_path);
    if (item == nullptr) return std::nullopt;
    WebAsset asset;
    asset.body.assign(reinterpret_cast<const char*>(item->bytes), item->size);
    asset.mime = item->mime;
    asset.etag = item->etag;
    asset.immutable = hashed_bundle(url_path);
    return asset;
#else
    namespace fs = std::filesystem;
    if (disk_root_.empty()) return std::nullopt;
    std::error_code ec;
    const auto canonical_root = fs::weakly_canonical(fs::path(disk_root_), ec);
    if (ec || !fs::is_directory(canonical_root, ec)) return std::nullopt;
    const auto input = (canonical_root / std::string(url_path.substr(1))).lexically_normal();
    const auto resolved = fs::canonical(input, ec);
    if (ec) return std::nullopt;
    // Protect against symlink escape and lexicographic prefix collisions.
    auto expected = canonical_root.begin();
    auto candidate = resolved.begin();
    for (; expected != canonical_root.end(); ++expected, ++candidate) {
        if (candidate == resolved.end() || *expected != *candidate) return std::nullopt;
    }
    if (!fs::is_regular_file(resolved, ec) || ec) return std::nullopt;
    const auto size = fs::file_size(resolved, ec);
    if (ec || size > kMaxDiskAssetSize) return std::nullopt;
    std::ifstream stream(resolved, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad() || data.size() != size) return std::nullopt;
    WebAsset asset;
    asset.body = std::move(data);
    asset.mime = mime_for_path(url_path);
    asset.immutable = hashed_bundle(url_path);
    return asset;
#endif
}

} // namespace edgedesk
