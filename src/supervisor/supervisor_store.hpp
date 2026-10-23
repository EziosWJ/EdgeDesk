#pragma once

#include "core/error.hpp"
#include "supervisor/supervisor_types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

// Supervisor-owned SQLite store. This is deliberately independent from the
// extension platform's extensions.db (SPEC-0009 §8.1): schema, migrations and
// failure handling are local to the daemon.
//
// Corruption, permission errors and newer schema versions fail startup instead
// of clearing or recreating the database.

namespace edgedesk::supervisor {

class SupervisorStore {
public:
    static std::unique_ptr<SupervisorStore> open(const std::string& database_path, Error& error);

    SupervisorStore(const SupervisorStore&) = delete;
    SupervisorStore& operator=(const SupervisorStore&) = delete;
    ~SupervisorStore();

    const std::string& database_path() const { return database_path_; }

    Error load_all(std::vector<ServiceRecord>& out) const;
    Error get(std::string_view id, ServiceRecord& out, bool& found) const;
    // Assigns id/created_at/updated_at when empty and writes the record.
    Error insert(ServiceRecord& record);
    Error update(const ServiceConfig& config);
    Error remove(std::string_view id);
    Error save_runtime(std::string_view id, const ServiceRuntime& runtime);
    Error count(std::uint64_t& out) const;

    // Durable "previous persistence writes failed" marker. It lives next to the
    // database and is created through the verified directory descriptor so a
    // path swap cannot redirect it. Startup uses it to refuse auto-starting
    // services from a runtime row that may not reflect the last transition
    // (SPEC-0010 PSH-04).
    Error mark_inconsistent(std::string_view reason);
    bool inconsistent_marker_present() const;
    Error clear_inconsistent_marker();

    static constexpr int schema_version() { return 2; }

private:
    SupervisorStore(std::string database_path, sqlite3* db, int directory_fd)
        : database_path_(std::move(database_path)), db_(db), directory_fd_(directory_fd) {}
    Error initialize_schema();
    Error apply_schema_v2();
    Error migrate_schema_v1();
    Error set_user_version(int version);

    std::string database_path_;
    sqlite3* db_ = nullptr;
    // Pins the data directory inode for the connection lifetime. The SQLite
    // filename is opened through /proc/self/fd/<directory_fd_>/<basename>, so
    // renaming or replacing the configured path cannot move the database.
    int directory_fd_ = -1;
};

// Private directory helper shared with tests. The path is walked component by
// component with directory descriptors (openat/mkdirat) and must be absolute
// without ".", "..", symlink or non-directory components. Missing components are
// created as 0700. Only the final configured directory is verified to be owned
// by the effective user and tightened to 0700; ancestors are never modified.
//
// When `directory_fd` is non-null it receives an open descriptor for the final
// directory (caller closes it). Keeping that descriptor open pins the directory
// inode so later file operations use openat instead of re-resolving the path.
bool ensure_private_directory(const std::string& path, std::string& error,
                              int* directory_fd = nullptr);
Error sqlite_error_to_error(int code, const std::string& context);

} // namespace edgedesk::supervisor
