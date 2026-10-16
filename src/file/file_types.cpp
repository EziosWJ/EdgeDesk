#include "file/file_types.hpp"

#include "core/types.hpp"
#include "core/util.hpp"

#include <algorithm>
#include <cstring>

namespace edgedesk {
namespace {

struct ExtensionMime {
    const char* extension;
    const char* mime;
};

const ExtensionMime kKnownMimes[] = {
    {".html", "text/html"},          {".htm", "text/html"},
    {".css", "text/css"},            {".js", "text/javascript"},
    {".mjs", "text/javascript"},     {".json", "application/json"},
    {".map", "application/json"},    {".txt", "text/plain"},
    {".log", "text/plain"},          {".md", "text/markdown"},
    {".csv", "text/csv"},            {".tsv", "text/tab-separated-values"},
    {".xml", "application/xml"},     {".yml", "application/yaml"},
    {".yaml", "application/yaml"},   {".toml", "application/toml"},
    {".ini", "text/plain"},          {".conf", "text/plain"},
    {".cfg", "text/plain"},          {".service", "text/plain"},
    {".sh", "text/x-shellscript"},   {".bash", "text/x-shellscript"},
    {".py", "text/x-python"},        {".c", "text/x-c"},
    {".h", "text/x-c"},              {".hpp", "text/x-c++"},
    {".cc", "text/x-c++"},           {".cpp", "text/x-c++"},
    {".rs", "text/x-rust"},          {".go", "text/x-go"},
    {".java", "text/x-java"},        {".lua", "text/x-lua"},
    {".sql", "text/x-sql"},          {".env", "text/plain"},
    {".desktop", "text/plain"},      {".svg", "image/svg+xml"},
    {".png", "image/png"},           {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},         {".gif", "image/gif"},
    {".webp", "image/webp"},         {".ico", "image/x-icon"},
    {".bmp", "image/bmp"},           {".pdf", "application/pdf"},
    {".gz", "application/gzip"},     {".xz", "application/x-xz"},
    {".zip", "application/zip"},     {".tar", "application/x-tar"},
    {".tgz", "application/gzip"},    {".7z", "application/x-7z-compressed"},
    {".bz2", "application/x-bzip2"}, {".zst", "application/zstd"},
    {".bin", "application/octet-stream"},
    {".so", "application/x-sharedlib"},
    {".a", "application/x-archive"},
    {".o", "application/x-object"},
    {".db", "application/octet-stream"},
    {".sqlite", "application/octet-stream"},
    {".woff", "font/woff"},          {".woff2", "font/woff2"},
    {".ttf", "font/ttf"},            {".otf", "font/otf"},
    {".mp3", "audio/mpeg"},          {".mp4", "video/mp4"},
};

std::string extension_of(std::string_view name) {
    const auto slash = name.rfind('/');
    const auto dot = name.rfind('.');
    if (dot == name.npos) return {};
    if (slash != name.npos && dot < slash) return {};
    return to_lower_ascii(name.substr(dot));
}

} // namespace

RangeStatus parse_range_header(std::string_view header, std::uint64_t file_size, ContentRange& out) {
    const auto trimmed = trim(header);
    constexpr std::string_view prefix = "bytes=";
    if (trimmed.rfind(prefix, 0) != 0) return RangeStatus::malformed;
    const auto spec = trimmed.substr(prefix.size());
    if (spec.find(',') != std::string::npos) return RangeStatus::malformed; // multi-range unsupported
    const auto dash = spec.find('-');
    if (dash == std::string::npos) return RangeStatus::malformed;
    const auto start_text = trim(spec.substr(0, dash));
    const auto end_text = trim(spec.substr(dash + 1));

    std::uint64_t start = 0;
    std::uint64_t end = 0;
    if (start_text.empty()) {
        // Suffix range: bytes=-N
        std::uint64_t suffix = 0;
        if (!parse_uint64(end_text, suffix) || suffix == 0) return RangeStatus::unsatisfiable;
        if (file_size == 0) return RangeStatus::unsatisfiable;
        start = suffix >= file_size ? 0 : file_size - suffix;
        end = file_size - 1;
    } else {
        if (!parse_uint64(start_text, start)) return RangeStatus::malformed;
        if (end_text.empty()) {
            end = file_size == 0 ? 0 : file_size - 1;
        } else if (!parse_uint64(end_text, end)) {
            return RangeStatus::malformed;
        }
        if (file_size == 0 || start >= file_size) return RangeStatus::unsatisfiable;
        if (end >= file_size) end = file_size - 1;
        if (end < start) return RangeStatus::unsatisfiable;
    }
    out.start = start;
    out.end = end;
    out.total = file_size;
    return RangeStatus::partial;
}

std::string mime_for_file_name(std::string_view name) {
    const auto extension = extension_of(name);
    if (!extension.empty()) {
        for (const auto& item : kKnownMimes) {
            if (extension == item.extension) return item.mime;
        }
    }
    return "application/octet-stream";
}

bool is_text_mime(std::string_view mime) {
    if (mime.rfind("text/", 0) == 0) return true;
    static const char* kTextLike[] = {
        "application/json", "application/xml",     "application/yaml", "application/toml",
        "application/javascript", "application/x-httpd-php", "application/x-shellscript",
    };
    for (const auto* candidate : kTextLike) {
        if (mime == candidate) return true;
    }
    return false;
}

bool looks_like_utf8_text(std::string_view sample) {
    std::size_t i = 0;
    while (i < sample.size()) {
        const auto byte = static_cast<unsigned char>(sample[i]);
        if (byte == 0x00) return false;
        if (byte < 0x80) {
            ++i;
            continue;
        }
        int continuation = 0;
        unsigned int codepoint = 0;
        if ((byte & 0xe0) == 0xc0) {
            continuation = 1;
            codepoint = byte & 0x1f;
        } else if ((byte & 0xf0) == 0xe0) {
            continuation = 2;
            codepoint = byte & 0x0f;
        } else if ((byte & 0xf8) == 0xf0) {
            continuation = 3;
            codepoint = byte & 0x07;
        } else {
            return false;
        }
        if (i + static_cast<std::size_t>(continuation) >= sample.size()) {
            // Possibly truncated multi-byte sequence at the sample boundary.
            return true;
        }
        for (int k = 1; k <= continuation; ++k) {
            const auto next = static_cast<unsigned char>(sample[i + static_cast<std::size_t>(k)]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
        i += static_cast<std::size_t>(continuation) + 1;
    }
    return true;
}

} // namespace edgedesk
