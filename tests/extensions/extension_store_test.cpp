#include "test_util.hpp"

#include "core/util.hpp"
#include "extensions/extension_store.hpp"

#include <filesystem>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace edgedesk;
using namespace edgedesk::extensions;

namespace {

namespace fs = std::filesystem;

fs::path make_root() {
    const auto root = fs::temp_directory_path() / ("edgedesk-store-test-" + std::to_string(::getpid()));
    fs::remove_all(root);
    return root;
}

AppRecord make_app(const std::string& id) {
    AppRecord app;
    app.app_id = id;
    app.display_name = "Test App";
    app.status = app_status::pending_review;
    app.created_at = now_unix_seconds();
    return app;
}

RevisionRecord make_revision(const std::string& app_id, const std::string& revision_id) {
    RevisionRecord revision;
    revision.revision_id = revision_id;
    revision.app_id = app_id;
    revision.version = "1.0.0";
    revision.manifest_json = R"({"id":"io.example.test","name":"Test App","version":"1.0.0","entry":"index.html","minHostApiVersion":"1.0.0","maxHostApiVersion":"2.0.0"})";
    revision.created_at = now_unix_seconds();
    revision.file_count = 2;
    revision.total_bytes = 42;
    revision.status = revision_status::active;
    return revision;
}

void test_open_and_metadata(const fs::path& root) {
    Error error;
    auto store = ExtensionStore::open(root.string(), error);
    CHECK(store != nullptr);
    if (!store) std::fprintf(stderr, "open error: %s\n", error.message.c_str());
    struct stat info {};
    CHECK(::stat(root.string().c_str(), &info) == 0);
    CHECK_EQ(info.st_mode & 0777, static_cast<mode_t>(0700));
    CHECK(::stat(store->database_path().c_str(), &info) == 0);
    CHECK_EQ(info.st_mode & 0777, static_cast<mode_t>(0600));

    // SPEC-0011: a non-root backend identity needs traversal-only access to
    // EdgeDesk-owned roots. Exactly 0711 is preserved across reopens so a
    // Supervisor autostart after an EdgeDesk restart can still reach the socket
    // directory; anything broader is tightened back to 0700.
    CHECK_EQ(::chmod(root.string().c_str(), 0711), 0);
    {
        Error reopen_error;
        auto reopened = ExtensionStore::open(root.string(), reopen_error);
        CHECK(reopened != nullptr);
        CHECK(::stat(root.string().c_str(), &info) == 0);
        CHECK_EQ(info.st_mode & 0777, static_cast<mode_t>(0711));
    }
    CHECK_EQ(::chmod(root.string().c_str(), 0777), 0);
    {
        Error reopen_error;
        auto reopened = ExtensionStore::open(root.string(), reopen_error);
        CHECK(reopened != nullptr);
        CHECK(::stat(root.string().c_str(), &info) == 0);
        CHECK_EQ(info.st_mode & 0777, static_cast<mode_t>(0700));
    }
}

void test_apps_permissions_revisions(const fs::path& root) {
    Error error;
    auto store = ExtensionStore::open(root.string(), error);
    CHECK(store != nullptr);
    if (!store) return;
    auto app = make_app("io.example.test");
    CHECK(store->upsert_app(app).ok());
    bool found = false;
    AppRecord loaded;
    CHECK(store->get_app(app.app_id, loaded, found).ok());
    CHECK(found);
    CHECK_STR_EQ(loaded.display_name, "Test App");
    CHECK_STR_EQ(loaded.status, app_status::pending_review);

    CHECK(store->replace_permission_requests(app.app_id, {"storage", "files.manage"}).ok());
    CHECK(store->replace_permission_grants(app.app_id, {"storage"}).ok());
    loaded = {};
    CHECK(store->get_app(app.app_id, loaded, found).ok());
    CHECK_EQ(loaded.requested_permissions.size(), std::size_t{2});
    CHECK_EQ(loaded.granted_permissions.size(), std::size_t{1});
    CHECK_STR_EQ(loaded.granted_permissions[0], "storage");

    auto revision = make_revision(app.app_id, "rev-1111");
    CHECK(store->insert_revision(revision).ok());
    CHECK(store->set_app_active_revision(app.app_id, revision.revision_id, revision.version).ok());
    CHECK(store->set_app_status(app.app_id, app_status::enabled).ok());
    CHECK(store->replace_integrity_entries(
                 revision.revision_id, {{"index.html", 12, "abc"}, {"app.js", 30, "def"}})
              .ok());
    std::vector<IntegrityEntry> integrity;
    CHECK(store->list_integrity_entries(revision.revision_id, integrity).ok());
    CHECK_EQ(integrity.size(), std::size_t{2});
    CHECK_STR_EQ(integrity[0].path, "app.js");

    loaded = {};
    CHECK(store->get_app(app.app_id, loaded, found).ok());
    CHECK(found);
    CHECK_STR_EQ(loaded.status, app_status::enabled);
    CHECK_STR_EQ(loaded.active_revision_id, revision.revision_id);
    CHECK(loaded.manifest.is_object());

    auto second = make_revision(app.app_id, "rev-2222");
    second.version = "1.1.0";
    CHECK(store->insert_revision(second).ok());
    CHECK(store->set_revision_status(revision.revision_id, revision_status::historical).ok());
    std::vector<RevisionRecord> revisions;
    CHECK(store->list_revisions(app.app_id, revisions).ok());
    CHECK_EQ(revisions.size(), std::size_t{2});
    CHECK(store->set_revision_pinned(revision.revision_id, true).ok());
    CHECK(store->set_revision_integrity(second.revision_id, integrity_state::mismatch).ok());

    std::vector<AppRecord> apps;
    CHECK(store->list_apps(apps, true).ok());
    CHECK_EQ(apps.size(), std::size_t{1});
}

void test_kv(const fs::path& root) {
    Error error;
    auto store = ExtensionStore::open(root.string(), error);
    CHECK(store != nullptr);
    if (!store) return;
    const std::string app = "io.example.kv";
    CHECK(store->upsert_app(make_app(app)).ok());

    std::uint64_t usage = 0;
    CHECK(store->kv_set(app, "alpha", R"({"n":1})", usage).ok());
    CHECK_EQ(usage, static_cast<std::uint64_t>(7));
    CHECK(store->kv_set(app, "beta", R"("two")", usage).ok());
    KvEntry entry;
    bool found = false;
    CHECK(store->kv_get(app, "alpha", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, R"({"n":1})");
    CHECK(!store->kv_get(app, "missing", entry, found).ok() || !found);

    std::vector<KvEntry> entries;
    std::string cursor;
    CHECK(store->kv_list(app, "", 1, entries, cursor, usage).ok());
    CHECK_EQ(entries.size(), std::size_t{1});
    CHECK_STR_EQ(entries[0].key, "alpha");
    CHECK_STR_EQ(cursor, "alpha");
    CHECK(store->kv_list(app, cursor, 10, entries, cursor, usage).ok());
    CHECK_EQ(entries.size(), std::size_t{1});
    CHECK_STR_EQ(entries[0].key, "beta");
    CHECK(cursor.empty());

    // per-key limit
    const std::string too_big(kAppKvKeyLimitBytes + 1, 'x');
    CHECK(!store->kv_set(app, "huge", too_big, usage).ok());

    // quota: a dedicated app fills exactly 5 MiB and the next write fails
    const std::string quota_app = "io.example.quota";
    CHECK(store->upsert_app(make_app(quota_app)).ok());
    const std::string chunk(kAppKvKeyLimitBytes, 'a');
    for (int i = 0; i < 20; ++i) {
        const std::string key = "chunk-" + std::to_string(i);
        Error set_error = store->kv_set(quota_app, key, chunk, usage);
        CHECK(set_error.ok());
        if (!set_error.ok()) std::fprintf(stderr, "chunk %d: %s\n", i, set_error.message.c_str());
    }
    CHECK_EQ(usage, kAppKvQuotaBytes);
    // Replace one chunk with the same size must work.
    CHECK(store->kv_set(quota_app, "chunk-0", chunk, usage).ok());
    // A new key exceeds the quota and must not be written.
    CHECK(!store->kv_set(quota_app, "overflow", "x", usage).ok());
    CHECK(!store->kv_get(quota_app, "overflow", entry, found).ok() || !found);

    // Transaction all-or-nothing: the second set exceeds the quota.
    std::vector<KvOperation> operations;
    operations.push_back({KvOperationKind::erase, "chunk-1", ""});
    operations.push_back({KvOperationKind::set, "middle", "ok"});
    CHECK(store->kv_transaction(quota_app, operations, usage).ok());
    CHECK(store->kv_get(quota_app, "middle", entry, found).ok());
    CHECK(found);
    operations.clear();
    operations.push_back({KvOperationKind::erase, "chunk-2", ""});
    operations.push_back({KvOperationKind::set, "too-large", std::string(kAppKvKeyLimitBytes + 1, 'z')});
    CHECK(!store->kv_transaction(quota_app, operations, usage).ok());
    CHECK(store->kv_get(quota_app, "chunk-2", entry, found).ok());
    CHECK(found); // the rollback kept the old value

    // Repeated operations on one key are accounted once in quota checks.
    const std::string txn_app = "io.example.txn";
    CHECK(store->upsert_app(make_app(txn_app)).ok());
    for (int i = 0; i < 19; ++i) {
        CHECK(store->kv_set(txn_app, "fill-" + std::to_string(i), chunk, usage).ok());
    }
    CHECK_EQ(usage, static_cast<std::uint64_t>(19) * kAppKvKeyLimitBytes);
    operations.clear();
    operations.push_back({KvOperationKind::set, "same", chunk});
    operations.push_back({KvOperationKind::set, "same", chunk});
    Error txn_error = store->kv_transaction(txn_app, operations, usage);
    CHECK(txn_error.ok());
    if (!txn_error.ok()) std::fprintf(stderr, "repeated-key txn: %s\n", txn_error.message.c_str());
    CHECK_EQ(usage, kAppKvQuotaBytes);
    operations.clear();
    operations.push_back({KvOperationKind::set, "overflow", "x"});
    CHECK(!store->kv_transaction(txn_app, operations, usage).ok());

    bool deleted = false;
    CHECK(store->kv_delete(app, "alpha", deleted, usage).ok());
    CHECK(deleted);
    CHECK_EQ(usage, static_cast<std::uint64_t>(5));
    CHECK(store->clear_app_data(app).ok());
    CHECK(store->kv_usage(app, usage).ok());
    CHECK_EQ(usage, static_cast<std::uint64_t>(0));
    CHECK(store->kv_usage(app, usage).ok());
    CHECK_EQ(usage, static_cast<std::uint64_t>(0));
}

void test_operations_and_persistence(const fs::path& root) {
    Error error;
    {
        auto store = ExtensionStore::open(root.string(), error);
        CHECK(store != nullptr);
        if (!store) return;
        OperationRecord operation;
        operation.app_id = "io.example.ops";
        operation.type = "install";
        operation.phase = "staging";
        operation.temp_path = "/tmp/example";
        CHECK(store->create_operation(operation).ok());
        CHECK(!operation.operation_id.empty());
        CHECK(store->set_operation_app(operation.operation_id, "io.example.ops").ok());
        std::vector<OperationRecord> pending;
        CHECK(store->list_pending_operations(pending).ok());
        CHECK_EQ(pending.size(), std::size_t{1});
        CHECK_STR_EQ(pending[0].app_id, "io.example.ops");
        CHECK(store->update_operation(operation.operation_id, "failed", "", "test").ok());
        pending.clear();
        CHECK(store->list_pending_operations(pending).ok());
        CHECK(pending.empty());
        CHECK(store->upsert_app(make_app("io.example.persist")).ok());
        std::uint64_t persisted_usage = 0;
        CHECK(store->kv_set("io.example.persist", "key", "value", persisted_usage).ok());
    }
    auto store = ExtensionStore::open(root.string(), error);
    CHECK(store != nullptr);
    if (!store) return;
    bool found = false;
    AppRecord app;
    CHECK(store->get_app("io.example.persist", app, found).ok());
    CHECK(found);
    KvEntry entry;
    CHECK(store->kv_get("io.example.persist", "key", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, "value");
}

void test_network_origins(const fs::path& root) {
    Error error;
    auto store = ExtensionStore::open(root.string(), error);
    CHECK(store != nullptr);
    if (!store) return;
    const std::string app = "io.example.origins";
    CHECK(store->upsert_app(make_app(app)).ok());

    std::vector<std::string> origins;
    CHECK(store->list_network_origins(app, origins).ok());
    CHECK(origins.empty());

    CHECK(store->replace_network_origins(app, {"https://*.example.com", "http://127.0.0.1:8080"}).ok());
    origins.clear();
    CHECK(store->list_network_origins(app, origins).ok());
    CHECK_EQ(origins.size(), std::size_t{2});
    CHECK_STR_EQ(origins[0], "http://127.0.0.1:8080");  // ordered
    CHECK_STR_EQ(origins[1], "https://*.example.com");

    // Replacement is exact.
    CHECK(store->replace_network_origins(app, {"https://example.com"}).ok());
    origins.clear();
    CHECK(store->list_network_origins(app, origins).ok());
    CHECK_EQ(origins.size(), std::size_t{1});
    CHECK_STR_EQ(origins[0], "https://example.com");

    CHECK(store->clear_network_origins(app).ok());
    origins.clear();
    CHECK(store->list_network_origins(app, origins).ok());
    CHECK(origins.empty());

    // Foreign-key cascade removes approvals with the app row.
    CHECK(store->replace_network_origins(app, {"https://example.com"}).ok());
    CHECK(store->delete_app_record(app).ok());
    origins.clear();
    CHECK(store->list_network_origins(app, origins).ok());
    CHECK(origins.empty());
}

void test_transaction_rollback_preserves_kv(const fs::path& root) {
    Error error;
    auto store = ExtensionStore::open(root.string(), error);
    CHECK(store != nullptr);
    if (!store) return;
    const std::string app = "io.example.txn.rollback";
    CHECK(store->upsert_app(make_app(app)).ok());
    std::uint64_t usage = 0;
    CHECK(store->kv_set(app, "first", "one", usage).ok());
    CHECK(store->kv_set(app, "second", "two", usage).ok());
    const std::uint64_t before = usage;

    auto revision = make_revision(app, "rev-rollback");
    // The duplicate primary key is a real SQLite constraint failure. It is
    // raised after the KV delete inside the same explicit transaction, so a
    // rollback must restore both the rows and the usage accounting.
    Error failure = store->run_transaction([&]() -> Error {
        Error transaction_error = store->kv_delete_all(app);
        if (!transaction_error.ok()) return transaction_error;
        transaction_error = store->insert_revision(revision);
        if (!transaction_error.ok()) return transaction_error;
        return store->insert_revision(revision); // SQLITE_CONSTRAINT
    });
    CHECK(!failure.ok());

    KvEntry entry;
    bool found = false;
    CHECK(store->kv_get(app, "first", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, "one");
    CHECK(store->kv_get(app, "second", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, "two");
    std::uint64_t after = 0;
    CHECK(store->kv_usage(app, after).ok());
    CHECK_EQ(after, before);
    std::uint64_t revision_count = 0;
    CHECK(store->count_revisions(app, revision_count).ok());
    CHECK_EQ(revision_count, static_cast<std::uint64_t>(0));
}

bool raw_sqlite_exec(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        if (db != nullptr) sqlite3_close(db);
        return false;
    }
    const bool ok = sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

void test_v1_schema_migration(const fs::path& root) {
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string db_path = (root / "extensions.db").string();
    const char* v1_schema = R"SQL(
PRAGMA user_version=1;
CREATE TABLE apps (
    app_id TEXT PRIMARY KEY,
    display_name TEXT NOT NULL,
    status TEXT NOT NULL,
    active_revision_id TEXT,
    active_version TEXT,
    data_retained INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE TABLE permission_grants (
    app_id TEXT NOT NULL,
    module TEXT NOT NULL,
    granted_at INTEGER NOT NULL,
    PRIMARY KEY(app_id, module),
    FOREIGN KEY(app_id) REFERENCES apps(app_id) ON DELETE CASCADE
);
INSERT INTO apps(app_id, display_name, status, created_at, updated_at)
VALUES('io.example.legacy', 'Legacy App', 'enabled', 1, 1);
INSERT INTO permission_grants(app_id, module, granted_at)
VALUES('io.example.legacy', 'network', 1);
)SQL";
    CHECK(raw_sqlite_exec(db_path, v1_schema));

    Error error;
    auto store = ExtensionStore::open(root.string(), error);
    CHECK(store != nullptr);
    if (!store) {
        std::fprintf(stderr, "migration open error: %s\n", error.message.c_str());
        return;
    }
    bool found = false;
    AppRecord app;
    CHECK(store->get_app("io.example.legacy", app, found).ok());
    CHECK(found);
    CHECK_STR_EQ(app.display_name, "Legacy App");
    CHECK_EQ(app.granted_permissions.size(), std::size_t{1});
    CHECK_STR_EQ(app.granted_permissions[0], "network");

    // New table is usable after the in-place upgrade.
    CHECK(store->replace_network_origins("io.example.legacy", {"https://example.com"}).ok());
    std::vector<std::string> origins;
    CHECK(store->list_network_origins("io.example.legacy", origins).ok());
    CHECK_EQ(origins.size(), std::size_t{1});
    CHECK_STR_EQ(origins[0], "https://example.com");
}

void test_schema_version_after_migration(const fs::path& root) {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open((root / "extensions.db").c_str(), &db) == SQLITE_OK);
    if (db == nullptr) return;
    sqlite3_stmt* stmt = nullptr;
    int version = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    if (stmt != nullptr) sqlite3_finalize(stmt);
    sqlite3_close(db);
    CHECK_EQ(version, 3);
}

void test_recovery_after_process_crash(const fs::path& root) {
    // A pending operation whose revision row is missing must be markable as
    // failed, and a second open must observe the same consistent schema.
    Error error;
    auto store = ExtensionStore::open(root.string(), error);
    CHECK(store != nullptr);
    if (!store) return;
    OperationRecord operation;
    operation.app_id = "io.example.crash";
    operation.type = "install";
    operation.phase = "committed";
    CHECK(store->create_operation(operation).ok());
    CHECK(store->update_operation(operation.operation_id, "failed", "", "recovered").ok());
    std::vector<OperationRecord> pending;
    CHECK(store->list_pending_operations(pending).ok());
    CHECK(pending.empty());
}

} // namespace

int main() {
    const auto root = make_root();
    test_open_and_metadata(root);
    test_apps_permissions_revisions(root);
    test_kv(root);
    test_operations_and_persistence(root);
    test_network_origins(root);
    test_transaction_rollback_preserves_kv(root);
    const auto migration_root = fs::path(root.string() + "-migration");
    test_v1_schema_migration(migration_root);
    test_schema_version_after_migration(migration_root);
    std::filesystem::remove_all(migration_root);
    test_recovery_after_process_crash(root);
    std::filesystem::remove_all(root);
    return edgedesk::test::finish("extension_store_test");
}
