#pragma once

#include "core/error.hpp"
#include "extensions/extension_store.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Minimal, defensive ZIP reader for application packages.
//
// Supported: stored and deflate entries, ZIP64 central directories, UTF-8
// names. Rejected: encrypted entries, symlinks, non-regular files, absolute
// paths, '..' traversal, backslashes, duplicate names (case-insensitive) and
// any local/central header disagreement.

namespace edgedesk::extensions {

struct ZipEntry {
    std::string name; // normalized relative path; directories end with '/'
    std::uint16_t method = 0;
    std::uint32_t crc32 = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t local_header_offset = 0;
    std::uint16_t flags = 0;
    std::uint32_t external_attributes = 0;
    bool directory = false;
};

// Compatibility view of an extracted regular file. The install path uses the
// integrity manifest directly to avoid keeping a second copy of every path.
struct ExtractedFile {
    std::string path;
    std::uint64_t size = 0;
    std::string sha256;
    std::string mime;
    bool text_editable = false;
};

class ZipReader {
public:
    // Opens and validates the central directory. No file content is read.
    static std::unique_ptr<ZipReader> open(const std::string& path, Error& error);

    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    const std::vector<ZipEntry>& entries() const { return entries_; }
    std::uint64_t file_size() const { return file_size_; }

    // Extracts every entry below `dir_fd` (an opened directory), creates
    // parent directories with 0700, files with 0600 and computes SHA-256.
    // On failure the caller owns cleanup of the temporary directory.
    //
    // The install path uses this overload: the integrity manifest is the only
    // per-file metadata kept, so paths and digests are not duplicated.
    Error extract_all(int dir_fd, std::vector<IntegrityEntry>& integrity,
                      std::uint64_t& total_bytes) const;

    // Compatibility overload for callers that need the older per-file view.
    // It builds ExtractedFile records from the integrity manifest.
    Error extract_all(int dir_fd, std::vector<ExtractedFile>& files,
                      std::vector<IntegrityEntry>& integrity, std::uint64_t& total_bytes) const;

    // Verifies every file of an installed revision against its integrity
    // manifest. Returns false with `mismatch_path` on the first difference.
    static Error verify_revision(const std::string& revision_root,
                                 const std::vector<IntegrityEntry>& entries,
                                 std::string& mismatch_path);

private:
    ZipReader(std::string path, std::uint64_t file_size) : path_(std::move(path)), file_size_(file_size) {}
    Error load_entries(int fd);

    std::string path_;
    std::uint64_t file_size_ = 0;
    std::vector<ZipEntry> entries_;
};

bool validate_entry_name(std::string_view name, std::string& normalized, bool& directory, std::string& error);

} // namespace edgedesk::extensions
