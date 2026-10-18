#pragma once

#include "core/error.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace edgedesk {

class PathSandbox;

// In-process advisory locks for all EdgeDesk file operations (ADR-0013).
//
// Identity combines the normalized path with filesystem object identity:
// existing targets use (st_dev, st_ino), missing targets use the parent
// directory identity plus the final name. Locks are non-blocking and are
// released on every success, failure, cancellation and process exit.
class FileLockManager {
public:
    // Returns a non-zero token, or ErrorCode::busy when the lock is held.
    Error acquire_shared(const PathSandbox& sandbox, std::string_view path, std::uint64_t& token);
    Error acquire_exclusive(const PathSandbox& sandbox, std::string_view path, std::uint64_t& token);

    // Locks several paths atomically in identity order (rename). On failure no
    // lock is retained.
    Error acquire_exclusive_many(const PathSandbox& sandbox, const std::vector<std::string>& paths,
                                 std::vector<std::uint64_t>& tokens);

    void release(std::uint64_t token);

    std::size_t active_locks() const;

private:
    struct Entry {
        int shared = 0;
        bool exclusive = false;
        std::uint64_t exclusive_token = 0;
    };

    Error identity(const PathSandbox& sandbox, std::string_view path, std::string& key) const;
    Error acquire(const PathSandbox& sandbox, std::string_view path, bool exclusive, std::uint64_t& token);
    void release_locked(std::uint64_t token);

    mutable std::mutex mutex_;
    std::map<std::string, Entry> locks_;
    std::map<std::uint64_t, std::string> tokens_;
    std::uint64_t next_token_ = 1;
};

} // namespace edgedesk
