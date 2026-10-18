#pragma once

#include "core/error.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

// SQLite-backed extension metadata store. This is the only database in
// EdgeDesk and is deliberately scoped to the extension platform: auth,
// settings, terminals and metrics remain file/in-memory based (ADR-0011).
//
// All public methods serialize on one mutex. Transactions are explicit and
// roll back on any failure.

namespace edgedesk::extensions {

inline constexpr std::uint64_t kAppKvQuotaBytes = 5ull * 1024 * 1024;
inline constexpr std::uint64_t kAppKvKeyLimitBytes = 256ull * 1024;
inline constexpr std::uint64_t kAppKvMaxKeyBytes = 1024;
inline constexpr int kDefaultHistoryRevisions = 2;

namespace app_status {
inline constexpr const char* pending_review = "pending_review";
inline constexpr const char* enabled = "enabled";
inline constexpr const char* disabled = "disabled";
inline constexpr const char* isolated = "isolated";
inline constexpr const char* uninstalled = "uninstalled";
} // namespace app_status

namespace revision_status {
inline constexpr const char* pending = "pending";
inline constexpr const char* active = "active";
inline constexpr const char* historical = "historical";
inline constexpr const char* isolated = "isolated";
} // namespace revision_status

namespace integrity_state {
inline constexpr const char* ok = "ok";
inline constexpr const char* mismatch = "mismatch";
} // namespace integrity_state

struct AppRecord {
    std::string app_id;
    std::string display_name;
    std::string status = app_status::pending_review;
    std::string active_revision_id;
    std::string active_version;
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
    bool data_retained = false;
    std::vector<std::string> requested_permissions;
    std::vector<std::string> granted_permissions;
    nlohmann::json manifest;
    std::string icon;
};

struct RevisionRecord {
    std::string revision_id;
    std::string app_id;
    std::string version;
    std::string manifest_json;
    std::int64_t created_at = 0;
    std::uint64_t file_count = 0;
    std::uint64_t total_bytes = 0;
    std::string status = revision_status::pending;
    bool pinned = false;
    std::string integrity = integrity_state::ok;
    bool active = false;
};

struct IntegrityEntry {
    std::string path;
    std::uint64_t size = 0;
    std::string sha256;
};

struct OperationRecord {
    std::string operation_id;
    std::string app_id;
    std::string type;  // install | upgrade | downgrade | uninstall | integrity
    std::string phase; // staging | extracted | committed | done | failed
    std::string temp_path;
    std::string revision_id;
    std::string error;
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
};

struct KvEntry {
    std::string key;
    std::string value;
    std::uint64_t size_bytes = 0;
    std::int64_t updated_at = 0;
};

enum class KvOperationKind { set, erase };
struct KvOperation {
    KvOperationKind kind = KvOperationKind::set;
    std::string key;
    std::string value;
};

class ExtensionStore {
public:
    static std::unique_ptr<ExtensionStore> open(const std::string& root, Error& error);

    ExtensionStore(const ExtensionStore&) = delete;
    ExtensionStore& operator=(const ExtensionStore&) = delete;
    ~ExtensionStore();

    const std::string& root() const { return root_; }
    const std::string& database_path() const { return database_path_; }

    // app/revision metadata
    Error get_app(std::string_view app_id, AppRecord& out, bool& found) const;
    Error list_apps(std::vector<AppRecord>& out, bool include_uninstalled = true) const;
    Error upsert_app(const AppRecord& app);
    Error set_app_status(std::string_view app_id, std::string_view status);
    Error set_app_active_revision(std::string_view app_id, std::string_view revision_id,
                                  std::string_view version);
    Error set_data_retained(std::string_view app_id, bool retained);
    Error clear_app_data(std::string_view app_id);
    Error delete_app_record(std::string_view app_id);

    Error get_revision(std::string_view revision_id, RevisionRecord& out, bool& found) const;
    Error list_revisions(std::string_view app_id, std::vector<RevisionRecord>& out) const;
    Error insert_revision(const RevisionRecord& revision);
    Error set_revision_status(std::string_view revision_id, std::string_view status);
    Error set_revision_pinned(std::string_view revision_id, bool pinned);
    Error set_revision_integrity(std::string_view revision_id, std::string_view state);
    Error delete_revision(std::string_view revision_id);
    Error count_revisions(std::string_view app_id, std::uint64_t& out) const;

    // permission requests and grants
    Error replace_permission_requests(std::string_view app_id,
                                      const std::vector<std::string>& modules);
    Error add_permission_request(std::string_view app_id, std::string_view module);
    Error replace_permission_grants(std::string_view app_id,
                                    const std::vector<std::string>& modules);
    Error clear_permission_grants(std::string_view app_id);

    // integrity manifests
    Error replace_integrity_entries(std::string_view revision_id,
                                    const std::vector<IntegrityEntry>& entries);
    Error list_integrity_entries(std::string_view revision_id,
                                 std::vector<IntegrityEntry>& out) const;

    // operations and crash recovery
    Error create_operation(OperationRecord& operation);
    Error update_operation(std::string_view operation_id, std::string_view phase,
                           std::string_view revision_id, std::string_view error_message);
    Error set_operation_app(std::string_view operation_id, std::string_view app_id);
    Error list_pending_operations(std::vector<OperationRecord>& out) const;

    // application KV
    Error kv_get(std::string_view app_id, std::string_view key, KvEntry& out, bool& found) const;
    Error kv_set(std::string_view app_id, std::string_view key, std::string_view value,
                 std::uint64_t& usage_after);
    Error kv_delete(std::string_view app_id, std::string_view key, bool& deleted,
                    std::uint64_t& usage_after);
    Error kv_list(std::string_view app_id, std::string_view after_key, int limit,
                  std::vector<KvEntry>& entries, std::string& next_cursor,
                  std::uint64_t& usage) const;
    Error kv_usage(std::string_view app_id, std::uint64_t& usage) const;
    Error kv_transaction(std::string_view app_id, const std::vector<KvOperation>& operations,
                         std::uint64_t& usage_after);
    Error kv_delete_all(std::string_view app_id);

    // Transaction callback: return an error to roll back.
    Error run_transaction(const std::function<Error()>& body);

private:
    ExtensionStore(std::string root, std::string database_path, sqlite3* db);
    Error initialize_schema();
    Error exec_locked(std::string_view sql);
    Error load_app_locked(std::string_view app_id, AppRecord& out, bool& found) const;
    Error load_permissions_locked(std::string_view app_id, std::vector<std::string>& requested,
                                  std::vector<std::string>& granted) const;
    Error insert_revision_locked(const RevisionRecord& revision);
    Error insert_integrity_locked(const std::vector<IntegrityEntry>& entries,
                                  std::string_view revision_id);

    std::string root_;
    std::string database_path_;
    sqlite3* db_ = nullptr;
    mutable std::recursive_mutex mutex_;
};

// Helpers for SQLite file permissions and recursive private directory creation.
bool ensure_private_directory(const std::string& path, std::string& error);
Error sqlite_error_to_error(int code, const std::string& context);

std::string now_iso8601();

} // namespace edgedesk::extensions
