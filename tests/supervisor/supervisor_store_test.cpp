#include "test_util.hpp"

#include "supervisor/supervisor_store.hpp"
#include "supervisor/supervisor_types.hpp"

#include <filesystem>
#include <fstream>
#include <grp.h>
#include <pwd.h>
#include <sqlite3.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace edgedesk;
using namespace edgedesk::supervisor;
namespace fs = std::filesystem;

namespace {

std::string current_user() {
    struct passwd* entry = ::getpwuid(::geteuid());
    if (entry != nullptr && entry->pw_name != nullptr) return entry->pw_name;
    return std::to_string(::geteuid());
}

std::string current_group() {
    struct group* entry = ::getgrgid(::getegid());
    if (entry != nullptr && entry->gr_name != nullptr) return entry->gr_name;
    return std::to_string(::getegid());
}

fs::path make_root(const char* name) {
    const fs::path root = fs::temp_directory_path() /
                          (std::string(name) + "-" + std::to_string(::getpid()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    return root;
}

ServiceRecord make_record(const std::string& name) {
    ServiceRecord record;
    record.config.name = name;
    record.config.executable = "/bin/true";
    record.config.cwd = "/";
    record.config.run_user = current_user();
    record.config.run_group = current_group();
    record.config.allow_root = true;
    return record;
}

void test_crud_and_persistence() {
    const fs::path root = make_root("edgedesk-supervisor-store-test");
    const std::string database = (root / "state" / "supervisor.db").string();

    std::uint64_t count = 0;
    {
        Error error;
        auto store = SupervisorStore::open(database, error);
        CHECK(store != nullptr);
        if (!store) return;

        CHECK(store->count(count).ok());
        CHECK_EQ(count, static_cast<std::uint64_t>(0));

        ServiceRecord record = make_record("alpha");
        error = store->insert(record);
        CHECK(error.ok());
        CHECK(!record.config.id.empty());
        CHECK(record.config.created_at > 0);

        ServiceRecord duplicate = make_record("alpha");
        error = store->insert(duplicate);
        CHECK(!error.ok());
        CHECK(error.code == ErrorCode::conflict);

        record.config.max_restarts = 9;
        record.config.name = "alpha-renamed";
        CHECK(store->update(record.config).ok());

        record.runtime.state = ServiceState::running;
        record.runtime.desired_state = "running";
        record.runtime.pid = 4242;
        record.runtime.pgid = 4242;
        record.runtime.boot_id = "boot-test";
        record.runtime.start_ticks = 123456;
        record.runtime.restart_count = 3;
        record.runtime.window_restarts = 2;
        CHECK(store->save_runtime(record.config.id, record.runtime).ok());
        CHECK(store->count(count).ok());
        CHECK_EQ(count, static_cast<std::uint64_t>(1));
    }

    {
        // Reopen to prove the rows survive a process-local close/open cycle.
        Error error;
        auto store = SupervisorStore::open(database, error);
        CHECK(store != nullptr);
        if (!store) return;
        std::vector<ServiceRecord> records;
        CHECK(store->load_all(records).ok());
        CHECK_EQ(records.size(), static_cast<std::size_t>(1));
        if (records.size() == 1) {
            CHECK_EQ(records[0].config.name, "alpha-renamed");
            CHECK_EQ(records[0].config.max_restarts, 9);
            CHECK_EQ(records[0].runtime.pid, 4242);
            CHECK_EQ(records[0].runtime.boot_id, "boot-test");
            CHECK_EQ(records[0].runtime.start_ticks, static_cast<std::uint64_t>(123456));
            CHECK_EQ(records[0].runtime.restart_count, static_cast<std::uint64_t>(3));
            CHECK_EQ(records[0].runtime.window_restarts, static_cast<std::uint64_t>(2));
        }

        ServiceRecord got;
        bool found = false;
        CHECK(store->get(records[0].config.id, got, found).ok());
        CHECK(found);
        CHECK(store->remove(records[0].config.id).ok());
        CHECK(store->get(records[0].config.id, got, found).ok());
        CHECK(!found);
    }

    struct stat info {};
    CHECK(::stat((root / "state").c_str(), &info) == 0);
    CHECK_EQ(static_cast<int>(info.st_mode & 0777), 0700);
    CHECK(::stat(database.c_str(), &info) == 0);
    CHECK_EQ(static_cast<int>(info.st_mode & 0777), 0600);

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_corrupt_database_is_not_recreated() {
    const fs::path root = make_root("edgedesk-supervisor-corrupt-test");
    const std::string database = (root / "supervisor.db").string();
    {
        std::ofstream out(database, std::ios::binary);
        out << "this is not a sqlite database and never will be";
    }
    const auto size_before = fs::file_size(database);

    Error error;
    auto store = SupervisorStore::open(database, error);
    CHECK(store == nullptr);
    CHECK(!error.ok());
    CHECK(fs::exists(database));
    CHECK_EQ(fs::file_size(database), size_before);

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_newer_schema_is_rejected() {
    const fs::path root = make_root("edgedesk-supervisor-schema-test");
    const std::string database = (root / "supervisor.db").string();
    {
        sqlite3* db = nullptr;
        CHECK_EQ(sqlite3_open(database.c_str(), &db), SQLITE_OK);
        CHECK_EQ(sqlite3_exec(db, "PRAGMA user_version=99;", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }

    Error error;
    auto store = SupervisorStore::open(database, error);
    CHECK(store == nullptr);
    CHECK(!error.ok());
    CHECK(error.message.find("newer") != std::string::npos ||
          error.message.find("version") != std::string::npos);

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_private_directory_rejects_file_component() {
    const fs::path root = make_root("edgedesk-supervisor-dir-test");
    const fs::path file = root / "not-a-directory";
    {
        std::ofstream out(file);
        out << "x";
    }
    std::string error;
    CHECK(!ensure_private_directory((file / "child").string(), error));
    CHECK(!error.empty());

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

} // namespace

int main() {
    test_crud_and_persistence();
    test_corrupt_database_is_not_recreated();
    test_newer_schema_is_rejected();
    test_private_directory_rejects_file_component();
    return edgedesk::test::finish("supervisor_store_test");
}
