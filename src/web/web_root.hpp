#pragma once

#include <optional>
#include <string>
#include <string_view>

// Unified static web asset abstraction shared by disk and #embed modes.
// read() never touches device files; it only serves the frontend bundle.

namespace edgedesk {

struct WebAsset {
    std::string body;
    std::string mime;
    std::string etag;
    bool immutable = false; // content-hashed bundles may be cached forever
};

bool is_safe_asset_path(std::string_view path);
std::string mime_for_path(std::string_view path);

class WebRoot {
public:
    // disk_root is only used when EDGEDESK_EMBED_ASSETS == 0.
    explicit WebRoot(std::string disk_root);

    // Returns nullopt when the path is unsafe, missing or unreadable.
    std::optional<WebAsset> read(std::string_view url_path) const;

    bool embedded() const { return embedded_; }

private:
    std::string disk_root_;
    bool embedded_ = false;
};

} // namespace edgedesk
