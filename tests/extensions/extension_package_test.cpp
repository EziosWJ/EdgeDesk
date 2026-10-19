#include "test_util.hpp"
#include "extensions/zip_builder.hpp"

#include "extensions/package_manager.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <sys/stat.h>

using namespace edgedesk;
using namespace edgedesk::extensions;
using edgedesk::test::ZipItem;

namespace {

namespace fs = std::filesystem;

// Fault injection uses the real database through a second connection: an
// aborting trigger makes the install commit transaction fail exactly where the
// test needs it, without adding production-only hooks.
bool run_sql(const std::string& database, const std::string& sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(database.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (db != nullptr) sqlite3_close(db);
        return false;
    }
    char* message = nullptr;
    const int result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    if (message != nullptr) sqlite3_free(message);
    sqlite3_close(db);
    return result == SQLITE_OK;
}

std::string fail_revision_insert_trigger(const std::string& app_id) {
    return "CREATE TRIGGER injected_revision_failure BEFORE INSERT ON revisions WHEN NEW.app_id='" +
           app_id + "' BEGIN SELECT RAISE(ABORT,'injected revision failure'); END;";
}

std::string fail_kv_delete_trigger(const std::string& app_id) {
    return "CREATE TRIGGER injected_kv_delete_failure BEFORE DELETE ON app_kv WHEN OLD.app_id='" +
           app_id + "' BEGIN SELECT RAISE(ABORT,'injected kv delete failure'); END;";
}

std::size_t directory_entries(const fs::path& path) {
    if (!fs::exists(path)) return 0;
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(path)) {
        (void)entry;
        ++count;
    }
    return count;
}

fs::path make_base() {
    const auto base = fs::temp_directory_path() / ("edgedesk-package-test-" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

nlohmann::json manifest_json(const std::string& id, const std::string& version,
                             const std::vector<std::string>& permissions = {}) {
    nlohmann::json manifest = {
        {"id", id},
        {"name", "Test Application"},
        {"version", version},
        {"entry", "index.html"},
        {"minHostApiVersion", "1.0.0"},
        {"maxHostApiVersion", "1.9.9"},
        {"permissions", permissions},
        {"window", {{"defaultWidth", 800}, {"defaultHeight", 600}, {"minWidth", 320}, {"minHeight", 240},
                     {"instances", "single"}}},
    };
    return manifest;
}

std::vector<ZipItem> app_items(const nlohmann::json& manifest, const std::string& index,
                               const std::string& script = "console.log('ok')") {
    return {
        {"manifest.json", manifest.dump(), 0, false, ""},
        {"index.html", index, 0, false, ""},
        {"assets/app.js", script, 0, true, ""},
    };
}

std::string make_zip(const fs::path& base, const std::string& name, const std::vector<ZipItem>& items) {
    const auto path = (base / name).string();
    CHECK(edgedesk::test::write_zip(path, items));
    return path;
}

void test_install_approve_static(const fs::path& base) {
    const auto apps_root = (base / "apps").string();
    Error error;
    auto manager = PackageManager::create(apps_root, error);
    CHECK(manager != nullptr);
    if (!manager) {
        std::fprintf(stderr, "manager error: %s\n", error.message.c_str());
        return;
    }
    const auto zip = make_zip(base, "app-1.zip",
                              app_items(manifest_json("io.example.app", "1.0.0"), "<h1>one</h1>"));
    InstallResult result;
    error = manager->install(zip, {}, result);
    CHECK(error.ok());
    if (!error.ok()) {
        std::fprintf(stderr, "install error: %s\n", error.message.c_str());
        return;
    }
    CHECK_STR_EQ(result.app.status, app_status::pending_review);
    CHECK(result.approval_required);
    CHECK(!result.revision.revision_id.empty());
    CHECK_EQ(result.revision.revision_id.size(), std::size_t{32});
    CHECK_EQ(result.manifest.id, "io.example.app");
    CHECK(!result.publisher_verified);

    StaticAsset asset;
    CHECK(!manager->read_static("io.example.app", "1.0.0", result.revision.revision_id, "", asset).ok());
    AppRecord approved;
    error = manager->approve("io.example.app", {}, true, approved);
    CHECK(error.ok());
    if (!error.ok()) std::fprintf(stderr, "approve error: %s\n", error.message.c_str());
    CHECK_STR_EQ(approved.status, app_status::enabled);
    CHECK(manager->read_static("io.example.app", "1.0.0", result.revision.revision_id, "", asset).ok());
    CHECK_STR_EQ(asset.body, "<h1>one</h1>");
    CHECK(asset.mime.find("text/html") != std::string::npos);
    CHECK(!asset.etag.empty());
    CHECK(asset.csp.find("default-src 'self'") != std::string::npos);
    CHECK(manager->read_static("io.example.app", "1.0.0", result.revision.revision_id, "route/deep", asset).ok());
    CHECK_STR_EQ(asset.body, "<h1>one</h1>"); // SPA fallback
    CHECK(!manager->read_static("io.example.app", "1.0.0", result.revision.revision_id, "missing.js", asset).ok());
    CHECK(!manager->read_static("io.example.app", "1.0.0", result.revision.revision_id, "../manifest.json", asset)
               .ok());
    CHECK(!manager->read_static("io.example.app", "9.9.9", result.revision.revision_id, "", asset).ok());
    CHECK(manager->read_static("io.example.app", "1.0.0", result.revision.revision_id, "assets/app.js", asset).ok());
    CHECK_STR_EQ(asset.body, "console.log('ok')");
    CHECK_EQ(asset.mime, "text/javascript");

    std::vector<RevisionRecord> revisions;
    CHECK(manager->list_revisions("io.example.app", revisions).ok());
    CHECK_EQ(revisions.size(), std::size_t{1});
}

void test_same_version_and_upgrade(const fs::path& base) {
    const std::string apps_root = (base / "apps-upgrade").string();
    Error error;
    auto manager = PackageManager::create(apps_root, error);
    CHECK(manager != nullptr);
    if (!manager) return;
    const std::string id = "io.example.upgrade";
    const auto v1 = make_zip(base, "up-1.zip", app_items(manifest_json(id, "1.0.0"), "<h1>v1</h1>"));
    InstallResult first;
    CHECK(manager->install(v1, {}, first).ok());
    AppRecord app;
    CHECK(manager->approve(id, {}, true, app).ok());

    InstallResult duplicate;
    Error duplicate_error = manager->install(v1, {}, duplicate);
    CHECK(!duplicate_error.ok());
    CHECK(duplicate_error.code == ErrorCode::conflict);

    const auto v1b = make_zip(base, "up-1b.zip", app_items(manifest_json(id, "1.0.0"), "<h1>v1b</h1>"));
    InstallOptions overwrite;
    overwrite.overwrite_same_version = true;
    InstallResult overwritten;
    CHECK(manager->install(v1b, overwrite, overwritten).ok());
    CHECK(overwritten.revision.revision_id != first.revision.revision_id);
    CHECK_STR_EQ(overwritten.app.status, app_status::enabled);
    StaticAsset asset;
    CHECK(manager->read_static(id, "1.0.0", overwritten.revision.revision_id, "", asset).ok());
    CHECK_STR_EQ(asset.body, "<h1>v1b</h1>");

    const auto v2 = make_zip(base, "up-2.zip",
                             app_items(manifest_json(id, "1.1.0", {"files.manage"}), "<h1>v2</h1>"));
    InstallOptions upgrade;
    upgrade.expected_app_id = id;
    InstallResult second;
    CHECK(manager->install(v2, upgrade, second).ok());
    CHECK_STR_EQ(second.app.status, app_status::enabled);
    CHECK_EQ(second.app.requested_permissions.size(), std::size_t{1});
    CHECK(second.app.granted_permissions.empty()); // new permission still requires approval
    CHECK(manager->read_static(id, "1.1.0", second.revision.revision_id, "", asset).ok());
    CHECK_STR_EQ(asset.body, "<h1>v2</h1>");

    // Downgrade to the previous revision requires confirmation.
    CHECK(!manager->activate_revision(id, overwritten.revision.revision_id, false, app).ok());
    CHECK(manager->activate_revision(id, overwritten.revision.revision_id, true, app).ok());
    CHECK_STR_EQ(app.active_revision_id, overwritten.revision.revision_id);
    CHECK(manager->read_static(id, "1.0.0", overwritten.revision.revision_id, "", asset).ok());
    CHECK_STR_EQ(asset.body, "<h1>v1b</h1>");

    // Older package needs an explicit downgrade flag.
    const auto old = make_zip(base, "up-old.zip", app_items(manifest_json(id, "0.9.0"), "<h1>old</h1>"));
    InstallResult old_result;
    CHECK(!manager->install(old, {}, old_result).ok());
    InstallOptions downgrade;
    downgrade.allow_downgrade = true;
    CHECK(manager->install(old, downgrade, old_result).ok());
    CHECK_STR_EQ(old_result.app.active_version, "0.9.0");

    // Wrong target id is rejected for upgrade uploads.
    const auto other = make_zip(base, "other.zip", app_items(manifest_json("io.example.other", "1.0.0"), "<h1>x</h1>"));
    InstallResult wrong;
    InstallOptions wrong_options;
    wrong_options.expected_app_id = id;
    CHECK(!manager->install(other, wrong_options, wrong).ok());
}

void test_integrity_isolation(const fs::path& base) {
    const std::string apps_root = (base / "apps-integrity").string();
    Error error;
    auto manager = PackageManager::create(apps_root, error);
    CHECK(manager != nullptr);
    if (!manager) return;
    const std::string id = "io.example.integrity";
    const auto zip = make_zip(base, "int-1.zip",
                              app_items(manifest_json(id, "1.0.0", {"storage"}), "<h1>int</h1>"));
    InstallResult installed;
    CHECK(manager->install(zip, {}, installed).ok());
    AppRecord app;
    CHECK(manager->approve(id, {"storage"}, true, app).ok());
    IntegrityResult check;
    CHECK(manager->check_integrity(id, "", check).ok());
    CHECK(check.ok);

    std::string revision_dir;
    CHECK(manager->revision_directory(id, installed.revision.revision_id, revision_dir).ok());
    {
        std::ofstream file(fs::path(revision_dir) / "index.html", std::ios::trunc);
        file << "<h1>tampered</h1>";
    }
    check = {};
    CHECK(manager->check_integrity(id, "", check).ok());
    CHECK(!check.ok);
    CHECK(check.app_isolated);
    CHECK_STR_EQ(check.mismatch_path, "index.html");
    bool found = false;
    CHECK(manager->get_app(id, app, found).ok());
    CHECK(found);
    CHECK_STR_EQ(app.status, app_status::isolated);
    CHECK(app.granted_permissions.empty());
    StaticAsset asset;
    CHECK(!manager->read_static(id, "1.0.0", installed.revision.revision_id, "", asset).ok());

    // Reinstall repairs the app but requires a fresh approval.
    const auto repaired = make_zip(base, "int-2.zip", app_items(manifest_json(id, "1.0.0"), "<h1>fixed</h1>"));
    InstallOptions overwrite;
    overwrite.overwrite_same_version = true;
    InstallResult reinstalled;
    CHECK(manager->install(repaired, overwrite, reinstalled).ok());
    CHECK_STR_EQ(reinstalled.app.status, app_status::pending_review);
    CHECK(manager->approve(id, {}, true, app).ok());
    CHECK(manager->read_static(id, "1.0.0", reinstalled.revision.revision_id, "", asset).ok());
    CHECK_STR_EQ(asset.body, "<h1>fixed</h1>");
}

void test_reject_bad_packages(const fs::path& base) {
    const std::string apps_root = (base / "apps-bad").string();
    Error error;
    auto manager = PackageManager::create(apps_root, error);
    CHECK(manager != nullptr);
    if (!manager) return;
    const auto good_manifest = manifest_json("io.example.bad", "1.0.0");

    auto install_named = [&](const std::string& name, const std::vector<ZipItem>& items) {
        InstallResult result;
        const auto path = make_zip(base, name, items);
        return manager->install(path, {}, result);
    };

    CHECK(!install_named("bad-no-index.zip",
                         {{"manifest.json", good_manifest.dump(), 0, false, ""}})
               .ok());
    CHECK(!install_named("bad-no-manifest.zip", {{"index.html", "x", 0, false, ""}}).ok());
    CHECK(!install_named("bad-json.zip",
                         {{"manifest.json", "{", 0, false, ""}, {"index.html", "x", 0, false, ""}})
               .ok());
    CHECK(!install_named("bad-traversal.zip",
                         {{"manifest.json", good_manifest.dump(), 0, false, ""},
                          {"index.html", "x", 0, false, ""},
                          {"../evil.txt", "x", 0, false, ""}})
               .ok());
    CHECK(!install_named("bad-absolute.zip",
                         {{"manifest.json", good_manifest.dump(), 0, false, ""},
                          {"index.html", "x", 0, false, ""},
                          {"/etc/evil", "x", 0, false, ""}})
               .ok());
    CHECK(!install_named("bad-backslash.zip",
                         {{"manifest.json", good_manifest.dump(), 0, false, ""},
                          {"index.html", "x", 0, false, ""},
                          {"dir\\evil", "x", 0, false, ""}})
               .ok());
    CHECK(!install_named("bad-symlink.zip",
                         {{"manifest.json", good_manifest.dump(), 0, false, ""},
                          {"index.html", "x", 0, false, ""},
                          {"link", "/etc/passwd", S_IFLNK | 0777, false, ""}})
               .ok());
    CHECK(!install_named("bad-duplicate.zip",
                         {{"manifest.json", good_manifest.dump(), 0, false, ""},
                          {"index.html", "x", 0, false, ""},
                          {"Asset.js", "a", 0, false, ""},
                          {"asset.js", "b", 0, false, ""}})
               .ok());
    CHECK(!install_named("bad-local-name.zip",
                         {{"manifest.json", good_manifest.dump(), 0, false, ""},
                          {"index.html", "x", 0, false, ""},
                          {"asset.js", "a", 0, false, "other.js"}})
               .ok());
    CHECK(!install_named("bad-utf8-name.zip",
                         {{"manifest.json", good_manifest.dump(), 0, false, ""},
                          {"index.html", "x", 0, false, ""},
                          {std::string("bad-\xFF.js"), "a", 0, false, ""}})
               .ok());
    CHECK(!install_named("bad-host-api.zip",
                         {{"manifest.json",
                           nlohmann::json({{"id", "io.example.badapi"},
                                           {"name", "Bad API"},
                                           {"version", "1.0.0"},
                                           {"entry", "index.html"},
                                           {"minHostApiVersion", "2.0.0"},
                                           {"maxHostApiVersion", "3.0.0"}})
                               .dump(),
                           0, false, ""},
                          {"index.html", "x", 0, false, ""}})
               .ok());
}

void test_uninstall_and_retention(const fs::path& base) {
    const std::string apps_root = (base / "apps-uninstall").string();
    Error error;
    auto manager = PackageManager::create(apps_root, error);
    CHECK(manager != nullptr);
    if (!manager) return;
    const std::string id = "io.example.data";
    const auto zip = make_zip(base, "data-1.zip", app_items(manifest_json(id, "1.0.0", {"storage"}), "v1"));
    InstallResult installed;
    CHECK(manager->install(zip, {}, installed).ok());
    AppRecord app;
    CHECK(manager->approve(id, {"storage"}, true, app).ok());
    std::uint64_t usage = 0;
    CHECK(manager->store().kv_set(id, "keep", R"({"v":1})", usage).ok());

    CHECK(manager->uninstall(id, true).ok());
    bool found = false;
    CHECK(manager->get_app(id, app, found).ok());
    CHECK(found);
    CHECK_STR_EQ(app.status, app_status::uninstalled);
    CHECK(app.data_retained);
    KvEntry entry;
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(found);

    // Reinstall without restore clears retained data.
    InstallOptions reinstall;
    reinstall.overwrite_same_version = true;
    InstallResult reinstalled;
    CHECK(manager->install(zip, reinstall, reinstalled).ok());
    CHECK_STR_EQ(reinstalled.app.status, app_status::pending_review);
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(!found);
    CHECK(manager->approve(id, {"storage"}, true, app).ok());
    CHECK(manager->store().kv_set(id, "keep", "again", usage).ok());
    CHECK(manager->uninstall(id, true).ok());

    InstallOptions restore;
    restore.overwrite_same_version = true;
    restore.restore_retained_data = true;
    InstallResult restored;
    CHECK(manager->install(zip, restore, restored).ok());
    CHECK(restored.retained_data_restored);
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, "again");

    // Uninstall with delete removes data and the app row.
    CHECK(manager->approve(id, {"storage"}, true, app).ok());
    CHECK(manager->uninstall(id, false).ok());
    CHECK(manager->get_app(id, app, found).ok());
    CHECK(!found);
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(!found);

    // Retention: active + two historical revisions, pinned revisions survive.
    const std::string retain_id = "io.example.retain";
    std::vector<std::string> revision_ids;
    for (int index = 0; index < 5; ++index) {
        const std::string version = "1." + std::to_string(index) + ".0";
        const auto path = make_zip(base, "retain-" + std::to_string(index) + ".zip",
                                   app_items(manifest_json(retain_id, version), "v" + std::to_string(index)));
        InstallResult result;
        InstallOptions options;
        options.expected_app_id = index == 0 ? std::string() : retain_id;
        CHECK(manager->install(path, options, result).ok());
        revision_ids.push_back(result.revision.revision_id);
        if (index == 0) {
            AppRecord retain_app;
            CHECK(manager->approve(retain_id, {}, true, retain_app).ok());
        }
    }
    std::vector<RevisionRecord> revisions;
    CHECK(manager->list_revisions(retain_id, revisions).ok());
    CHECK_EQ(revisions.size(), std::size_t{3});
    // Pin the oldest remaining revision and ensure cleanup keeps it.
    const std::string pinned = revisions.back().revision_id;
    CHECK(manager->pin_revision(retain_id, pinned, true).ok());
    const auto sixth = make_zip(base, "retain-5.zip",
                                app_items(manifest_json(retain_id, "1.5.0"), "v5"));
    InstallResult sixth_result;
    InstallOptions sixth_options;
    sixth_options.expected_app_id = retain_id;
    CHECK(manager->install(sixth, sixth_options, sixth_result).ok());
    revisions.clear();
    CHECK(manager->list_revisions(retain_id, revisions).ok());
    CHECK_EQ(revisions.size(), std::size_t{4}); // active + two newest history + pinned
    bool pinned_found = false;
    for (const auto& revision : revisions) pinned_found = pinned_found || revision.revision_id == pinned;
    CHECK(pinned_found);
}

void test_retained_restore_survives_failed_install(const fs::path& base) {
    const std::string apps_root = (base / "apps-retained-restore").string();
    Error error;
    auto manager = PackageManager::create(apps_root, error);
    CHECK(manager != nullptr);
    if (!manager) return;
    const std::string id = "io.example.retained.restore";
    const auto zip = make_zip(base, "retained-restore.zip",
                              app_items(manifest_json(id, "1.0.0", {"storage"}), "v1"));
    InstallResult installed;
    CHECK(manager->install(zip, {}, installed).ok());
    AppRecord app;
    CHECK(manager->approve(id, {"storage"}, true, app).ok());
    std::uint64_t usage = 0;
    CHECK(manager->store().kv_set(id, "keep", R"({"v":1})", usage).ok());
    CHECK(manager->uninstall(id, true).ok());

    const std::string database = manager->store().database_path();
    CHECK(run_sql(database, fail_revision_insert_trigger(id)));
    InstallOptions restore;
    restore.restore_retained_data = true;
    InstallResult failed;
    CHECK(!manager->install(zip, restore, failed).ok());
    CHECK(run_sql(database, "DROP TRIGGER injected_revision_failure"));

    // The uninstalled app row, retained flag, KV and revision state survived.
    KvEntry entry;
    bool found = false;
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, R"({"v":1})");
    AppRecord after_failure;
    CHECK(manager->get_app(id, after_failure, found).ok());
    CHECK(found);
    CHECK_STR_EQ(after_failure.status, app_status::uninstalled);
    CHECK(after_failure.data_retained);
    std::vector<RevisionRecord> revisions;
    CHECK(manager->list_revisions(id, revisions).ok());
    CHECK(revisions.empty());
    CHECK_EQ(directory_entries(fs::path(apps_root) / "revisions" / id), std::size_t{0});

    InstallResult restored;
    CHECK(manager->install(zip, restore, restored).ok());
    CHECK(restored.retained_data_restored);
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, R"({"v":1})");
}

void test_retained_delete_is_atomic_with_install(const fs::path& base) {
    const std::string apps_root = (base / "apps-retained-delete").string();
    Error error;
    auto manager = PackageManager::create(apps_root, error);
    CHECK(manager != nullptr);
    if (!manager) return;
    const std::string id = "io.example.retained.delete";
    const auto zip = make_zip(base, "retained-delete.zip",
                              app_items(manifest_json(id, "1.0.0", {"storage"}), "v1"));
    InstallResult installed;
    CHECK(manager->install(zip, {}, installed).ok());
    AppRecord app;
    CHECK(manager->approve(id, {"storage"}, true, app).ok());
    std::uint64_t usage = 0;
    CHECK(manager->store().kv_set(id, "keep", "before", usage).ok());
    CHECK(manager->uninstall(id, true).ok());

    const std::string database = manager->store().database_path();
    // Fail inside the final commit transaction after the revision directory has
    // already been moved into place: the KV delete must not have happened yet.
    CHECK(run_sql(database, fail_revision_insert_trigger(id)));
    InstallResult failed;
    CHECK(!manager->install(zip, {}, failed).ok()); // explicitly no restore
    CHECK(run_sql(database, "DROP TRIGGER injected_revision_failure"));

    KvEntry entry;
    bool found = false;
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, "before");
    AppRecord after_failure;
    CHECK(manager->get_app(id, after_failure, found).ok());
    CHECK(found);
    CHECK_STR_EQ(after_failure.status, app_status::uninstalled);
    CHECK(after_failure.data_retained);
    std::vector<RevisionRecord> revisions;
    CHECK(manager->list_revisions(id, revisions).ok());
    CHECK(revisions.empty());
    CHECK_EQ(directory_entries(fs::path(apps_root) / "revisions" / id), std::size_t{0});

    // A real SQLite failure at the delete statement itself also rolls back the
    // app/revision writes that preceded it in the same transaction.
    CHECK(run_sql(database, fail_kv_delete_trigger(id)));
    InstallResult failed_delete;
    CHECK(!manager->install(zip, {}, failed_delete).ok());
    CHECK(run_sql(database, "DROP TRIGGER injected_kv_delete_failure"));
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, "before");
    CHECK(manager->list_revisions(id, revisions).ok());
    CHECK(revisions.empty());

    // Only a committed install deletes the retained KV.
    InstallResult reinstalled;
    CHECK(manager->install(zip, {}, reinstalled).ok());
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(!found);
    AppRecord after_success;
    CHECK(manager->get_app(id, after_success, found).ok());
    CHECK(found);
    CHECK_STR_EQ(after_success.status, app_status::pending_review);
    CHECK(!after_success.data_retained);
    CHECK(manager->list_revisions(id, revisions).ok());
    CHECK_EQ(revisions.size(), std::size_t{1});
    std::string revision_dir;
    CHECK(manager->revision_directory(id, reinstalled.revision.revision_id, revision_dir).ok());
    CHECK(fs::exists(revision_dir));
}

void test_stale_retained_rows_deleted_only_on_success(const fs::path& base) {
    const std::string apps_root = (base / "apps-stale-retained").string();
    Error error;
    auto manager = PackageManager::create(apps_root, error);
    CHECK(manager != nullptr);
    if (!manager) return;
    const std::string id = "io.example.stale";
    const auto zip = make_zip(base, "stale.zip", app_items(manifest_json(id, "1.0.0", {"storage"}), "v1"));
    InstallResult installed;
    CHECK(manager->install(zip, {}, installed).ok());
    AppRecord app;
    CHECK(manager->approve(id, {"storage"}, true, app).ok());
    std::uint64_t usage = 0;
    CHECK(manager->store().kv_set(id, "keep", "stale", usage).ok());
    CHECK(manager->uninstall(id, true).ok());
    // Force the stale combination: rows exist but the retained flag is gone.
    CHECK(manager->store().set_data_retained(id, false).ok());

    const std::string database = manager->store().database_path();
    CHECK(run_sql(database, fail_revision_insert_trigger(id)));
    InstallResult failed;
    CHECK(!manager->install(zip, {}, failed).ok());
    CHECK(run_sql(database, "DROP TRIGGER injected_revision_failure"));

    KvEntry entry;
    bool found = false;
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, "stale");

    InstallResult reinstalled;
    CHECK(manager->install(zip, {}, reinstalled).ok());
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(!found);
}

void test_upgrade_failure_preserves_kv_and_active_revision(const fs::path& base) {
    const std::string apps_root = (base / "apps-upgrade-failure").string();
    Error error;
    auto manager = PackageManager::create(apps_root, error);
    CHECK(manager != nullptr);
    if (!manager) return;
    const std::string id = "io.example.upgrade.failure";
    const auto v1 = make_zip(base, "upgrade-fail-1.zip",
                             app_items(manifest_json(id, "1.0.0", {"storage"}), "v1"));
    InstallResult installed;
    CHECK(manager->install(v1, {}, installed).ok());
    AppRecord app;
    CHECK(manager->approve(id, {"storage"}, true, app).ok());
    std::uint64_t usage = 0;
    CHECK(manager->store().kv_set(id, "keep", "upgrade-state", usage).ok());
    const std::string old_revision = installed.revision.revision_id;
    CHECK_STR_EQ(app.active_revision_id, old_revision);

    const auto v2 = make_zip(base, "upgrade-fail-2.zip",
                             app_items(manifest_json(id, "1.1.0", {"storage"}), "v2"));
    InstallOptions upgrade;
    upgrade.expected_app_id = id;
    const std::string database = manager->store().database_path();
    CHECK(run_sql(database, fail_revision_insert_trigger(id)));
    InstallResult failed;
    CHECK(!manager->install(v2, upgrade, failed).ok());
    CHECK(run_sql(database, "DROP TRIGGER injected_revision_failure"));

    KvEntry entry;
    bool found = false;
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, "upgrade-state");
    AppRecord after_failure;
    CHECK(manager->get_app(id, after_failure, found).ok());
    CHECK(found);
    CHECK_STR_EQ(after_failure.status, app_status::enabled);
    CHECK_STR_EQ(after_failure.active_revision_id, old_revision);
    CHECK_STR_EQ(after_failure.active_version, "1.0.0");
    CHECK_EQ(after_failure.granted_permissions.size(), std::size_t{1});
    CHECK_STR_EQ(after_failure.granted_permissions[0], "storage");
    std::vector<RevisionRecord> revisions;
    CHECK(manager->list_revisions(id, revisions).ok());
    CHECK_EQ(revisions.size(), std::size_t{1});
    CHECK_EQ(directory_entries(fs::path(apps_root) / "revisions" / id), std::size_t{1});
    std::string old_dir;
    CHECK(manager->revision_directory(id, old_revision, old_dir).ok());
    CHECK(fs::exists(old_dir));

    // A successful upgrade keeps the KV untouched.
    InstallResult upgraded;
    CHECK(manager->install(v2, upgrade, upgraded).ok());
    CHECK(manager->store().kv_get(id, "keep", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, "upgrade-state");
    AppRecord after_upgrade;
    CHECK(manager->get_app(id, after_upgrade, found).ok());
    CHECK(found);
    CHECK_STR_EQ(after_upgrade.active_revision_id, upgraded.revision.revision_id);
}

void test_recovery_preserves_kv_and_committed_install(const fs::path& base) {
    const std::string apps_root = (base / "apps-recovery-kv").string();
    Error error;
    auto manager = PackageManager::create(apps_root, error);
    CHECK(manager != nullptr);
    if (!manager) return;
    const std::string id = "io.example.recovery.kv";
    const auto zip = make_zip(base, "recovery-kv.zip",
                              app_items(manifest_json(id, "1.0.0", {"storage"}), "v1"));
    InstallResult installed;
    CHECK(manager->install(zip, {}, installed).ok());
    AppRecord app;
    CHECK(manager->approve(id, {"storage"}, true, app).ok());
    std::uint64_t usage = 0;
    CHECK(manager->store().kv_set(id, "keep", "crash-state", usage).ok());
    const std::string active_revision = installed.revision.revision_id;

    // Crash after the revision directory was moved into place but before the
    // metadata transaction committed: the journal points at an uncommitted
    // revision that recovery must clean up without touching the KV.
    OperationRecord inflight;
    inflight.type = "install";
    inflight.phase = "committed";
    inflight.temp_path = manager->temporary_directory() + "/op-inflight";
    CHECK(manager->store().create_operation(inflight).ok());
    CHECK(manager->store().set_operation_app(inflight.operation_id, id).ok());
    const std::string orphan_revision(32, 'a');
    CHECK(manager->store()
              .update_operation(inflight.operation_id, "committed", orphan_revision, "")
              .ok());
    const fs::path orphan_dir = fs::path(apps_root) / "revisions" / id / orphan_revision;
    fs::create_directories(orphan_dir);
    {
        std::ofstream file(orphan_dir / "index.html");
        file << "partial";
    }

    // Crash after the transaction committed but before the operation row was
    // marked done: recovery must keep the committed revision and KV.
    OperationRecord committed;
    committed.type = "upgrade";
    committed.phase = "committed";
    CHECK(manager->store().create_operation(committed).ok());
    CHECK(manager->store().set_operation_app(committed.operation_id, id).ok());
    CHECK(manager->store()
              .update_operation(committed.operation_id, "committed", active_revision, "")
              .ok());

    manager.reset();
    auto reopened = PackageManager::create(apps_root, error);
    CHECK(reopened != nullptr);
    if (!reopened) {
        std::fprintf(stderr, "reopen error: %s\n", error.message.c_str());
        return;
    }

    CHECK(!fs::exists(orphan_dir));
    KvEntry entry;
    bool found = false;
    CHECK(reopened->store().kv_get(id, "keep", entry, found).ok());
    CHECK(found);
    CHECK_STR_EQ(entry.value, "crash-state");
    AppRecord after_recovery;
    CHECK(reopened->get_app(id, after_recovery, found).ok());
    CHECK(found);
    CHECK_STR_EQ(after_recovery.status, app_status::enabled);
    CHECK_STR_EQ(after_recovery.active_revision_id, active_revision);
    std::string active_dir;
    CHECK(reopened->revision_directory(id, active_revision, active_dir).ok());
    CHECK(fs::exists(active_dir));
    std::vector<OperationRecord> pending;
    CHECK(reopened->store().list_pending_operations(pending).ok());
    CHECK(pending.empty());
}

void test_recovery(const fs::path& base) {
    const std::string apps_root = (base / "apps-recovery").string();
    Error error;
    auto manager = PackageManager::create(apps_root, error);
    CHECK(manager != nullptr);
    if (!manager) return;
    const fs::path orphan = fs::path(manager->temporary_directory()) / "op-orphan";
    fs::create_directories(orphan);
    {
        std::ofstream file(orphan / "partial");
        file << "partial";
    }
    const fs::path upload_orphan = fs::path(manager->temporary_directory()) / "upload-orphan.zip";
    {
        std::ofstream file(upload_orphan);
        file << "partial";
    }
    auto reopened = PackageManager::create(apps_root, error);
    CHECK(reopened != nullptr);
    CHECK(!fs::exists(orphan));
    CHECK(!fs::exists(upload_orphan));
}

} // namespace

int main() {
    const auto base = make_base();
    test_install_approve_static(base);
    test_same_version_and_upgrade(base);
    test_integrity_isolation(base);
    test_reject_bad_packages(base);
    test_uninstall_and_retention(base);
    test_retained_restore_survives_failed_install(base);
    test_retained_delete_is_atomic_with_install(base);
    test_stale_retained_rows_deleted_only_on_success(base);
    test_upgrade_failure_preserves_kv_and_active_revision(base);
    test_recovery_preserves_kv_and_committed_install(base);
    test_recovery(base);
    std::filesystem::remove_all(base);
    return edgedesk::test::finish("extension_package_test");
}
