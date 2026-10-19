#pragma once

#include <sys/types.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Platform-internal paths that the extension file API must never access.
//
// The list is maintained centrally in src/file/ (SPEC-0008 SEC-01) so route
// handlers, the extension runtime and the frontend never implement their own
// path policy. PathSandbox consumes the compiled set and enforces it at the
// directory-FD/openat boundary.

namespace edgedesk {

struct Config;

struct ProtectedPathEntry {
    std::string canonical; // absolute, symlink-resolved when the path exists
    bool exists = false;
    dev_t device = 0;
    ino_t inode = 0;
};

// Compiled protected-path set. Entries are deduplicated after canonicalization.
class ProtectedPaths {
public:
    ProtectedPaths() = default;

    static ProtectedPaths from_paths(const std::vector<std::string>& paths);

    bool empty() const { return entries_.empty(); }

    // True when the filesystem object is a protected entry or a descendant of
    // a protected directory (covers hard-link aliases and bind mounts with the
    // same identity).
    bool blocks_identity(dev_t device, ino_t inode) const;

    // True when the canonical absolute path is a protected path or lies below
    // a protected directory. Used for targets that do not exist yet.
    bool blocks_canonical(std::string_view canonical) const;

    const std::vector<ProtectedPathEntry>& entries() const { return entries_; }

private:
    std::vector<ProtectedPathEntry> entries_;
    // Sorted (device, inode) identities of every object in the protected
    // trees at construction time. Descendants are indexed so that a hard link
    // placed outside the protected tree cannot alias a protected file.
    std::vector<std::pair<dev_t, ino_t>> identities_;
};

// Computes the protected host paths from the effective configuration. At
// minimum: data dir (auth/settings), state dir (runtime state), apps root
// (SQLite metadata plus revisions/transfers), audit log and its rotated
// siblings, config file and the optional auth token file.
std::vector<std::string> platform_protected_paths(const Config& config);

} // namespace edgedesk
