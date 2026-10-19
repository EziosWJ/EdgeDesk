#pragma once

#include "core/error.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

// Bounded per-service log files. Rotation keeps at most one previous file, so
// disk usage is bounded at roughly 2 * log_max_bytes per service. Reads are
// bounded and cursor based; a rotating file reports `reset` when the cursor no
// longer refers to the current generation.

namespace edgedesk::supervisor {

struct LogReadResult {
    std::string content;
    std::uint64_t cursor = 0;
    std::uint64_t next_cursor = 0;
    std::uint64_t size = 0;
    bool reset = false;
    bool truncated = false;
};

class LogStore {
public:
    static std::unique_ptr<LogStore> open(const std::string& directory, Error& error);

    LogStore(const LogStore&) = delete;
    LogStore& operator=(const LogStore&) = delete;
    ~LogStore();

    const std::string& directory() const { return directory_; }

    // Appends captured output, rotating when the file would exceed max_bytes.
    // `data` is truncated to the last max_bytes if a single chunk is too large.
    Error append(std::string_view id, std::string_view data, std::uint64_t max_bytes);
    Error read(std::string_view id, std::uint64_t tail_bytes, bool has_cursor,
               std::uint64_t cursor, std::uint64_t max_bytes, LogReadResult& out) const;
    Error remove(std::string_view id);
    void close(std::string_view id);

private:
    explicit LogStore(std::string directory) : directory_(std::move(directory)) {}
    struct OpenFile {
        int fd = -1;
        std::uint64_t size = 0;
    };
    std::string path_for(std::string_view id) const;
    Error open_append_locked(std::string_view id, OpenFile& out);
    static bool valid_id(std::string_view id);

    std::string directory_;
    std::map<std::string, OpenFile> files_;
};

} // namespace edgedesk::supervisor
