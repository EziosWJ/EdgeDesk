#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace edgedesk {

struct Asset {
    std::string body;
    std::string mime;
    std::string etag;
};

struct EmbeddedAsset {
    const char* path;
    const unsigned char* bytes;
    std::size_t size;
    const char* mime;
    const char* etag;
};

#if EDGEDESK_EMBED_ASSETS
// Defined only by the generated #embed translation unit.
const EmbeddedAsset* find_embedded_asset(std::string_view path);
#endif

bool is_safe_asset_path(std::string_view path);
std::string mime_for_path(std::string_view path);

class AssetProvider {
public:
    explicit AssetProvider(std::string disk_root);
    std::optional<Asset> read(std::string_view url_path) const;

private:
    std::string disk_root_;
};

} // namespace edgedesk
