#pragma once

#include "core/error.hpp"
#include "extensions/extension_store.hpp"
#include "extensions/manifest.hpp"
#include "file/file_service.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace edgedesk {
class TerminalManager;
}

namespace edgedesk::extensions {

class PackageManager;

inline constexpr std::uint64_t kTransferChunkBytes = 1024 * 1024;
inline constexpr int kListPageDefault = 100;
inline constexpr int kListPageMax = 500;
inline constexpr std::int64_t kRuntimeSessionTtlSeconds = 12 * 3600;
inline constexpr std::int64_t kTransferIdleTimeoutMs = 5 * 60 * 1000;

struct RuntimeSession {
    std::string token;
    std::string app_id;
    std::string instance_id;
    std::string revision_id;
    std::string version;
    std::vector<std::string> permissions;
    std::vector<std::string> origins;
    std::int64_t created_at = 0;
};

struct TransferInfo {
    std::string transfer_id;
    std::uint64_t chunk_size = kTransferChunkBytes;
    std::uint64_t total_size = 0;
};

struct ListPage {
    std::string path;
    std::vector<FileEntry> entries;
    std::string next_cursor;
    bool stale = false;
};

// Runtime state for sandboxed applications. Sessions snapshot the granted
// permission set at window creation; revocation is therefore delayed until the
// next instance. Disable/upgrade/uninstall/integrity isolation invalidates all
// sessions for the app immediately (the route calls invalidate_app()).
class ExtensionRuntime {
public:
    ExtensionRuntime(ExtensionStore& store, PackageManager& packages, FileService& files,
                     TerminalManager* terminals = nullptr);
    ~ExtensionRuntime();

    ExtensionRuntime(const ExtensionRuntime&) = delete;
    ExtensionRuntime& operator=(const ExtensionRuntime&) = delete;

    // Backing file service for every extension files.* operation. It carries
    // the platform-internal protected-path policy; routes must not use the
    // administrator file service for extension calls.
    FileService& files() { return files_; }
    const FileService& files() const { return files_; }

    Error create_session(std::string_view app_id, std::string_view instance_id, RuntimeSession& out);
    Error validate(std::string_view token, RuntimeSession& out);
    void remove_session(std::string_view token);
    void invalidate_app(std::string_view app_id);
    void invalidate_revision(std::string_view revision_id);
    void sweep(std::int64_t now_ms);

    // Terminal sessions created through a runtime instance are terminated when
    // that window closes or the app is disabled/upgraded/removed.
    void track_terminal(std::string_view token, std::string_view session_id);
    void untrack_terminal(std::string_view token, std::string_view session_id);

    static bool has_permission(const RuntimeSession& session, Permission permission);
    static Error require_permission(const RuntimeSession& session, Permission permission);

    // Cursor payload is opaque to applications and HMAC-bound to app and kind.
    std::string encode_cursor(std::string_view app_id, std::string_view kind,
                              const std::string& payload) const;
    bool decode_cursor(std::string_view app_id, std::string_view kind, std::string_view cursor,
                       std::string& payload) const;

    Error list_directory(const RuntimeSession& session, std::string_view path,
                         std::string_view cursor, int limit, ListPage& out) const;

    Error begin_upload(const RuntimeSession& session, std::string_view path, std::uint64_t expected_size,
                       TransferInfo& out);
    Error append_upload(const RuntimeSession& session, std::string_view transfer_id,
                        std::uint64_t offset, const char* data, std::size_t length,
                        std::uint64_t& received);
    Error finish_upload(const RuntimeSession& session, std::string_view transfer_id,
                        std::uint64_t expected_size, FileEntry& entry);
    Error cancel_upload(const RuntimeSession& session, std::string_view transfer_id);

    Error begin_download(const RuntimeSession& session, std::string_view path, TransferInfo& out);
    Error read_download(const RuntimeSession& session, std::string_view transfer_id,
                        std::uint64_t offset, std::string& data, bool& eof);
    Error finish_download(const RuntimeSession& session, std::string_view transfer_id);
    Error cancel_download(const RuntimeSession& session, std::string_view transfer_id);

private:
    struct UploadTask {
        UploadTask() = default;
        UploadTask(UploadTask&&) noexcept = default;
        UploadTask& operator=(UploadTask&&) noexcept = default;
        UploadTask(const UploadTask&) = delete;
        UploadTask& operator=(const UploadTask&) = delete;
        ~UploadTask();
        std::string token;
        std::string app_id;
        std::string instance_id;
        std::string path;
        std::string journal_path;
        std::uint64_t expected_size = 0;
        std::uint64_t received = 0;
        FileService::Upload upload;
        std::int64_t last_activity_ms = 0;
    };
    struct DownloadTask {
        std::string token;
        std::string app_id;
        std::string instance_id;
        std::string path;
        std::uint64_t offset = 0;
        FileService::ReadHandle handle;
        std::int64_t last_activity_ms = 0;
    };

    static Error cursor_stale();
    bool find_session_locked(std::string_view token, RuntimeSession& out) const;
    std::vector<std::string> erase_session_locked(std::string_view token);
    void close_terminals(const std::vector<std::string>& session_ids);
    Error write_transfer_journal(UploadTask& task, std::string_view transfer_id) const;
    void recover_transfer_journal();
    Error transfer_owner_check(const RuntimeSession& session, const UploadTask& task) const;
    Error transfer_owner_check(const RuntimeSession& session, const DownloadTask& task) const;

    ExtensionStore& store_;
    PackageManager& packages_;
    FileService& files_;
    TerminalManager* terminals_ = nullptr;
    std::string cursor_key_;
    std::string journal_directory_;

    mutable std::mutex mutex_;
    std::map<std::string, RuntimeSession> sessions_;
    std::map<std::string, std::set<std::string>> terminal_sessions_;
    std::map<std::string, UploadTask> uploads_;
    std::map<std::string, DownloadTask> downloads_;
};

} // namespace edgedesk::extensions
