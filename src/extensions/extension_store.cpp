#include "extensions/extension_store.hpp"

#include "core/util.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

namespace edgedesk::extensions {
namespace {

constexpr int kSchemaVersion = 3;

class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql, Error& error) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
            error = sqlite_error_to_error(sqlite3_errcode(db), "prepare: " + sql + ": " + sqlite3_errmsg(db));
            stmt_ = nullptr;
        } else {
            error = Error::none();
        }
    }
    ~Stmt() {
        if (stmt_ != nullptr) sqlite3_finalize(stmt_);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool ok() const { return stmt_ != nullptr; }
    sqlite3_stmt* get() const { return stmt_; }

    void text(int index, std::string_view value) {
        sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }
    void null_text(int index) { sqlite3_bind_null(stmt_, index); }
    void int64(int index, std::int64_t value) { sqlite3_bind_int64(stmt_, index, value); }

    // Returns SQLITE_ROW / SQLITE_DONE or an error code.
    int step() { return sqlite3_step(stmt_); }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

std::string column_text(sqlite3_stmt* stmt, int index) {
    const auto* text = sqlite3_column_text(stmt, index);
    if (text == nullptr) return {};
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<std::size_t>(sqlite3_column_bytes(stmt, index)));
}

std::int64_t column_int64(sqlite3_stmt* stmt, int index) {
    return static_cast<std::int64_t>(sqlite3_column_int64(stmt, index));
}

Error step_result(sqlite3* db, int result, const std::string& context) {
    if (result == SQLITE_DONE || result == SQLITE_ROW) return Error::none();
    return sqlite_error_to_error(result, context + ": " + sqlite3_errmsg(db));
}

Error exec_stmt(sqlite3* db, const std::string& sql, const std::string& context) {
    char* message = nullptr;
    const int result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        std::string text = message != nullptr ? message : sqlite3_errmsg(db);
        if (message != nullptr) sqlite3_free(message);
        return sqlite_error_to_error(result, context + ": " + text);
    }
    return Error::none();
}

bool valid_kv_key(std::string_view key, std::string& error) {
    if (key.empty()) {
        error = "key must not be empty";
        return false;
    }
    if (key.size() > kAppKvMaxKeyBytes) {
        error = "key exceeds 1024 bytes";
        return false;
    }
    if (key.find('\0') != std::string_view::npos) {
        error = "key must not contain NUL";
        return false;
    }
    error.clear();
    return true;
}

} // namespace

Error sqlite_error_to_error(int code, const std::string& context) {
    switch (code & 0xff) {
        case SQLITE_CONSTRAINT:
            return Error::make(ErrorCode::conflict, context, code);
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            return Error::make(ErrorCode::busy, context, code);
        case SQLITE_FULL:
            return Error::make(ErrorCode::too_large, context, code);
        case SQLITE_READONLY:
        case SQLITE_PERM:
        case SQLITE_AUTH:
            return Error::make(ErrorCode::permission_denied, context, code);
        case SQLITE_NOTFOUND:
            return Error::make(ErrorCode::not_found, context, code);
        case SQLITE_NOMEM:
            return Error::make(ErrorCode::unavailable, context, code);
        default:
            return Error::make(ErrorCode::io_error, context, code);
    }
}

std::string now_iso8601() { return format_iso8601_utc(now_unix_seconds()); }

bool ensure_private_directory(const std::string& path, std::string& error) {
    if (path.empty() || path.front() != '/') {
        error = "application data root must be an absolute path";
        return false;
    }
    std::string current = "/";
    std::size_t start = 1;
    for (;;) {
        const auto slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? path.size() - start : slash - start);
        if (!part.empty()) {
            if (current.size() > 1) current.push_back('/');
            current += part;
            struct stat info {};
            if (::lstat(current.c_str(), &info) != 0) {
                if (errno != ENOENT) {
                    error = "cannot inspect directory " + current + ": " + std::strerror(errno);
                    return false;
                }
                if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
                    error = "cannot create directory " + current + ": " + std::strerror(errno);
                    return false;
                }
                if (::lstat(current.c_str(), &info) != 0) {
                    error = "cannot inspect created directory " + current + ": " + std::strerror(errno);
                    return false;
                }
            }
            if (!S_ISDIR(info.st_mode)) {
                error = "path component is not a directory: " + current;
                return false;
            }
            const bool is_final = current.size() == path.size();
            if (is_final) {
                if (info.st_uid != ::geteuid()) {
                    error = "directory is not owned by the service user: " + current;
                    return false;
                }
                if (::chmod(current.c_str(), 0700) != 0) {
                    error = "cannot tighten directory permissions: " + current + ": " + std::strerror(errno);
                    return false;
                }
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    error.clear();
    return true;
}

std::unique_ptr<ExtensionStore> ExtensionStore::open(const std::string& root, Error& error) {
    std::string message;
    if (!ensure_private_directory(root, message)) {
        error = Error::make(ErrorCode::permission_denied, message);
        return nullptr;
    }
    const std::string database_path = root + "/extensions.db";
    sqlite3* db = nullptr;
    const int result = sqlite3_open_v2(database_path.c_str(), &db,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                       nullptr);
    if (result != SQLITE_OK) {
        const std::string sqlite_message = db != nullptr ? sqlite3_errmsg(db) : "out of memory";
        error = sqlite_error_to_error(result, "cannot open extension database: " + database_path + ": " +
                                                  sqlite_message);
        if (db != nullptr) sqlite3_close(db);
        return nullptr;
    }
    std::unique_ptr<ExtensionStore> store(new ExtensionStore(root, database_path, db));
    if (::chmod(database_path.c_str(), 0600) != 0) {
        error = Error::make(ErrorCode::permission_denied,
                            "cannot set extension database permissions: " + std::string(std::strerror(errno)));
        return nullptr;
    }
    sqlite3_busy_timeout(db, 5000);
    const Error setup = store->initialize_schema();
    if (!setup.ok()) {
        error = setup;
        return nullptr;
    }
    error = Error::none();
    return store;
}

ExtensionStore::ExtensionStore(std::string root, std::string database_path, sqlite3* db)
    : root_(std::move(root)), database_path_(std::move(database_path)), db_(db) {}

ExtensionStore::~ExtensionStore() {
    if (db_ != nullptr) {
        sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
        sqlite3_close(db_);
    }
}

Error ExtensionStore::exec_locked(std::string_view sql) {
    return exec_stmt(db_, std::string(sql), "extension database");
}

Error ExtensionStore::initialize_schema() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error = exec_locked("PRAGMA journal_mode=WAL");
    if (!error.ok()) return error;
    error = exec_locked("PRAGMA synchronous=FULL");
    if (!error.ok()) return error;
    error = exec_locked("PRAGMA foreign_keys=ON");
    if (!error.ok()) return error;

    int version = 0;
    {
        Stmt stmt(db_, "PRAGMA user_version", error);
        if (!stmt.ok()) return error;
        if (stmt.step() == SQLITE_ROW) version = static_cast<int>(column_int64(stmt.get(), 0));
    }
    if (version > kSchemaVersion) {
        return Error::make(ErrorCode::unsupported,
                           "extension database schema is newer than this EdgeDesk build");
    }
    if (version == kSchemaVersion) return Error::none();

    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS apps (
    app_id TEXT PRIMARY KEY,
    display_name TEXT NOT NULL,
    status TEXT NOT NULL,
    active_revision_id TEXT,
    active_version TEXT,
    data_retained INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS revisions (
    revision_id TEXT PRIMARY KEY,
    app_id TEXT NOT NULL,
    version TEXT NOT NULL,
    manifest_json TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    file_count INTEGER NOT NULL,
    total_bytes INTEGER NOT NULL,
    status TEXT NOT NULL,
    pinned INTEGER NOT NULL DEFAULT 0,
    integrity TEXT NOT NULL DEFAULT 'ok',
    FOREIGN KEY(app_id) REFERENCES apps(app_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS revisions_app_idx ON revisions(app_id, created_at DESC);
CREATE TABLE IF NOT EXISTS permission_requests (
    app_id TEXT NOT NULL,
    module TEXT NOT NULL,
    PRIMARY KEY(app_id, module),
    FOREIGN KEY(app_id) REFERENCES apps(app_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS permission_grants (
    app_id TEXT NOT NULL,
    module TEXT NOT NULL,
    granted_at INTEGER NOT NULL,
    PRIMARY KEY(app_id, module),
    FOREIGN KEY(app_id) REFERENCES apps(app_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS network_origins (
    app_id TEXT NOT NULL,
    origin TEXT NOT NULL,
    PRIMARY KEY(app_id, origin),
    FOREIGN KEY(app_id) REFERENCES apps(app_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS integrity_entries (
    revision_id TEXT NOT NULL,
    path TEXT NOT NULL,
    size INTEGER NOT NULL,
    sha256 TEXT NOT NULL,
    PRIMARY KEY(revision_id, path),
    FOREIGN KEY(revision_id) REFERENCES revisions(revision_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS operations (
    operation_id TEXT PRIMARY KEY,
    app_id TEXT NOT NULL,
    type TEXT NOT NULL,
    phase TEXT NOT NULL,
    temp_path TEXT,
    revision_id TEXT,
    error TEXT,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS operations_pending_idx ON operations(phase);
CREATE TABLE IF NOT EXISTS app_kv (
    app_id TEXT NOT NULL,
    key TEXT NOT NULL,
    value TEXT NOT NULL,
    size_bytes INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY(app_id, key)
);
CREATE TABLE IF NOT EXISTS app_backends (
    app_id TEXT PRIMARY KEY,
    service_id TEXT NOT NULL UNIQUE,
    executable_rel TEXT NOT NULL,
    args_json TEXT NOT NULL DEFAULT '[]',
    arch TEXT NOT NULL,
    run_user TEXT NOT NULL DEFAULT '',
    run_group TEXT NOT NULL DEFAULT '',
    allow_root INTEGER NOT NULL DEFAULT 0,
    autostart INTEGER NOT NULL DEFAULT 0,
    state TEXT NOT NULL DEFAULT 'stopped',
    error TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    FOREIGN KEY(app_id) REFERENCES apps(app_id) ON DELETE CASCADE
);
)SQL";
    error = exec_locked(schema);
    if (!error.ok()) return error;
    error = exec_locked("PRAGMA user_version=" + std::to_string(kSchemaVersion));
    return error;
}

Error ExtensionStore::load_permissions_locked(std::string_view app_id, std::vector<std::string>& requested,
                                             std::vector<std::string>& granted) const {
    requested.clear();
    granted.clear();
    Error error;
    {
        Stmt stmt(db_, "SELECT module FROM permission_requests WHERE app_id=? ORDER BY module", error);
        if (!stmt.ok()) return error;
        stmt.text(1, app_id);
        for (;;) {
            const int result = stmt.step();
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) return step_result(db_, result, "read permission requests");
            requested.push_back(column_text(stmt.get(), 0));
        }
    }
    {
        Stmt stmt(db_, "SELECT module FROM permission_grants WHERE app_id=? ORDER BY module", error);
        if (!stmt.ok()) return error;
        stmt.text(1, app_id);
        for (;;) {
            const int result = stmt.step();
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) return step_result(db_, result, "read permission grants");
            granted.push_back(column_text(stmt.get(), 0));
        }
    }
    return Error::none();
}

Error ExtensionStore::get_app(std::string_view app_id, AppRecord& out, bool& found) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return load_app_locked(app_id, out, found);
}

Error ExtensionStore::load_app_locked(std::string_view app_id, AppRecord& out, bool& found) const {
    out = {};
    found = false;
    Error error;
    Stmt stmt(db_,
              "SELECT app_id, display_name, status, active_revision_id, active_version, data_retained, "
              "created_at, updated_at FROM apps WHERE app_id=?",
              error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    const int result = stmt.step();
    if (result == SQLITE_DONE) return Error::none();
    if (result != SQLITE_ROW) return step_result(db_, result, "read application");
    found = true;
    out.app_id = column_text(stmt.get(), 0);
    out.display_name = column_text(stmt.get(), 1);
    out.status = column_text(stmt.get(), 2);
    out.active_revision_id = column_text(stmt.get(), 3);
    out.active_version = column_text(stmt.get(), 4);
    out.data_retained = column_int64(stmt.get(), 5) != 0;
    out.created_at = column_int64(stmt.get(), 6);
    out.updated_at = column_int64(stmt.get(), 7);
    Error permission_error = load_permissions_locked(out.app_id, out.requested_permissions,
                                                     out.granted_permissions);
    if (!permission_error.ok()) return permission_error;
    if (!out.active_revision_id.empty()) {
        RevisionRecord revision;
        bool revision_found = false;
        Error revision_error = get_revision(out.active_revision_id, revision, revision_found);
        if (!revision_error.ok()) return revision_error;
        if (revision_found) {
            const auto parsed = nlohmann::json::parse(revision.manifest_json, nullptr, false);
            if (parsed.is_object()) {
                out.manifest = parsed;
                out.icon = parsed.value("icon", "");
            }
        }
    }
    return Error::none();
}

Error ExtensionStore::list_apps(std::vector<AppRecord>& out, bool include_uninstalled) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    out.clear();
    const char* sql = include_uninstalled
                          ? "SELECT app_id FROM apps ORDER BY display_name COLLATE NOCASE, app_id"
                          : "SELECT app_id FROM apps WHERE status<>'uninstalled' ORDER BY display_name COLLATE NOCASE, app_id";
    Error error;
    Stmt stmt(db_, sql, error);
    if (!stmt.ok()) return error;
    for (;;) {
        const int result = stmt.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW) return step_result(db_, result, "list applications");
        AppRecord app;
        bool found = false;
        Error load_error = load_app_locked(column_text(stmt.get(), 0), app, found);
        if (!load_error.ok()) return load_error;
        if (found) out.push_back(std::move(app));
    }
    return Error::none();
}

Error ExtensionStore::upsert_app(const AppRecord& app) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_,
              "INSERT INTO apps(app_id, display_name, status, active_revision_id, active_version, "
              "data_retained, created_at, updated_at) VALUES(?,?,?,?,?,?,?,?) "
              "ON CONFLICT(app_id) DO UPDATE SET display_name=excluded.display_name, status=excluded.status, "
              "active_revision_id=excluded.active_revision_id, active_version=excluded.active_version, "
              "data_retained=excluded.data_retained, updated_at=excluded.updated_at",
              error);
    if (!stmt.ok()) return error;
    const std::int64_t now = now_unix_seconds();
    stmt.text(1, app.app_id);
    stmt.text(2, app.display_name);
    stmt.text(3, app.status);
    if (app.active_revision_id.empty()) stmt.null_text(4); else stmt.text(4, app.active_revision_id);
    if (app.active_version.empty()) stmt.null_text(5); else stmt.text(5, app.active_version);
    stmt.int64(6, app.data_retained ? 1 : 0);
    stmt.int64(7, app.created_at > 0 ? app.created_at : now);
    stmt.int64(8, now);
    return step_result(db_, stmt.step(), "upsert application");
}

Error ExtensionStore::set_app_status(std::string_view app_id, std::string_view status) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "UPDATE apps SET status=?, updated_at=? WHERE app_id=?", error);
    if (!stmt.ok()) return error;
    stmt.text(1, status);
    stmt.int64(2, now_unix_seconds());
    stmt.text(3, app_id);
    Error step_error = step_result(db_, stmt.step(), "update application status");
    if (!step_error.ok()) return step_error;
    if (sqlite3_changes(db_) == 0) return Error::make(ErrorCode::not_found, "application not found");
    return Error::none();
}

Error ExtensionStore::set_app_active_revision(std::string_view app_id, std::string_view revision_id,
                                              std::string_view version) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "UPDATE apps SET active_revision_id=?, active_version=?, updated_at=? WHERE app_id=?", error);
    if (!stmt.ok()) return error;
    if (revision_id.empty()) stmt.null_text(1); else stmt.text(1, revision_id);
    if (version.empty()) stmt.null_text(2); else stmt.text(2, version);
    stmt.int64(3, now_unix_seconds());
    stmt.text(4, app_id);
    Error step_error = step_result(db_, stmt.step(), "set active revision");
    if (!step_error.ok()) return step_error;
    if (sqlite3_changes(db_) == 0) return Error::make(ErrorCode::not_found, "application not found");
    return Error::none();
}

Error ExtensionStore::set_data_retained(std::string_view app_id, bool retained) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "UPDATE apps SET data_retained=?, updated_at=? WHERE app_id=?", error);
    if (!stmt.ok()) return error;
    stmt.int64(1, retained ? 1 : 0);
    stmt.int64(2, now_unix_seconds());
    stmt.text(3, app_id);
    return step_result(db_, stmt.step(), "set retained data flag");
}

Error ExtensionStore::clear_app_data(std::string_view app_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return run_transaction([&]() -> Error {
        Error error;
        {
            Stmt stmt(db_, "DELETE FROM app_kv WHERE app_id=?", error);
            if (!stmt.ok()) return error;
            stmt.text(1, app_id);
            Error step_error = step_result(db_, stmt.step(), "clear application kv");
            if (!step_error.ok()) return step_error;
        }
        return set_data_retained(app_id, false);
    });
}

Error ExtensionStore::delete_app_record(std::string_view app_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "DELETE FROM apps WHERE app_id=?", error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    return step_result(db_, stmt.step(), "delete application record");
}

Error ExtensionStore::get_revision(std::string_view revision_id, RevisionRecord& out, bool& found) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    out = {};
    found = false;
    Error error;
    Stmt stmt(db_,
              "SELECT revision_id, app_id, version, manifest_json, created_at, file_count, total_bytes, "
              "status, pinned, integrity FROM revisions WHERE revision_id=?",
              error);
    if (!stmt.ok()) return error;
    stmt.text(1, revision_id);
    const int result = stmt.step();
    if (result == SQLITE_DONE) return Error::none();
    if (result != SQLITE_ROW) return step_result(db_, result, "read revision");
    found = true;
    out.revision_id = column_text(stmt.get(), 0);
    out.app_id = column_text(stmt.get(), 1);
    out.version = column_text(stmt.get(), 2);
    out.manifest_json = column_text(stmt.get(), 3);
    out.created_at = column_int64(stmt.get(), 4);
    out.file_count = static_cast<std::uint64_t>(column_int64(stmt.get(), 5));
    out.total_bytes = static_cast<std::uint64_t>(column_int64(stmt.get(), 6));
    out.status = column_text(stmt.get(), 7);
    out.pinned = column_int64(stmt.get(), 8) != 0;
    out.integrity = column_text(stmt.get(), 9);
    out.active = out.status == revision_status::active;
    return Error::none();
}

Error ExtensionStore::list_revisions(std::string_view app_id, std::vector<RevisionRecord>& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    out.clear();
    Error error;
    Stmt stmt(db_,
              "SELECT revision_id FROM revisions WHERE app_id=? ORDER BY created_at DESC, revision_id DESC",
              error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    for (;;) {
        const int result = stmt.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW) return step_result(db_, result, "list revisions");
        RevisionRecord revision;
        bool found = false;
        Error load_error = get_revision(column_text(stmt.get(), 0), revision, found);
        if (!load_error.ok()) return load_error;
        if (found) out.push_back(std::move(revision));
    }
    return Error::none();
}

Error ExtensionStore::insert_revision_locked(const RevisionRecord& revision) {
    Error error;
    Stmt stmt(db_,
              "INSERT INTO revisions(revision_id, app_id, version, manifest_json, created_at, file_count, "
              "total_bytes, status, pinned, integrity) VALUES(?,?,?,?,?,?,?,?,?,?)",
              error);
    if (!stmt.ok()) return error;
    stmt.text(1, revision.revision_id);
    stmt.text(2, revision.app_id);
    stmt.text(3, revision.version);
    stmt.text(4, revision.manifest_json);
    stmt.int64(5, revision.created_at > 0 ? revision.created_at : now_unix_seconds());
    stmt.int64(6, static_cast<std::int64_t>(revision.file_count));
    stmt.int64(7, static_cast<std::int64_t>(revision.total_bytes));
    stmt.text(8, revision.status);
    stmt.int64(9, revision.pinned ? 1 : 0);
    stmt.text(10, revision.integrity);
    return step_result(db_, stmt.step(), "insert revision");
}

Error ExtensionStore::insert_revision(const RevisionRecord& revision) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return insert_revision_locked(revision);
}

Error ExtensionStore::set_revision_status(std::string_view revision_id, std::string_view status) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "UPDATE revisions SET status=? WHERE revision_id=?", error);
    if (!stmt.ok()) return error;
    stmt.text(1, status);
    stmt.text(2, revision_id);
    Error step_error = step_result(db_, stmt.step(), "update revision status");
    if (!step_error.ok()) return step_error;
    if (sqlite3_changes(db_) == 0) return Error::make(ErrorCode::not_found, "revision not found");
    return Error::none();
}

Error ExtensionStore::set_revision_pinned(std::string_view revision_id, bool pinned) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "UPDATE revisions SET pinned=? WHERE revision_id=?", error);
    if (!stmt.ok()) return error;
    stmt.int64(1, pinned ? 1 : 0);
    stmt.text(2, revision_id);
    Error step_error = step_result(db_, stmt.step(), "pin revision");
    if (!step_error.ok()) return step_error;
    if (sqlite3_changes(db_) == 0) return Error::make(ErrorCode::not_found, "revision not found");
    return Error::none();
}

Error ExtensionStore::set_revision_integrity(std::string_view revision_id, std::string_view state) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "UPDATE revisions SET integrity=? WHERE revision_id=?", error);
    if (!stmt.ok()) return error;
    stmt.text(1, state);
    stmt.text(2, revision_id);
    return step_result(db_, stmt.step(), "update revision integrity");
}

Error ExtensionStore::delete_revision(std::string_view revision_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "DELETE FROM revisions WHERE revision_id=?", error);
    if (!stmt.ok()) return error;
    stmt.text(1, revision_id);
    return step_result(db_, stmt.step(), "delete revision");
}

Error ExtensionStore::count_revisions(std::string_view app_id, std::uint64_t& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    out = 0;
    Error error;
    Stmt stmt(db_, "SELECT COUNT(*) FROM revisions WHERE app_id=?", error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    const int result = stmt.step();
    if (result != SQLITE_ROW) return step_result(db_, result, "count revisions");
    out = static_cast<std::uint64_t>(column_int64(stmt.get(), 0));
    return Error::none();
}

Error ExtensionStore::replace_permission_requests(std::string_view app_id,
                                                  const std::vector<std::string>& modules) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    {
        Stmt stmt(db_, "DELETE FROM permission_requests WHERE app_id=?", error);
        if (!stmt.ok()) return error;
        stmt.text(1, app_id);
        Error step_error = step_result(db_, stmt.step(), "clear permission requests");
        if (!step_error.ok()) return step_error;
    }
    for (const auto& module : modules) {
        Stmt stmt(db_, "INSERT OR IGNORE INTO permission_requests(app_id, module) VALUES(?,?)", error);
        if (!stmt.ok()) return error;
        stmt.text(1, app_id);
        stmt.text(2, module);
        Error step_error = step_result(db_, stmt.step(), "insert permission request");
        if (!step_error.ok()) return step_error;
    }
    return Error::none();
}

Error ExtensionStore::add_permission_request(std::string_view app_id, std::string_view module) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "INSERT OR IGNORE INTO permission_requests(app_id, module) VALUES(?,?)", error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    stmt.text(2, module);
    return step_result(db_, stmt.step(), "insert permission request");
}

Error ExtensionStore::replace_permission_grants(std::string_view app_id,
                                                const std::vector<std::string>& modules) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    {
        Stmt stmt(db_, "DELETE FROM permission_grants WHERE app_id=?", error);
        if (!stmt.ok()) return error;
        stmt.text(1, app_id);
        Error step_error = step_result(db_, stmt.step(), "clear permission grants");
        if (!step_error.ok()) return step_error;
    }
    const std::int64_t now = now_unix_seconds();
    for (const auto& module : modules) {
        Stmt stmt(db_, "INSERT OR IGNORE INTO permission_grants(app_id, module, granted_at) VALUES(?,?,?)",
                  error);
        if (!stmt.ok()) return error;
        stmt.text(1, app_id);
        stmt.text(2, module);
        stmt.int64(3, now);
        Error step_error = step_result(db_, stmt.step(), "insert permission grant");
        if (!step_error.ok()) return step_error;
    }
    return Error::none();
}

Error ExtensionStore::clear_permission_grants(std::string_view app_id) {
    return replace_permission_grants(app_id, {});
}

Error ExtensionStore::replace_network_origins(std::string_view app_id,
                                              const std::vector<std::string>& origins) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    {
        Stmt stmt(db_, "DELETE FROM network_origins WHERE app_id=?", error);
        if (!stmt.ok()) return error;
        stmt.text(1, app_id);
        Error step_error = step_result(db_, stmt.step(), "clear network origins");
        if (!step_error.ok()) return step_error;
    }
    for (const auto& origin : origins) {
        Stmt stmt(db_, "INSERT OR IGNORE INTO network_origins(app_id, origin) VALUES(?,?)", error);
        if (!stmt.ok()) return error;
        stmt.text(1, app_id);
        stmt.text(2, origin);
        Error step_error = step_result(db_, stmt.step(), "insert network origin");
        if (!step_error.ok()) return step_error;
    }
    return Error::none();
}

Error ExtensionStore::list_network_origins(std::string_view app_id,
                                           std::vector<std::string>& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    out.clear();
    Error error;
    Stmt stmt(db_, "SELECT origin FROM network_origins WHERE app_id=? ORDER BY origin", error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    for (;;) {
        const int result = stmt.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW) return step_result(db_, result, "read network origins");
        out.push_back(column_text(stmt.get(), 0));
    }
    return Error::none();
}

Error ExtensionStore::clear_network_origins(std::string_view app_id) {
    return replace_network_origins(app_id, {});
}

Error ExtensionStore::upsert_backend(const BackendRecord& record) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string args_json = nlohmann::json(record.args).dump();
    Error error;
    Stmt stmt(db_,
              "INSERT INTO app_backends(app_id, service_id, executable_rel, args_json, arch, "
              "run_user, run_group, allow_root, autostart, state, error, created_at, updated_at) "
              "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) "
              "ON CONFLICT(app_id) DO UPDATE SET service_id=excluded.service_id, "
              "executable_rel=excluded.executable_rel, args_json=excluded.args_json, "
              "arch=excluded.arch, run_user=excluded.run_user, run_group=excluded.run_group, "
              "allow_root=excluded.allow_root, autostart=excluded.autostart, "
              "state=excluded.state, error=excluded.error, updated_at=excluded.updated_at",
              error);
    if (!stmt.ok()) return error;
    const std::int64_t now = now_unix_seconds();
    stmt.text(1, record.app_id);
    stmt.text(2, record.service_id);
    stmt.text(3, record.executable_rel);
    stmt.text(4, args_json);
    stmt.text(5, record.arch);
    stmt.text(6, record.run_user);
    stmt.text(7, record.run_group);
    stmt.int64(8, record.allow_root ? 1 : 0);
    stmt.int64(9, record.autostart ? 1 : 0);
    stmt.text(10, record.state);
    stmt.text(11, record.error);
    stmt.int64(12, record.created_at > 0 ? record.created_at : now);
    stmt.int64(13, now);
    return step_result(db_, stmt.step(), "upsert backend");
}

Error ExtensionStore::get_backend(std::string_view app_id, BackendRecord& out, bool& found) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    found = false;
    out = {};
    Error error;
    Stmt stmt(db_,
              "SELECT app_id, service_id, executable_rel, args_json, arch, run_user, run_group, "
              "allow_root, autostart, state, error, created_at, updated_at "
              "FROM app_backends WHERE app_id=?",
              error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    const int result = stmt.step();
    if (result == SQLITE_DONE) return Error::none();
    if (result != SQLITE_ROW) return step_result(db_, result, "read backend");
    out.app_id = column_text(stmt.get(), 0);
    out.service_id = column_text(stmt.get(), 1);
    out.executable_rel = column_text(stmt.get(), 2);
    const auto args = nlohmann::json::parse(column_text(stmt.get(), 3), nullptr, false);
    if (args.is_discarded() || !args.is_array()) {
        return Error::make(ErrorCode::io_error, "stored backend args_json is invalid");
    }
    out.args.clear();
    for (const auto& item : args) {
        if (!item.is_string()) {
            return Error::make(ErrorCode::io_error, "stored backend args_json is invalid");
        }
        out.args.push_back(item.get<std::string>());
    }
    out.arch = column_text(stmt.get(), 4);
    out.run_user = column_text(stmt.get(), 5);
    out.run_group = column_text(stmt.get(), 6);
    out.allow_root = column_int64(stmt.get(), 7) != 0;
    out.autostart = column_int64(stmt.get(), 8) != 0;
    out.state = column_text(stmt.get(), 9);
    out.error = column_text(stmt.get(), 10);
    out.created_at = column_int64(stmt.get(), 11);
    out.updated_at = column_int64(stmt.get(), 12);
    found = true;
    return Error::none();
}

Error ExtensionStore::list_backends(std::vector<BackendRecord>& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    out.clear();
    Error error;
    Stmt stmt(db_, "SELECT app_id FROM app_backends ORDER BY app_id", error);
    if (!stmt.ok()) return error;
    for (;;) {
        const int result = stmt.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW) return step_result(db_, result, "list backends");
        BackendRecord record;
        bool found = false;
        Error read_error = get_backend(column_text(stmt.get(), 0), record, found);
        if (!read_error.ok()) return read_error;
        if (found) out.push_back(std::move(record));
    }
    return Error::none();
}

Error ExtensionStore::set_backend_state(std::string_view app_id, std::string_view state,
                                        const std::string& error_message) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "UPDATE app_backends SET state=?, error=?, updated_at=? WHERE app_id=?", error);
    if (!stmt.ok()) return error;
    stmt.text(1, state);
    stmt.text(2, error_message);
    stmt.int64(3, now_unix_seconds());
    stmt.text(4, app_id);
    Error step_error = step_result(db_, stmt.step(), "update backend state");
    if (!step_error.ok()) return step_error;
    if (sqlite3_changes(db_) == 0) {
        return Error::make(ErrorCode::not_found, "application has no native backend");
    }
    return Error::none();
}

Error ExtensionStore::set_backend_identity(std::string_view app_id, const std::string& run_user,
                                           const std::string& run_group, bool allow_root) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_,
              "UPDATE app_backends SET run_user=?, run_group=?, allow_root=?, updated_at=? "
              "WHERE app_id=?",
              error);
    if (!stmt.ok()) return error;
    stmt.text(1, run_user);
    stmt.text(2, run_group);
    stmt.int64(3, allow_root ? 1 : 0);
    stmt.int64(4, now_unix_seconds());
    stmt.text(5, app_id);
    Error step_error = step_result(db_, stmt.step(), "update backend identity");
    if (!step_error.ok()) return step_error;
    if (sqlite3_changes(db_) == 0) {
        return Error::make(ErrorCode::not_found, "application has no native backend");
    }
    return Error::none();
}

Error ExtensionStore::set_backend_autostart(std::string_view app_id, bool autostart) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "UPDATE app_backends SET autostart=?, updated_at=? WHERE app_id=?", error);
    if (!stmt.ok()) return error;
    stmt.int64(1, autostart ? 1 : 0);
    stmt.int64(2, now_unix_seconds());
    stmt.text(3, app_id);
    Error step_error = step_result(db_, stmt.step(), "update backend autostart");
    if (!step_error.ok()) return step_error;
    if (sqlite3_changes(db_) == 0) {
        return Error::make(ErrorCode::not_found, "application has no native backend");
    }
    return Error::none();
}

Error ExtensionStore::delete_backend(std::string_view app_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "DELETE FROM app_backends WHERE app_id=?", error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    return step_result(db_, stmt.step(), "delete backend");
}

Error ExtensionStore::insert_integrity_locked(const std::vector<IntegrityEntry>& entries,
                                              std::string_view revision_id) {
    Error error;
    for (const auto& entry : entries) {
        Stmt stmt(db_, "INSERT OR REPLACE INTO integrity_entries(revision_id, path, size, sha256) "
                       "VALUES(?,?,?,?)",
                  error);
        if (!stmt.ok()) return error;
        stmt.text(1, revision_id);
        stmt.text(2, entry.path);
        stmt.int64(3, static_cast<std::int64_t>(entry.size));
        stmt.text(4, entry.sha256);
        Error step_error = step_result(db_, stmt.step(), "insert integrity entry");
        if (!step_error.ok()) return step_error;
    }
    return Error::none();
}

Error ExtensionStore::replace_integrity_entries(std::string_view revision_id,
                                                const std::vector<IntegrityEntry>& entries) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    {
        Stmt stmt(db_, "DELETE FROM integrity_entries WHERE revision_id=?", error);
        if (!stmt.ok()) return error;
        stmt.text(1, revision_id);
        Error step_error = step_result(db_, stmt.step(), "clear integrity entries");
        if (!step_error.ok()) return step_error;
    }
    return insert_integrity_locked(entries, revision_id);
}

Error ExtensionStore::list_integrity_entries(std::string_view revision_id,
                                             std::vector<IntegrityEntry>& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    out.clear();
    Error error;
    Stmt stmt(db_, "SELECT path, size, sha256 FROM integrity_entries WHERE revision_id=? ORDER BY path", error);
    if (!stmt.ok()) return error;
    stmt.text(1, revision_id);
    for (;;) {
        const int result = stmt.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW) return step_result(db_, result, "list integrity entries");
        IntegrityEntry entry;
        entry.path = column_text(stmt.get(), 0);
        entry.size = static_cast<std::uint64_t>(column_int64(stmt.get(), 1));
        entry.sha256 = column_text(stmt.get(), 2);
        out.push_back(std::move(entry));
    }
    return Error::none();
}

Error ExtensionStore::create_operation(OperationRecord& operation) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (operation.operation_id.empty()) {
        operation.operation_id = random_hex(16);
    }
    const std::int64_t now = now_unix_seconds();
    operation.created_at = now;
    operation.updated_at = now;
    Error error;
    Stmt stmt(db_,
              "INSERT INTO operations(operation_id, app_id, type, phase, temp_path, revision_id, error, "
              "created_at, updated_at) VALUES(?,?,?,?,?,?,?,?,?)",
              error);
    if (!stmt.ok()) return error;
    stmt.text(1, operation.operation_id);
    stmt.text(2, operation.app_id);
    stmt.text(3, operation.type);
    stmt.text(4, operation.phase);
    if (operation.temp_path.empty()) stmt.null_text(5); else stmt.text(5, operation.temp_path);
    if (operation.revision_id.empty()) stmt.null_text(6); else stmt.text(6, operation.revision_id);
    if (operation.error.empty()) stmt.null_text(7); else stmt.text(7, operation.error);
    stmt.int64(8, now);
    stmt.int64(9, now);
    return step_result(db_, stmt.step(), "create operation");
}

Error ExtensionStore::update_operation(std::string_view operation_id, std::string_view phase,
                                       std::string_view revision_id, std::string_view error_message) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_,
              "UPDATE operations SET phase=?, revision_id=COALESCE(?, revision_id), error=?, updated_at=? "
              "WHERE operation_id=?",
              error);
    if (!stmt.ok()) return error;
    stmt.text(1, phase);
    if (revision_id.empty()) stmt.null_text(2); else stmt.text(2, revision_id);
    if (error_message.empty()) stmt.null_text(3); else stmt.text(3, error_message);
    stmt.int64(4, now_unix_seconds());
    stmt.text(5, operation_id);
    return step_result(db_, stmt.step(), "update operation");
}

Error ExtensionStore::set_operation_app(std::string_view operation_id, std::string_view app_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "UPDATE operations SET app_id=?, updated_at=? WHERE operation_id=?", error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    stmt.int64(2, now_unix_seconds());
    stmt.text(3, operation_id);
    return step_result(db_, stmt.step(), "update operation application");
}

Error ExtensionStore::list_pending_operations(std::vector<OperationRecord>& out) const {    std::lock_guard<std::recursive_mutex> lock(mutex_);
    out.clear();
    Error error;
    Stmt stmt(db_,
              "SELECT operation_id, app_id, type, phase, temp_path, revision_id, error, created_at, "
              "updated_at FROM operations WHERE phase NOT IN ('done','failed') ORDER BY created_at",
              error);
    if (!stmt.ok()) return error;
    for (;;) {
        const int result = stmt.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW) return step_result(db_, result, "list pending operations");
        OperationRecord op;
        op.operation_id = column_text(stmt.get(), 0);
        op.app_id = column_text(stmt.get(), 1);
        op.type = column_text(stmt.get(), 2);
        op.phase = column_text(stmt.get(), 3);
        op.temp_path = column_text(stmt.get(), 4);
        op.revision_id = column_text(stmt.get(), 5);
        op.error = column_text(stmt.get(), 6);
        op.created_at = column_int64(stmt.get(), 7);
        op.updated_at = column_int64(stmt.get(), 8);
        out.push_back(std::move(op));
    }
    return Error::none();
}

Error ExtensionStore::kv_get(std::string_view app_id, std::string_view key, KvEntry& out,
                             bool& found) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    out = {};
    found = false;
    Error error;
    Stmt stmt(db_, "SELECT key, value, size_bytes, updated_at FROM app_kv WHERE app_id=? AND key=?", error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    stmt.text(2, key);
    const int result = stmt.step();
    if (result == SQLITE_DONE) return Error::none();
    if (result != SQLITE_ROW) return step_result(db_, result, "read kv");
    found = true;
    out.key = column_text(stmt.get(), 0);
    out.value = column_text(stmt.get(), 1);
    out.size_bytes = static_cast<std::uint64_t>(column_int64(stmt.get(), 2));
    out.updated_at = column_int64(stmt.get(), 3);
    return Error::none();
}

Error ExtensionStore::kv_usage(std::string_view app_id, std::uint64_t& usage) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    usage = 0;
    Error error;
    Stmt stmt(db_, "SELECT COALESCE(SUM(size_bytes), 0) FROM app_kv WHERE app_id=?", error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    const int result = stmt.step();
    if (result != SQLITE_ROW) return step_result(db_, result, "read kv usage");
    usage = static_cast<std::uint64_t>(column_int64(stmt.get(), 0));
    return Error::none();
}

Error ExtensionStore::kv_set(std::string_view app_id, std::string_view key, std::string_view value,
                             std::uint64_t& usage_after) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string key_error;
    if (!valid_kv_key(key, key_error)) return Error::make(ErrorCode::invalid_argument, key_error);
    if (value.size() > kAppKvKeyLimitBytes) {
        return Error::make(ErrorCode::too_large, "value exceeds the 256 KiB per-key limit");
    }
    return run_transaction([&]() -> Error {
        std::uint64_t usage = 0;
        Error error = kv_usage(app_id, usage);
        if (!error.ok()) return error;
        std::uint64_t previous = 0;
        {
            Stmt stmt(db_, "SELECT size_bytes FROM app_kv WHERE app_id=? AND key=?", error);
            if (!stmt.ok()) return error;
            stmt.text(1, app_id);
            stmt.text(2, key);
            const int result = stmt.step();
            if (result == SQLITE_ROW) previous = static_cast<std::uint64_t>(column_int64(stmt.get(), 0));
            else if (result != SQLITE_DONE) return step_result(db_, result, "read kv size");
        }
        const std::uint64_t next_usage = usage - previous + value.size();
        if (next_usage > kAppKvQuotaBytes) {
            return Error::make(ErrorCode::too_large, "application storage quota of 5 MiB would be exceeded");
        }
        Stmt stmt(db_,
                  "INSERT INTO app_kv(app_id, key, value, size_bytes, updated_at) VALUES(?,?,?,?,?) "
                  "ON CONFLICT(app_id, key) DO UPDATE SET value=excluded.value, "
                  "size_bytes=excluded.size_bytes, updated_at=excluded.updated_at",
                  error);
        if (!stmt.ok()) return error;
        stmt.text(1, app_id);
        stmt.text(2, key);
        stmt.text(3, value);
        stmt.int64(4, static_cast<std::int64_t>(value.size()));
        stmt.int64(5, now_unix_seconds());
        error = step_result(db_, stmt.step(), "write kv");
        usage_after = next_usage;
        return error;
    });
}

Error ExtensionStore::kv_delete(std::string_view app_id, std::string_view key, bool& deleted,
                                std::uint64_t& usage_after) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string key_error;
    if (!valid_kv_key(key, key_error)) return Error::make(ErrorCode::invalid_argument, key_error);
    return run_transaction([&]() -> Error {
        Error error;
        Stmt stmt(db_, "DELETE FROM app_kv WHERE app_id=? AND key=?", error);
        if (!stmt.ok()) return error;
        stmt.text(1, app_id);
        stmt.text(2, key);
        error = step_result(db_, stmt.step(), "delete kv");
        if (!error.ok()) return error;
        deleted = sqlite3_changes(db_) > 0;
        return kv_usage(app_id, usage_after);
    });
}

Error ExtensionStore::kv_list(std::string_view app_id, std::string_view after_key, int limit,
                              std::vector<KvEntry>& entries, std::string& next_cursor,
                              std::uint64_t& usage) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string after(after_key);
    entries.clear();
    next_cursor.clear();
    const int clamped = limit < 1 ? 100 : (limit > 500 ? 500 : limit);
    Error error = kv_usage(app_id, usage);
    if (!error.ok()) return error;
    Stmt stmt(db_, "SELECT key, value, size_bytes, updated_at FROM app_kv WHERE app_id=? AND key>? "
                   "ORDER BY key LIMIT ?",
              error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    stmt.text(2, after);
    stmt.int64(3, clamped + 1);
    for (;;) {
        const int result = stmt.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW) return step_result(db_, result, "list kv");
        KvEntry entry;
        entry.key = column_text(stmt.get(), 0);
        entry.value = column_text(stmt.get(), 1);
        entry.size_bytes = static_cast<std::uint64_t>(column_int64(stmt.get(), 2));
        entry.updated_at = column_int64(stmt.get(), 3);
        entries.push_back(std::move(entry));
    }
    if (static_cast<int>(entries.size()) > clamped) {
        entries.resize(static_cast<std::size_t>(clamped));
        next_cursor = entries.back().key;
    }
    return Error::none();
}

Error ExtensionStore::kv_transaction(std::string_view app_id,
                                     const std::vector<KvOperation>& operations,
                                     std::uint64_t& usage_after) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (operations.empty()) {
        return kv_usage(app_id, usage_after);
    }
    return run_transaction([&]() -> Error {
        Error error;
        std::uint64_t usage = 0;
        error = kv_usage(app_id, usage);
        if (!error.ok()) return error;
        // Validate every staged key/value before mutating anything. The map
        // tracks staged sizes so repeated operations on the same key are
        // accounted for exactly once.
        std::map<std::string, std::uint64_t> staged;
        std::uint64_t next_usage = usage;
        for (const auto& op : operations) {
            std::string key_error;
            if (!valid_kv_key(op.key, key_error)) {
                return Error::make(ErrorCode::invalid_argument, key_error);
            }
            if (op.kind == KvOperationKind::set && op.value.size() > kAppKvKeyLimitBytes) {
                return Error::make(ErrorCode::too_large,
                                   "value for '" + op.key + "' exceeds the 256 KiB per-key limit");
            }
            std::uint64_t previous = 0;
            const auto staged_it = staged.find(op.key);
            if (staged_it != staged.end()) {
                previous = staged_it->second;
            } else {
                Stmt lookup(db_, "SELECT size_bytes FROM app_kv WHERE app_id=? AND key=?", error);
                if (!lookup.ok()) return error;
                lookup.text(1, app_id);
                lookup.text(2, op.key);
                const int result = lookup.step();
                if (result == SQLITE_ROW) previous = static_cast<std::uint64_t>(column_int64(lookup.get(), 0));
                else if (result != SQLITE_DONE) return step_result(db_, result, "read kv for transaction");
            }
            next_usage -= previous;
            const std::uint64_t next_size = op.kind == KvOperationKind::set ? op.value.size() : 0;
            next_usage += next_size;
            staged[op.key] = next_size;
        }
        if (next_usage > kAppKvQuotaBytes) {
            return Error::make(ErrorCode::too_large, "application storage quota of 5 MiB would be exceeded");
        }
        for (const auto& op : operations) {
            if (op.kind == KvOperationKind::set) {
                Stmt stmt(db_,
                          "INSERT INTO app_kv(app_id, key, value, size_bytes, updated_at) VALUES(?,?,?,?,?) "
                          "ON CONFLICT(app_id, key) DO UPDATE SET value=excluded.value, "
                          "size_bytes=excluded.size_bytes, updated_at=excluded.updated_at",
                          error);
                if (!stmt.ok()) return error;
                stmt.text(1, app_id);
                stmt.text(2, op.key);
                stmt.text(3, op.value);
                stmt.int64(4, static_cast<std::int64_t>(op.value.size()));
                stmt.int64(5, now_unix_seconds());
                error = step_result(db_, stmt.step(), "apply kv transaction");
            } else {
                Stmt stmt(db_, "DELETE FROM app_kv WHERE app_id=? AND key=?", error);
                if (!stmt.ok()) return error;
                stmt.text(1, app_id);
                stmt.text(2, op.key);
                error = step_result(db_, stmt.step(), "apply kv transaction delete");
            }
            if (!error.ok()) return error;
        }
        usage_after = next_usage;
        return Error::none();
    });
}

Error ExtensionStore::kv_delete_all(std::string_view app_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error;
    Stmt stmt(db_, "DELETE FROM app_kv WHERE app_id=?", error);
    if (!stmt.ok()) return error;
    stmt.text(1, app_id);
    return step_result(db_, stmt.step(), "delete all application kv");
}

Error ExtensionStore::run_transaction(const std::function<Error()>& body) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Error error = exec_locked("BEGIN IMMEDIATE");
    if (!error.ok()) return error;
    Error body_error;
    try {
        body_error = body();
    } catch (...) {
        exec_locked("ROLLBACK");
        throw;
    }
    if (!body_error.ok()) {
        exec_locked("ROLLBACK");
        return body_error;
    }
    error = exec_locked("COMMIT");
    if (!error.ok()) {
        exec_locked("ROLLBACK");
        return error;
    }
    return Error::none();
}

} // namespace edgedesk::extensions
