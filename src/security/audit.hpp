#pragma once

#include "core/error.hpp"

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

// Lightweight JSON-lines audit log with size rotation (ADR-0003).
// detail/path pass through security/redaction at the sink: credentials are
// replaced with [REDACTED], and each field is bounded/UTF-8-safe.

namespace edgedesk {

struct AuditOptions {
    std::string path;
    std::uint64_t max_bytes = 1ull * 1024 * 1024;
    int max_files = 2;
    bool enabled = true;
};

class AuditLog {
public:
    // Returns nullptr when disabled. Fails when the log cannot be opened.
    static std::unique_ptr<AuditLog> create(const AuditOptions& options, Error& error);

    AuditLog(const AuditLog&) = delete;
    AuditLog& operator=(const AuditLog&) = delete;
    ~AuditLog();

    // detail/ip/path are optional; values are JSON-escaped.
    void record(std::string_view event, std::string_view outcome, std::string_view detail = {},
                std::string_view ip = {}, std::string_view path = {});

    // Drops records older than `cutoff_seconds` from the active log and all
    // rotated files. Rewrites are atomic per file; unparseable lines are kept.
    // Applies the configured retention policy from the maintenance thread.
    bool prune_older_than(std::int64_t cutoff_seconds, std::size_t& removed, std::string& error);

    bool enabled() const { return enabled_; }
    const std::string& path() const { return options_.path; }

private:
    explicit AuditLog(AuditOptions options) : options_(std::move(options)) {}

    void rotate_locked();
    void open_locked(bool append);

    AuditOptions options_;
    bool enabled_ = true;
    std::mutex mutex_;
    std::ofstream stream_;
    std::uint64_t written_ = 0;
};

} // namespace edgedesk
