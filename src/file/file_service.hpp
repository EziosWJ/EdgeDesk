#pragma once

#include "core/error.hpp"
#include "core/fd.hpp"
#include "file/file_types.hpp"
#include "file/path_sandbox.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// All file operations funnel through FileService -> PathSandbox -> POSIX.
// HTTP routes must not perform their own path handling.

namespace edgedesk {

class FileLockManager;

struct FileServiceOptions {
    std::uint64_t max_upload_size = 256ull * 1024 * 1024;
    std::uint64_t max_text_edit_size = 1ull * 1024 * 1024;
    std::uint64_t max_list_entries = 100000;
    std::uint32_t max_delete_entries = 100000;
    int max_delete_depth = 64;
};

class FileService {
public:
    explicit FileService(PathSandbox& sandbox);
    FileService(PathSandbox& sandbox, FileServiceOptions options);

    const PathSandbox& sandbox() const { return sandbox_; }
    const FileServiceOptions& options() const { return options_; }

    // Injected by Application. When unset (unit tests), operations run
    // without advisory locking.
    void set_lock_manager(FileLockManager* locks) { locks_ = locks; }

    Error list(const ListOptions& options, ListResult& result) const;

    // Metadata of a single entry (never follows the final symlink).
    Error stat(std::string_view path, FileEntry& entry) const;

    Error mkdir(std::string_view path) const;

    // overwrite=false refuses to replace an existing destination.
    Error rename(std::string_view from, std::string_view to, bool overwrite) const;

    Error remove(const std::vector<std::string>& paths, RemoveResult& result) const;

    // UTF-8 text read: rejects binaries and files above the edit limit.
    Error read_text(std::string_view path, std::string& content, FileEntry& info) const;

    // Atomic UTF-8 save: temp file in the same directory, fsync, rename;
    // keeps the previous permission bits when the file already existed.
    Error write_text(std::string_view path, std::string_view content, bool create_if_missing, FileEntry& info) const;

    struct ReadHandle {
        ReadHandle() = default;
        ReadHandle(ReadHandle&& other) noexcept;
        ReadHandle& operator=(ReadHandle&& other) noexcept;
        ReadHandle(const ReadHandle&) = delete;
        ReadHandle& operator=(const ReadHandle&) = delete;
        ~ReadHandle();

        UniqueFd fd;
        std::uint64_t size = 0;
        std::int64_t mtime = 0;
        std::uint32_t mode = 0;
        std::string mime;
        FileLockManager* lock_manager = nullptr;
        std::uint64_t lock_token = 0;
    };
    // Opens a regular file for streaming download; never loads it into memory.
    Error open_for_read(std::string_view path, ReadHandle& handle) const;

    // Streaming upload: begin -> append* -> finish (atomic rename) or abort.
    class Upload {
    public:
        Upload() = default;
        Upload(Upload&& other) noexcept;
        Upload& operator=(Upload&& other) noexcept;
        Upload(const Upload&) = delete;
        Upload& operator=(const Upload&) = delete;
        ~Upload();

        Error append(const char* data, std::size_t length);
        Error finish(FileEntry& info);
        void abort();

        std::uint64_t bytes_written() const { return written_; }
        const std::string& destination() const { return destination_; }
        const std::string& temporary_name() const { return temp_name_; }
        const std::string& directory_path() const { return directory_path_; }
        bool active() const { return fd_.valid(); }

    private:
        friend class FileService;
        void release_lock();
        UniqueFd dir_fd_;
        UniqueFd fd_;
        std::string temp_name_;
        std::string final_name_;
        std::string destination_;
        std::string directory_path_;
        std::uint32_t final_mode_ = 0;
        std::uint64_t written_ = 0;
        std::uint64_t limit_ = 0;
        std::uint64_t text_limit_ = 0;
        bool finished_ = false;
        FileLockManager* lock_manager_ = nullptr;
        std::uint64_t lock_token_ = 0;
        // Best-effort external-modification check (ADR-0013 §5). EdgeDesk's
        // own writers are excluded by the lock; this only detects outside
        // processes and cannot close the final check-to-rename race.
        bool target_existed_ = false;
        std::uint64_t target_dev_ = 0;
        std::uint64_t target_ino_ = 0;
        std::uint64_t target_size_ = 0;
        std::int64_t target_mtime_sec_ = 0;
        std::int64_t target_mtime_nsec_ = 0;
    };

    Error begin_upload(std::string_view path, Upload& upload) const;

private:
    Error entry_from_stat(const std::string& name, const std::string& logical_path, const struct stat& info,
                          FileEntry& entry) const;
    Error remove_tree(int dir_fd, const std::string& name, const std::string& logical_path, int depth,
                      std::uint32_t& counters_files, std::uint32_t& counters_dirs) const;

    PathSandbox& sandbox_;
    FileServiceOptions options_;
    FileLockManager* locks_ = nullptr;
};

} // namespace edgedesk
