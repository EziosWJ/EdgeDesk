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

    static constexpr int schema_version() { return 1; }

private:
    SupervisorStore(std::string database_path, sqlite3* db) : database_path_(std::move(database_path)), db_(db) {}
    Error initialize_schema();
    Error apply_schema_v1();
    Error set_user_version(int version);

    std::string database_path_;
    sqlite3* db_ = nullptr;
};

// Private directory helper shared with tests. The path is walked component by
// component with directory descriptors (openat/mkdirat) and must be absolute
// without ".", "..", symlink or non-directory components. Missing components are
// created as 0700. Only the final configured directory is verified to be owned
// by the effective user and tightened to 0700; ancestors are never modified.
bool ensure_private_directory(const std::string& path, std::string& error);
Error sqlite_error_to_error(int code, const std::string& context);

} // namespace edgedesk::supervisor
