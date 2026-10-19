#include "supervisor/supervisor_store.hpp"

#include "core/util.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

namespace edgedesk::supervisor {
namespace {

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql, Error& error) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            error = sqlite_error_to_error(sqlite3_errcode(db),
                                          std::string("prepare failed: ") + sqlite3_errmsg(db));
            stmt_ = nullptr;
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
    void i64(int index, std::int64_t value) { sqlite3_bind_int64(stmt_, index, value); }
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

std::int64_t column_i64(sqlite3_stmt* stmt, int index) {
    return static_cast<std::int64_t>(sqlite3_column_int64(stmt, index));
}

Error exec_sql(sqlite3* db, const std::string& sql, const std::string& context) {
    char* message = nullptr;
    const int result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        std::string text = message != nullptr ? message : sqlite3_errmsg(db);
        if (message != nullptr) sqlite3_free(message);
        return sqlite_error_to_error(result, context + ": " + text);
    }
    return Error::none();
}

Error args_from_json(const std::string& text, std::vector<std::string>& out) {
    try {
        const auto parsed = nlohmann::json::parse(text);
        if (!parsed.is_array()) {
            return Error::make(ErrorCode::io_error, "stored args_json is not an array");
        }
        out.clear();
        for (const auto& item : parsed) {
            if (!item.is_string()) {
                return Error::make(ErrorCode::io_error, "stored args_json contains a non-string");
            }
            out.push_back(item.get<std::string>());
        }
        return Error::none();
    } catch (const std::exception& error) {
        return Error::make(ErrorCode::io_error,
                           std::string("stored args_json is invalid: ") + error.what());
    }
}

Error env_from_json(const std::string& text, std::map<std::string, std::string>& out) {
    try {
        const auto parsed = nlohmann::json::parse(text);
        if (!parsed.is_object()) {
            return Error::make(ErrorCode::io_error, "stored env_json is not an object");
        }
        out.clear();
        for (const auto& item : parsed.items()) {
            if (!item.value().is_string()) {
                return Error::make(ErrorCode::io_error, "stored env_json contains a non-string");
            }
            out[item.key()] = item.value().get<std::string>();
        }
        return Error::none();
    } catch (const std::exception& error) {
        return Error::make(ErrorCode::io_error,
                           std::string("stored env_json is invalid: ") + error.what());
    }
}

ServiceRecord row_to_record(sqlite3_stmt* stmt, Error& error) {
    ServiceRecord record;
    record.config.id = column_text(stmt, 0);
    record.config.name = column_text(stmt, 1);
    record.config.executable = column_text(stmt, 2);
    error = args_from_json(column_text(stmt, 3), record.config.args);
    if (!error.ok()) return record;
    record.config.cwd = column_text(stmt, 4);
    error = env_from_json(column_text(stmt, 5), record.config.env);
    if (!error.ok()) return record;
    record.config.run_user = column_text(stmt, 6);
    record.config.run_group = column_text(stmt, 7);
    record.config.allow_root = column_i64(stmt, 8) != 0;
    record.config.autostart = column_i64(stmt, 9) != 0;
    RestartPolicy policy = RestartPolicy::on_failure;
    if (!parse_restart_policy(column_text(stmt, 10), policy)) {
        error = Error::make(ErrorCode::io_error, "stored restart_policy is invalid");
        return record;
    }
    record.config.restart_policy = policy;
    record.config.max_restarts = static_cast<int>(column_i64(stmt, 11));
    record.config.restart_window_sec = static_cast<int>(column_i64(stmt, 12));
    record.config.restart_delay_ms = static_cast<int>(column_i64(stmt, 13));
    record.config.max_restart_delay_ms = static_cast<int>(column_i64(stmt, 14));
    record.config.stable_after_sec = static_cast<int>(column_i64(stmt, 15));
    record.config.stop_timeout_ms = static_cast<int>(column_i64(stmt, 16));
    record.config.log_max_bytes = static_cast<std::uint64_t>(column_i64(stmt, 17));
    record.config.created_at = column_i64(stmt, 18);
    record.config.updated_at = column_i64(stmt, 19);

    ServiceState state = ServiceState::stopped;
    if (!parse_service_state(column_text(stmt, 20), state)) {
        error = Error::make(ErrorCode::io_error, "stored service state is invalid");
        return record;
    }
    record.runtime.state = state;
    record.runtime.desired_state = column_text(stmt, 21);
    record.runtime.boot_id = column_text(stmt, 22);
    record.runtime.pid = static_cast<int>(column_i64(stmt, 23));
    record.runtime.pgid = static_cast<int>(column_i64(stmt, 24));
    record.runtime.start_ticks = static_cast<std::uint64_t>(column_i64(stmt, 25));
    record.runtime.started_at_ms = column_i64(stmt, 26);
    record.runtime.last_exit_code = static_cast<int>(column_i64(stmt, 27));
    record.runtime.last_signal = static_cast<int>(column_i64(stmt, 28));
    record.runtime.last_exit_at = column_i64(stmt, 29);
    record.runtime.last_error = column_text(stmt, 30);
    record.runtime.restart_count = static_cast<std::uint64_t>(column_i64(stmt, 31));
    record.runtime.window_restarts = static_cast<std::uint64_t>(column_i64(stmt, 32));
    record.runtime.window_started_at = column_i64(stmt, 33);
    record.runtime.failed_locked = column_i64(stmt, 34) != 0;
    error = Error::none();
    return record;
}

constexpr const char* kServiceColumns =
    "id,name,executable,args_json,cwd,env_json,run_user,run_group,allow_root,autostart,"
    "restart_policy,max_restarts,restart_window_sec,restart_delay_ms,max_restart_delay_ms,"
    "stable_after_sec,stop_timeout_ms,log_max_bytes,created_at,updated_at,"
    "state,desired_state,boot_id,pid,pgid,start_ticks,started_at_ms,last_exit_code,"
    "last_signal,last_exit_at,last_error,restart_count,window_restarts,window_started_at,"
    "failed_locked";

constexpr const char* kInsertService =
    "INSERT INTO services("
    "id,name,executable,args_json,cwd,env_json,run_user,run_group,allow_root,autostart,"
    "restart_policy,max_restarts,restart_window_sec,restart_delay_ms,max_restart_delay_ms,"
    "stable_after_sec,stop_timeout_ms,log_max_bytes,created_at,updated_at,"
    "state,desired_state,boot_id,pid,pgid,start_ticks,started_at_ms,last_exit_code,"
    "last_signal,last_exit_at,last_error,restart_count,window_restarts,window_started_at,"
    "failed_locked) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,"
    "?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,?33,?34,?35)";

constexpr const char* kUpdateService =
    "UPDATE services SET name=?1,executable=?2,args_json=?3,cwd=?4,env_json=?5,run_user=?6,"
    "run_group=?7,allow_root=?8,autostart=?9,restart_policy=?10,max_restarts=?11,"
    "restart_window_sec=?12,restart_delay_ms=?13,max_restart_delay_ms=?14,stable_after_sec=?15,"
    "stop_timeout_ms=?16,log_max_bytes=?17,updated_at=?18 WHERE id=?19";

constexpr const char* kSaveRuntime =
    "UPDATE services SET state=?1,desired_state=?2,boot_id=?3,pid=?4,pgid=?5,start_ticks=?6,"
    "started_at_ms=?7,last_exit_code=?8,last_signal=?9,last_exit_at=?10,last_error=?11,"
    "restart_count=?12,window_restarts=?13,window_started_at=?14,failed_locked=?15 WHERE id=?16";

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
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            return Error::make(ErrorCode::io_error, "supervisor database is corrupt: " + context, code);
        case SQLITE_CANTOPEN:
        case SQLITE_IOERR:
        default:
            return Error::make(ErrorCode::io_error, context, code);
    }
}

bool ensure_private_directory(const std::string& path, std::string& error) {
    if (path.empty() || path.front() != '/') {
        error = "supervisor directory must be an absolute path";
        return false;
    }
    std::string current = "/";
    std::size_t start = 1;
    for (;;) {
        const auto slash = path.find('/', start);
        const std::string part =
            path.substr(start, slash == std::string::npos ? path.size() - start : slash - start);
        if (!part.empty()) {
            if (current.size() > 1) current.push_back('/');
            current += part;
            struct stat info {};
            if (::lstat(current.c_str(), &info) != 0) {
                if (errno != ENOENT) {
                    error = "cannot inspect " + current + ": " + std::strerror(errno);
                    return false;
                }
                if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
                    error = "cannot create " + current + ": " + std::strerror(errno);
                    return false;
                }
                if (::lstat(current.c_str(), &info) != 0) {
                    error = "cannot inspect created " + current + ": " + std::strerror(errno);
                    return false;
                }
            }
            if (S_ISLNK(info.st_mode)) {
                error = "refusing symlink in supervisor data path: " + current;
                return false;
            }
            if (!S_ISDIR(info.st_mode)) {
                error = "path component is not a directory: " + current;
                return false;
            }
            if (current.size() == path.size()) {
                if (info.st_uid != ::geteuid()) {
                    error = "supervisor directory is not owned by the service user: " + current;
                    return false;
                }
                if (::chmod(current.c_str(), 0700) != 0) {
                    error = "cannot tighten permissions on " + current + ": " + std::strerror(errno);
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

std::unique_ptr<SupervisorStore> SupervisorStore::open(const std::string& database_path,
                                                       Error& error) {
    if (database_path.empty() || database_path.front() != '/') {
        error = Error::make(ErrorCode::invalid_argument, "supervisor database path must be absolute");
        return nullptr;
    }
    const auto slash = database_path.rfind('/');
    const std::string directory = slash == 0 ? "/" : database_path.substr(0, slash);
    std::string directory_error;
    if (!ensure_private_directory(directory, directory_error)) {
        error = Error::make(ErrorCode::permission_denied, directory_error);
        return nullptr;
    }

    struct stat info {};
    const bool existed = ::lstat(database_path.c_str(), &info) == 0;
    if (existed) {
        if (S_ISLNK(info.st_mode)) {
            error = Error::make(ErrorCode::permission_denied,
                                "refusing symlinked supervisor database: " + database_path);
            return nullptr;
        }
        if (!S_ISREG(info.st_mode)) {
            error = Error::make(ErrorCode::permission_denied,
                                "supervisor database is not a regular file: " + database_path);
            return nullptr;
        }
        if (info.st_uid != ::geteuid()) {
            error = Error::make(ErrorCode::permission_denied,
                                "supervisor database is not owned by the service user: " + database_path);
            return nullptr;
        }
        if (::chmod(database_path.c_str(), 0600) != 0) {
            error = Error::make(ErrorCode::permission_denied,
                                "cannot tighten supervisor database permissions: " +
                                    std::string(std::strerror(errno)));
            return nullptr;
        }
    }

    sqlite3* db = nullptr;
    const mode_t previous_umask = ::umask(0077);
    const int result = sqlite3_open_v2(
        database_path.c_str(), &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (result != SQLITE_OK) {
        ::umask(previous_umask);
        error = sqlite_error_to_error(result, "cannot open supervisor database: " +
                                                  std::string(db != nullptr ? sqlite3_errmsg(db)
                                                                            : std::strerror(errno)));
        if (db != nullptr) sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 5000);
    std::unique_ptr<SupervisorStore> store(new SupervisorStore(database_path, db));
    error = store->initialize_schema();
    ::umask(previous_umask);
    if (!error.ok()) return nullptr;
    (void)::chmod(database_path.c_str(), 0600);
    return store;
}

SupervisorStore::~SupervisorStore() {
    if (db_ != nullptr) sqlite3_close(db_);
}

Error SupervisorStore::initialize_schema() {
    Error error;
    Stmt check(db_, "PRAGMA quick_check", error);
    if (!check.ok()) return error;
    for (;;) {
        const int step = check.step();
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return sqlite_error_to_error(step, std::string("quick_check: ") + sqlite3_errmsg(db_));
        }
        if (column_text(check.get(), 0) != "ok") {
            return Error::make(ErrorCode::io_error,
                               "supervisor database failed integrity check: " +
                                   column_text(check.get(), 0));
        }
    }

    Error pragma_error = exec_sql(db_, "PRAGMA synchronous=FULL", "set synchronous mode");
    if (!pragma_error.ok()) return pragma_error;

    Stmt version_stmt(db_, "PRAGMA user_version", error);
    if (!version_stmt.ok() || version_stmt.step() != SQLITE_ROW) {
        return error.ok() ? Error::make(ErrorCode::io_error, "cannot read supervisor schema version")
                          : error;
    }
    const int version = static_cast<int>(column_i64(version_stmt.get(), 0));
    if (version > schema_version()) {        return Error::make(ErrorCode::io_error,
                           "supervisor database schema version " + std::to_string(version) +
                               " is newer than supported version " +
                               std::to_string(schema_version()));
    }
    if (version == 0) {
        error = exec_sql(db_, "BEGIN IMMEDIATE", "begin schema transaction");
        if (!error.ok()) return error;
        error = apply_schema_v1();
        if (!error.ok()) {
            exec_sql(db_, "ROLLBACK", "rollback schema transaction");
            return error;
        }
        error = set_user_version(1);
        if (!error.ok()) {
            exec_sql(db_, "ROLLBACK", "rollback schema transaction");
            return error;
        }
        return exec_sql(db_, "COMMIT", "commit schema transaction");
    }
    // Version 1 is the initial schema. Future versions append forward-only
    // migrations here; older versions are never recreated from scratch.
    return Error::none();
}

Error SupervisorStore::apply_schema_v1() {
    const char* schema =
        "CREATE TABLE IF NOT EXISTS schema_migrations("
        "version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS services("
        "id TEXT PRIMARY KEY,"
        "name TEXT NOT NULL UNIQUE,"
        "executable TEXT NOT NULL,"
        "args_json TEXT NOT NULL,"
        "cwd TEXT NOT NULL,"
        "env_json TEXT NOT NULL,"
        "run_user TEXT NOT NULL,"
        "run_group TEXT NOT NULL,"
        "allow_root INTEGER NOT NULL DEFAULT 0,"
        "autostart INTEGER NOT NULL DEFAULT 0,"
        "restart_policy TEXT NOT NULL,"
        "max_restarts INTEGER NOT NULL,"
        "restart_window_sec INTEGER NOT NULL,"
        "restart_delay_ms INTEGER NOT NULL,"
        "max_restart_delay_ms INTEGER NOT NULL,"
        "stable_after_sec INTEGER NOT NULL,"
        "stop_timeout_ms INTEGER NOT NULL,"
        "log_max_bytes INTEGER NOT NULL,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL,"
        "state TEXT NOT NULL DEFAULT 'STOPPED',"
        "desired_state TEXT NOT NULL DEFAULT 'stopped',"
        "boot_id TEXT NOT NULL DEFAULT '',"
        "pid INTEGER NOT NULL DEFAULT 0,"
        "pgid INTEGER NOT NULL DEFAULT 0,"
        "start_ticks INTEGER NOT NULL DEFAULT 0,"
        "started_at_ms INTEGER NOT NULL DEFAULT 0,"
        "last_exit_code INTEGER NOT NULL DEFAULT -1,"
        "last_signal INTEGER NOT NULL DEFAULT 0,"
        "last_exit_at INTEGER NOT NULL DEFAULT 0,"
        "last_error TEXT NOT NULL DEFAULT '',"
        "restart_count INTEGER NOT NULL DEFAULT 0,"
        "window_restarts INTEGER NOT NULL DEFAULT 0,"
        "window_started_at INTEGER NOT NULL DEFAULT 0,"
        "failed_locked INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS services_name_idx ON services(name);";
    Error error = exec_sql(db_, schema, "create supervisor schema");
    if (!error.ok()) return error;
    return exec_sql(db_, "INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(1, " +
                             std::to_string(now_unix_seconds()) + ")",
                    "record schema migration");
}

Error SupervisorStore::set_user_version(int version) {
    return exec_sql(db_, "PRAGMA user_version=" + std::to_string(version), "set schema version");
}

Error SupervisorStore::load_all(std::vector<ServiceRecord>& out) const {
    out.clear();
    Error error;
    Stmt stmt(db_, (std::string("SELECT ") + kServiceColumns + " FROM services ORDER BY created_at,id").c_str(),
              error);
    if (!stmt.ok()) return error;
    for (;;) {
        const int step = stmt.step();
        if (step == SQLITE_DONE) return Error::none();
        if (step != SQLITE_ROW) {
            return sqlite_error_to_error(step, std::string("load services: ") + sqlite3_errmsg(db_));
        }
        ServiceRecord record = row_to_record(stmt.get(), error);
        if (!error.ok()) return error;
        out.push_back(std::move(record));
    }
}

Error SupervisorStore::get(std::string_view id, ServiceRecord& out, bool& found) const {
    found = false;
    Error error;
    Stmt stmt(db_, (std::string("SELECT ") + kServiceColumns + " FROM services WHERE id=?1").c_str(),
              error);
    if (!stmt.ok()) return error;
    stmt.text(1, id);
    const int step = stmt.step();
    if (step == SQLITE_DONE) return Error::none();
    if (step != SQLITE_ROW) {
        return sqlite_error_to_error(step, std::string("get service: ") + sqlite3_errmsg(db_));
    }
    out = row_to_record(stmt.get(), error);
    if (!error.ok()) return error;
    found = true;
    return Error::none();
}

Error SupervisorStore::insert(ServiceRecord& record) {
    if (record.config.id.empty()) record.config.id = random_hex(16);
    const std::int64_t now = now_unix_seconds();
    if (record.config.created_at == 0) record.config.created_at = now;
    record.config.updated_at = now;
    Error error;
    Stmt stmt(db_, kInsertService, error);
    if (!stmt.ok()) return error;
    const auto& c = record.config;
    const auto& r = record.runtime;
    stmt.text(1, c.id);
    stmt.text(2, c.name);
    stmt.text(3, c.executable);
    stmt.text(4, nlohmann::json(c.args).dump());
    stmt.text(5, c.cwd);
    stmt.text(6, nlohmann::json(c.env).dump());
    stmt.text(7, c.run_user);
    stmt.text(8, c.run_group);
    stmt.i64(9, c.allow_root ? 1 : 0);
    stmt.i64(10, c.autostart ? 1 : 0);
    stmt.text(11, restart_policy_name(c.restart_policy));
    stmt.i64(12, c.max_restarts);
    stmt.i64(13, c.restart_window_sec);
    stmt.i64(14, c.restart_delay_ms);
    stmt.i64(15, c.max_restart_delay_ms);
    stmt.i64(16, c.stable_after_sec);
    stmt.i64(17, c.stop_timeout_ms);
    stmt.i64(18, static_cast<std::int64_t>(c.log_max_bytes));
    stmt.i64(19, c.created_at);
    stmt.i64(20, c.updated_at);
    stmt.text(21, service_state_name(r.state));
    stmt.text(22, r.desired_state);
    stmt.text(23, r.boot_id);
    stmt.i64(24, r.pid);
    stmt.i64(25, r.pgid);
    stmt.i64(26, static_cast<std::int64_t>(r.start_ticks));
    stmt.i64(27, r.started_at_ms);
    stmt.i64(28, r.last_exit_code);
    stmt.i64(29, r.last_signal);
    stmt.i64(30, r.last_exit_at);
    stmt.text(31, r.last_error);
    stmt.i64(32, static_cast<std::int64_t>(r.restart_count));
    stmt.i64(33, static_cast<std::int64_t>(r.window_restarts));
    stmt.i64(34, r.window_started_at);
    stmt.i64(35, r.failed_locked ? 1 : 0);
    const int step = stmt.step();
    if (step != SQLITE_DONE) {
        return sqlite_error_to_error(step, std::string("insert service: ") + sqlite3_errmsg(db_));
    }
    return Error::none();
}

Error SupervisorStore::update(const ServiceConfig& config) {
    Error error;
    Stmt stmt(db_, kUpdateService, error);
    if (!stmt.ok()) return error;
    const std::int64_t now = now_unix_seconds();
    stmt.text(1, config.name);
    stmt.text(2, config.executable);
    stmt.text(3, nlohmann::json(config.args).dump());
    stmt.text(4, config.cwd);
    stmt.text(5, nlohmann::json(config.env).dump());
    stmt.text(6, config.run_user);
    stmt.text(7, config.run_group);
    stmt.i64(8, config.allow_root ? 1 : 0);
    stmt.i64(9, config.autostart ? 1 : 0);
    stmt.text(10, restart_policy_name(config.restart_policy));
    stmt.i64(11, config.max_restarts);
    stmt.i64(12, config.restart_window_sec);
    stmt.i64(13, config.restart_delay_ms);
    stmt.i64(14, config.max_restart_delay_ms);
    stmt.i64(15, config.stable_after_sec);
    stmt.i64(16, config.stop_timeout_ms);
    stmt.i64(17, static_cast<std::int64_t>(config.log_max_bytes));
    stmt.i64(18, now);
    stmt.text(19, config.id);
    const int step = stmt.step();
    if (step != SQLITE_DONE) {
        return sqlite_error_to_error(step, std::string("update service: ") + sqlite3_errmsg(db_));
    }
    if (sqlite3_changes(db_) == 0) {
        return Error::make(ErrorCode::not_found, "service not found: " + config.id);
    }
    return Error::none();
}

Error SupervisorStore::remove(std::string_view id) {
    Error error;
    Stmt stmt(db_, "DELETE FROM services WHERE id=?1", error);
    if (!stmt.ok()) return error;
    stmt.text(1, id);
    const int step = stmt.step();
    if (step != SQLITE_DONE) {
        return sqlite_error_to_error(step, std::string("delete service: ") + sqlite3_errmsg(db_));
    }
    if (sqlite3_changes(db_) == 0) {
        return Error::make(ErrorCode::not_found, "service not found");
    }
    return Error::none();
}

Error SupervisorStore::save_runtime(std::string_view id, const ServiceRuntime& runtime) {
    Error error;
    Stmt stmt(db_, kSaveRuntime, error);
    if (!stmt.ok()) return error;
    stmt.text(1, service_state_name(runtime.state));
    stmt.text(2, runtime.desired_state);
    stmt.text(3, runtime.boot_id);
    stmt.i64(4, runtime.pid);
    stmt.i64(5, runtime.pgid);
    stmt.i64(6, static_cast<std::int64_t>(runtime.start_ticks));
    stmt.i64(7, runtime.started_at_ms);
    stmt.i64(8, runtime.last_exit_code);
    stmt.i64(9, runtime.last_signal);
    stmt.i64(10, runtime.last_exit_at);
    stmt.text(11, runtime.last_error);
    stmt.i64(12, static_cast<std::int64_t>(runtime.restart_count));
    stmt.i64(13, static_cast<std::int64_t>(runtime.window_restarts));
    stmt.i64(14, runtime.window_started_at);
    stmt.i64(15, runtime.failed_locked ? 1 : 0);
    stmt.text(16, id);
    const int step = stmt.step();
    if (step != SQLITE_DONE) {
        return sqlite_error_to_error(step, std::string("save runtime: ") + sqlite3_errmsg(db_));
    }
    if (sqlite3_changes(db_) == 0) {
        return Error::make(ErrorCode::not_found, "service not found");
    }
    return Error::none();
}

Error SupervisorStore::count(std::uint64_t& out) const {
    out = 0;
    Error error;
    Stmt stmt(db_, "SELECT COUNT(*) FROM services", error);
    if (!stmt.ok()) return error;
    if (stmt.step() != SQLITE_ROW) {
        return Error::make(ErrorCode::io_error, "cannot count supervisor services");
    }
    out = static_cast<std::uint64_t>(column_i64(stmt.get(), 0));
    return Error::none();
}

} // namespace edgedesk::supervisor
