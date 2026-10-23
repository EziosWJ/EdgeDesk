#include "test_util.hpp"

#include "supervisor/supervisor_store.hpp"
#include "supervisor/supervisor_types.hpp"

#include <cstring>
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

struct FileMetadata {
    bool valid = false;
    mode_t mode = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    time_t mtime_sec = 0;
    long mtime_nsec = 0;
};

FileMetadata capture_metadata(const fs::path& path) {
    FileMetadata metadata;
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) return metadata;
    metadata.valid = true;
    metadata.mode = static_cast<mode_t>(info.st_mode & 07777);
    metadata.uid = info.st_uid;
    metadata.gid = info.st_gid;
    metadata.mtime_sec = info.st_mtim.tv_sec;
    metadata.mtime_nsec = info.st_mtim.tv_nsec;
    return metadata;
}

// Asserts a directory still has exactly the expected permission bits and owner.
void check_permissions(const fs::path& path, mode_t expected_mode, uid_t expected_uid) {
    struct stat info {};
    const int result = ::lstat(path.c_str(), &info);
    if (result != 0) {
        std::fprintf(stderr, "cannot lstat %s: %s\n", path.c_str(), std::strerror(errno));
    }
    CHECK(result == 0);
    CHECK(S_ISDIR(info.st_mode));
    CHECK_EQ(static_cast<unsigned>(info.st_mode & 07777), static_cast<unsigned>(expected_mode));
    CHECK_EQ(static_cast<unsigned>(info.st_uid), static_cast<unsigned>(expected_uid));
}

void check_private_directory(const fs::path& path) {
    check_permissions(path, 0700, ::geteuid());
}

// Asserts mode, ownership and mtime are untouched. Used for system ancestors
// that the code under test must never modify in any way.
void expect_metadata_unchanged(const fs::path& path, const FileMetadata& before) {
    const FileMetadata after = capture_metadata(path);
    if (!before.valid || !after.valid || before.mode != after.mode || before.uid != after.uid ||
        before.gid != after.gid || before.mtime_sec != after.mtime_sec ||
        before.mtime_nsec != after.mtime_nsec) {
        std::fprintf(stderr, "metadata changed for %s\n", path.c_str());
    }
    CHECK(before.valid);
    CHECK(after.valid);
    CHECK_EQ(static_cast<unsigned>(before.mode), static_cast<unsigned>(after.mode));
    CHECK_EQ(static_cast<unsigned>(before.uid), static_cast<unsigned>(after.uid));
    CHECK_EQ(static_cast<unsigned>(before.gid), static_cast<unsigned>(after.gid));
    CHECK(before.mtime_sec == after.mtime_sec);
    CHECK(before.mtime_nsec == after.mtime_nsec);
}

void rejected(const std::string& path, std::string& error,
              const std::string& expected_fragment = std::string()) {
    error.clear();
    const bool ok = ensure_private_directory(path, error);
    CHECK(!ok);
    CHECK(!error.empty());
    if (!expected_fragment.empty()) {
        CHECK(error.find(expected_fragment) != std::string::npos);
    }
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

void test_private_directory_rejects_root_and_relative_paths() {
    const FileMetadata var_before = capture_metadata("/var");
    const FileMetadata tmp_before = capture_metadata("/tmp");
    std::string error;

    rejected("", error);
    rejected("relative/path", error);
    rejected("/", error);
    rejected("///", error);

    expect_metadata_unchanged("/var", var_before);
    expect_metadata_unchanged("/tmp", tmp_before);
}

void test_private_directory_rejects_dot_components() {
    const fs::path root = make_root("edgedesk-supervisor-dots-test");
    const FileMetadata var_before = capture_metadata("/var");
    const FileMetadata tmp_before = capture_metadata("/tmp");
    std::string error;

    rejected((root / "." / "data").string(), error);
    rejected((root / "a" / ".." / "b").string(), error);
    rejected((root / "..").string(), error);
    rejected("/var/..", error);
    rejected("/tmp/./edgedesk-psh03-nonexistent", error);

    // "." and ".." are rejected before any component is created.
    CHECK(!fs::exists(root / "a"));
    CHECK(!fs::exists(root / "data"));
    CHECK(!fs::exists("/tmp/edgedesk-psh03-nonexistent"));

    expect_metadata_unchanged("/var", var_before);
    expect_metadata_unchanged("/tmp", tmp_before);

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_private_directory_rejects_symlink_components() {
    const fs::path root = make_root("edgedesk-supervisor-symlink-test");
    const fs::path real = root / "real";
    CHECK(fs::create_directories(real));
    const fs::path link = root / "link";
    fs::create_directory_symlink(real, link);
    CHECK(fs::is_symlink(link));
    const fs::path dangling = root / "dangling";
    fs::create_symlink(root / "missing", dangling);
    CHECK(fs::is_symlink(dangling));

    const FileMetadata real_before = capture_metadata(real);
    const FileMetadata var_before = capture_metadata("/var");
    const FileMetadata tmp_before = capture_metadata("/tmp");
    std::string error;

    // Symlink as the final configured component.
    rejected(link.string(), error, "symlink");
    // Symlink as an intermediate component must not be followed.
    rejected((link / "nested").string(), error, "symlink");
    // Dangling symlink must not be replaced or followed.
    rejected(dangling.string(), error, "symlink");

    CHECK(fs::is_symlink(link));
    CHECK(fs::is_symlink(dangling));
    CHECK(!fs::exists(real / "nested"));
    CHECK(!fs::exists(root / "missing"));
    expect_metadata_unchanged(real, real_before);
    expect_metadata_unchanged("/var", var_before);
    expect_metadata_unchanged("/tmp", tmp_before);

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_private_directory_rejects_regular_files() {
    const fs::path root = make_root("edgedesk-supervisor-file-test");
    const fs::path file = root / "not-a-directory";
    {
        std::ofstream out(file);
        out << "x";
    }
    const FileMetadata file_before = capture_metadata(file);
    const FileMetadata var_before = capture_metadata("/var");
    const FileMetadata tmp_before = capture_metadata("/tmp");
    std::string error;

    // A regular file posing as the configured data directory.
    rejected(file.string(), error);
    // A regular file as an intermediate component.
    rejected((file / "child").string(), error);

    CHECK(fs::is_regular_file(file));
    expect_metadata_unchanged(file, file_before);
    expect_metadata_unchanged("/var", var_before);
    expect_metadata_unchanged("/tmp", tmp_before);

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_private_directory_creates_nested_and_reopens() {
    const fs::path root = make_root("edgedesk-supervisor-nested-test");
    CHECK(::chmod(root.c_str(), 0755) == 0);
    const FileMetadata var_before = capture_metadata("/var");
    const FileMetadata tmp_before = capture_metadata("/tmp");

    const fs::path target = root / "a" / "b" / "c";
    std::string error;
    CHECK(ensure_private_directory(target.string(), error));
    CHECK(error.empty());
    check_private_directory(root / "a");
    check_private_directory(root / "a" / "b");
    check_private_directory(target);
    // Creating children updates the parent's mtime, but the ancestor's
    // permissions and ownership must not be tightened.
    check_permissions(root, 0755, ::geteuid());

    // Reopening an existing private tree succeeds and keeps the final mode.
    error.clear();
    CHECK(ensure_private_directory(target.string(), error));
    CHECK(error.empty());
    check_private_directory(target);

    // An existing final directory with looser permissions is tightened, while
    // its parent is left alone.
    const fs::path loose = root / "loose";
    CHECK(fs::create_directories(loose));
    CHECK(::chmod(loose.c_str(), 0755) == 0);
    error.clear();
    CHECK(ensure_private_directory(loose.string(), error));
    check_private_directory(loose);
    check_permissions(root, 0755, ::geteuid());

    expect_metadata_unchanged("/var", var_before);
    expect_metadata_unchanged("/tmp", tmp_before);

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_private_directory_never_tightens_system_ancestors() {
    const FileMetadata var_before = capture_metadata("/var");
    const FileMetadata var_lib_before = capture_metadata("/var/lib");
    const fs::path candidate =
        fs::path("/var/lib") / ("edgedesk-supervisor-psh03-" + std::to_string(::getpid()));

    std::error_code ignored;
    fs::remove_all(candidate, ignored);

    std::string error;
    const bool ok = ensure_private_directory(candidate.string(), error);
    if (::geteuid() == 0) {
        // Root can create the dedicated leaf; only that leaf is tightened.
        CHECK(ok);
        if (ok) check_private_directory(candidate);
        expect_metadata_unchanged("/var", var_before);
        // Creating/removing the leaf changes /var/lib's mtime, so only its
        // permissions and ownership are compared here.
        check_permissions("/var/lib", var_lib_before.mode, var_lib_before.uid);
    } else {
        // A non-root supervisor must fail closed without modifying /var/lib.
        CHECK(!ok);
        CHECK(!error.empty());
        CHECK(!fs::exists(candidate));
        expect_metadata_unchanged("/var", var_before);
        expect_metadata_unchanged("/var/lib", var_lib_before);
    }

    fs::remove_all(candidate, ignored);
}

void test_private_directory_rejects_wrong_owner() {
    struct stat info {};
    if (::geteuid() == 0 || ::lstat("/tmp", &info) != 0 || info.st_uid == ::geteuid()) {
        std::printf(
            "supervisor_store_test: skipping wrong-owner test (needs a non-root euid and a "
            "root-owned /tmp)\n");
        return;
    }
    const FileMetadata tmp_before = capture_metadata("/tmp");
    std::string error;
    // /tmp is a directory owned by root; the helper must refuse without chmod.
    rejected("/tmp", error, "owned");
    expect_metadata_unchanged("/tmp", tmp_before);
}

} // namespace

int main() {
    test_crud_and_persistence();
    test_corrupt_database_is_not_recreated();
    test_newer_schema_is_rejected();
    test_private_directory_rejects_root_and_relative_paths();
    test_private_directory_rejects_dot_components();
    test_private_directory_rejects_symlink_components();
    test_private_directory_rejects_regular_files();
    test_private_directory_creates_nested_and_reopens();
    test_private_directory_never_tightens_system_ancestors();
    test_private_directory_rejects_wrong_owner();
    return edgedesk::test::finish("supervisor_store_test");
}
