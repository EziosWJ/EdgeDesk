#include "test_util.hpp"

#include "file/file_lock.hpp"
#include "file/file_service.hpp"
#include "file/path_sandbox.hpp"

#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace edgedesk;

namespace {

namespace fs = std::filesystem;

fs::path make_root() {
    const auto root = fs::temp_directory_path() / ("edgedesk-lock-test-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "dir");
    {
        std::ofstream file(root / "a.txt");
        file << "alpha";
    }
    fs::create_hard_link(root / "a.txt", root / "alias.txt");
    return root;
}

void test_manager(const fs::path& root) {
    Error error;
    auto sandbox = PathSandbox::create(root.string(), false, error);
    CHECK(sandbox != nullptr);
    if (!sandbox) return;
    FileLockManager locks;
    std::uint64_t shared1 = 0;
    std::uint64_t shared2 = 0;
    CHECK(locks.acquire_shared(*sandbox, "/a.txt", shared1).ok());
    CHECK(locks.acquire_shared(*sandbox, "/alias.txt", shared2).ok()); // same inode
    CHECK_EQ(locks.active_locks(), std::size_t{2});
    std::uint64_t exclusive = 0;
    Error busy = locks.acquire_exclusive(*sandbox, "/a.txt", exclusive);
    CHECK(!busy.ok());
    CHECK(busy.code == ErrorCode::busy);
    locks.release(shared1);
    CHECK(!locks.acquire_exclusive(*sandbox, "/a.txt", exclusive).ok());
    locks.release(shared2);
    CHECK(locks.acquire_exclusive(*sandbox, "/a.txt", exclusive).ok());
    CHECK(!locks.acquire_shared(*sandbox, "/alias.txt", shared1).ok());
    locks.release(exclusive);
    CHECK_EQ(locks.active_locks(), std::size_t{0});

    // Missing target uses the parent-directory identity plus the name.
    std::uint64_t missing = 0;
    CHECK(locks.acquire_exclusive(*sandbox, "/dir/new.txt", missing).ok());
    std::uint64_t same_parent_other_name = 0;
    CHECK(locks.acquire_exclusive(*sandbox, "/dir/other.txt", same_parent_other_name).ok());
    std::uint64_t same_missing = 0;
    CHECK(!locks.acquire_exclusive(*sandbox, "/dir/new.txt", same_missing).ok());
    locks.release(missing);
    locks.release(same_parent_other_name);

    // Multi-lock failure releases everything acquired so far.
    std::vector<std::uint64_t> tokens;
    CHECK(locks.acquire_exclusive(*sandbox, "/dir/new.txt", missing).ok());
    Error many = locks.acquire_exclusive_many(*sandbox, {"/a.txt", "/dir/new.txt"}, tokens);
    CHECK(!many.ok());
    CHECK(tokens.empty());
    CHECK_EQ(locks.active_locks(), std::size_t{1});
    locks.release(missing);
    CHECK(locks.acquire_exclusive_many(*sandbox, {"/a.txt", "/dir/new.txt"}, tokens).ok());
    CHECK_EQ(tokens.size(), std::size_t{2});
    for (const auto token : tokens) locks.release(token);
    CHECK_EQ(locks.active_locks(), std::size_t{0});
}

void test_file_service_integration(const fs::path& root) {
    Error error;
    auto sandbox = PathSandbox::create(root.string(), false, error);
    CHECK(sandbox != nullptr);
    if (!sandbox) return;
    FileLockManager locks;
    FileService files(*sandbox);
    files.set_lock_manager(&locks);
    FileService::ReadHandle first;
    FileService::ReadHandle second;
    CHECK(files.open_for_read("/a.txt", first).ok());
    CHECK(files.open_for_read("/alias.txt", second).ok()); // shared readers may alias
    CHECK_EQ(locks.active_locks(), std::size_t{2});
    FileService::Upload upload;
    Error busy = files.begin_upload("/a.txt", upload);
    CHECK(!busy.ok());
    CHECK(busy.code == ErrorCode::busy);
    second = FileService::ReadHandle();
    CHECK_EQ(locks.active_locks(), std::size_t{1});
    CHECK(!files.begin_upload("/a.txt", upload).ok());
    first = FileService::ReadHandle();
    CHECK_EQ(locks.active_locks(), std::size_t{0});
    CHECK(files.begin_upload("/a.txt", upload).ok());
    CHECK_EQ(locks.active_locks(), std::size_t{1});
    FileService::ReadHandle blocked;
    CHECK(!files.open_for_read("/a.txt", blocked).ok());
    upload.abort();
    CHECK_EQ(locks.active_locks(), std::size_t{0});
    CHECK(files.open_for_read("/a.txt", blocked).ok());
    blocked = FileService::ReadHandle();
    CHECK_EQ(locks.active_locks(), std::size_t{0});

    // Moving an active upload must transfer the lock rather than releasing it
    // from the moved-from destructor.
    FileService::Upload first_upload;
    CHECK(files.begin_upload("/a.txt", first_upload).ok());
    FileService::Upload moved_upload = std::move(first_upload);
    CHECK_EQ(locks.active_locks(), std::size_t{1});
    FileService::Upload blocked_upload;
    CHECK(!files.begin_upload("/a.txt", blocked_upload).ok());
    moved_upload.abort();
    CHECK_EQ(locks.active_locks(), std::size_t{0});

    // External modifications between begin and commit abort the upload.
    FileService::Upload external_upload;
    CHECK(files.begin_upload("/a.txt", external_upload).ok());
    CHECK(external_upload.append("replacement", 11).ok());
    {
        std::ofstream external(root / "a.txt", std::ios::trunc);
        external << "outside change";
    }
    FileEntry info;
    Error conflict = external_upload.finish(info);
    CHECK(!conflict.ok());
    CHECK(conflict.code == ErrorCode::conflict);
    CHECK_EQ(locks.active_locks(), std::size_t{0});
    {
        std::ifstream original(root / "a.txt");
        std::string content((std::istreambuf_iterator<char>(original)), std::istreambuf_iterator<char>());
        CHECK_STR_EQ(content, "outside change");
    }

    // A target created externally during a new-file upload also conflicts.
    FileService::Upload created_upload;
    CHECK(files.begin_upload("/created.txt", created_upload).ok());
    CHECK(created_upload.append("data", 4).ok());
    {
        std::ofstream external(root / "created.txt", std::ios::trunc);
        external << "appeared";
    }
    CHECK(!created_upload.finish(info).ok());
    CHECK_EQ(locks.active_locks(), std::size_t{0});
}

} // namespace

int main() {
    const auto root = make_root();
    test_manager(root);
    test_file_service_integration(root);
    std::filesystem::remove_all(root);
    return edgedesk::test::finish("file_lock_test");
}
