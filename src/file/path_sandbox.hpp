#pragma once

#include "core/error.hpp"
#include "core/fd.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

// FD-based path sandbox (ADR-0004).
// All file operations must resolve paths through this class; individual routes
// must never re-implement realpath / ".." / symlink checks.

namespace edgedesk {

class PathSandbox {
public:
    // root must exist and be a directory. follow_symlinks enables in-sandbox
    // symlink resolution (default off).
    static std::unique_ptr<PathSandbox> create(const std::string& root, bool follow_symlinks, Error& error);

    PathSandbox(const PathSandbox&) = delete;
    PathSandbox& operator=(const PathSandbox&) = delete;
    ~PathSandbox();

    const std::string& root() const { return root_; }
    bool follow_symlinks() const { return follow_symlinks_; }

    // Normalizes a logical path ("/a/b"). Rejects traversal above the root,
    // NUL bytes, over-long paths. Logical "/" is always valid.
    static bool normalize(std::string_view path, std::string& out, std::string& error_message);

    // Opens the parent directory of the final component. out_dir is O_PATH.
    Error open_parent(std::string_view path, UniqueFd& out_dir, std::string& out_name) const;

    // Opens the directory the path points to (or the root for "/").
    Error open_dir(std::string_view path, UniqueFd& out_fd) const;

    // Opens the final component. O_NOFOLLOW is applied internally unless the
    // sandbox is configured to follow symlinks (which still cannot escape).
    Error open_file(std::string_view path, int flags, UniqueFd& out_fd, mode_t create_mode = 0644) const;

    // Metadata of the final component without following symlinks.
    Error stat(std::string_view path, struct stat& out) const;

    enum class NodeKind { regular_file, directory, symlink, other };

    struct TreeEntry {
        std::string path; // relative to the root, '/'-separated, no leading '/'
        NodeKind kind = NodeKind::other;
        std::uint64_t size = 0;
    };

    // Recursively enumerates every node below the root using directory file
    // descriptors (openat/fdopendir/fstatat). Symlinks are reported but never
    // followed or descended into; special files are reported as `other`.
    // The root itself is not included.
    Error enumerate_tree(std::vector<TreeEntry>& out) const;

    // Duplicates the root directory fd.
    Error open_root(UniqueFd& out_fd) const;

private:
    PathSandbox(std::string root, UniqueFd root_fd, bool follow_symlinks);

    Error walk(const std::string& normalized, int depth, UniqueFd& out_dir, std::string& out_name) const;
    Error resolve_link_target(std::string_view logical_path, std::string& out_target_path, int depth) const;
    Error open_file_depth(std::string_view path, int flags, UniqueFd& out_fd, mode_t create_mode, int depth) const;

    std::string root_;
    UniqueFd root_fd_;
    bool follow_symlinks_ = false;
};

} // namespace edgedesk
