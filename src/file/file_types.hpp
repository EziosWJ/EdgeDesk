#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Shared value types for the file module. No libhv / HTTP types here.

namespace edgedesk {

struct FileEntry {
    std::string name;
    std::string path; // logical sandbox path, e.g. "/etc/hostname"
    bool is_dir = false;
    bool is_file = false;
    bool is_symlink = false;
    bool hidden = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0; // unix seconds
    std::uint32_t mode = 0; // permission bits (0777 mask)
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    std::string link_target; // when is_symlink
    std::string mime;
    bool text_editable = false; // regular text file within edit limits
};

struct ListOptions {
    std::string path = "/";
    int page = 1;
    int page_size = 200;
    std::string sort = "name"; // name | size | mtime | type
    bool desc = false;
    bool show_hidden = true;
    // Directory-tree helper: keep only real subdirectories of `path`.
    // Symlinks to directories are deliberately excluded: the tree never
    // follows links, so it must not offer them as expandable nodes.
    bool dirs_only = false;
};

struct ListResult {
    std::string path;
    std::vector<FileEntry> entries;
    std::uint64_t total = 0;
    int page = 1;
    int page_size = 200;
    bool truncated = false; // directory had more entries than the scan cap
};

struct RemoveResult {
    std::uint32_t removed_files = 0;
    std::uint32_t removed_dirs = 0;
};

// Inclusive byte range for HTTP Range downloads.
struct ContentRange {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint64_t total = 0;
};

enum class RangeStatus { full, partial, unsatisfiable, malformed };

// Parses a single-range "bytes=a-b" header. Multi-range requests fall back to
// `malformed` (server then answers with the full body per RFC 7233 guidance).
RangeStatus parse_range_header(std::string_view header, std::uint64_t file_size, ContentRange& out);

// Content type by file name (broader than the web asset list).
std::string mime_for_file_name(std::string_view name);

// Heuristic: content type is text-like and content is valid UTF-8 without NUL.
bool is_text_mime(std::string_view mime);
bool looks_like_utf8_text(std::string_view sample);

} // namespace edgedesk
