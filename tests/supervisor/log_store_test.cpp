#include "test_util.hpp"

#include "supervisor/log_store.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace edgedesk;
using namespace edgedesk::supervisor;
namespace fs = std::filesystem;

namespace {

fs::path make_root(const char* name) {
    const fs::path root = fs::temp_directory_path() /
                          (std::string(name) + "-" + std::to_string(::getpid()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    return root;
}

void test_append_tail_and_cursor() {
    const fs::path root = make_root("edgedesk-log-store-test");
    Error error;
    auto logs = LogStore::open(root.string(), error);
    CHECK(logs != nullptr);
    if (!logs) return;

    CHECK(logs->append("serviceA", "hello\n", 4096).ok());
    CHECK(logs->append("serviceA", "world\n", 4096).ok());

    LogReadResult read;
    CHECK(logs->read("serviceA", 0, false, 0, 0, read).ok());
    CHECK_EQ(read.content, "hello\nworld\n");
    CHECK_EQ(read.size, static_cast<std::uint64_t>(12));
    CHECK_EQ(read.next_cursor, static_cast<std::uint64_t>(12));
    CHECK(!read.reset);

    CHECK(logs->append("serviceA", "third\n", 4096).ok());
    CHECK(logs->read("serviceA", 6, false, 0, 0, read).ok());
    CHECK_EQ(read.content, "third\n");
    CHECK_EQ(read.cursor, static_cast<std::uint64_t>(12));

    CHECK(logs->read("serviceA", 0, true, 12, 0, read).ok());
    CHECK_EQ(read.content, "third\n");
    CHECK_EQ(read.next_cursor, static_cast<std::uint64_t>(18));

    // Reading at the current position yields an empty delta, not the old text.
    CHECK(logs->read("serviceA", 0, true, 18, 0, read).ok());
    CHECK(read.content.empty());
    CHECK_EQ(read.next_cursor, static_cast<std::uint64_t>(18));

    struct stat info {};
    CHECK(::stat((root / "serviceA.log").c_str(), &info) == 0);
    CHECK_EQ(static_cast<int>(info.st_mode & 0777), 0600);

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_single_chunk_is_bounded() {
    const fs::path root = make_root("edgedesk-log-bounded-test");
    Error error;
    auto logs = LogStore::open(root.string(), error);
    CHECK(logs != nullptr);
    if (!logs) return;

    const std::string large(10000, 'x');
    CHECK(logs->append("bounded", large, 4096).ok());
    LogReadResult read;
    CHECK(logs->read("bounded", 0, false, 0, 0, read).ok());
    CHECK_EQ(read.size, static_cast<std::uint64_t>(4096));
    CHECK_EQ(read.content.size(), static_cast<std::size_t>(4096));

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_rotation_and_reset() {
    const fs::path root = make_root("edgedesk-log-rotation-test");
    Error error;
    auto logs = LogStore::open(root.string(), error);
    CHECK(logs != nullptr);
    if (!logs) return;

    const std::string chunk(3000, 'a');
    CHECK(logs->append("rotator", chunk, 4096).ok());
    CHECK(logs->append("rotator", chunk, 4096).ok());
    CHECK(fs::exists(root / "rotator.log.1"));

    LogReadResult read;
    // A cursor beyond the new file size signals that the generation changed.
    CHECK(logs->read("rotator", 0, true, 6000, 0, read).ok());
    CHECK(read.reset);
    CHECK_EQ(read.size, static_cast<std::uint64_t>(3000));

    CHECK(logs->remove("rotator").ok());
    CHECK(!fs::exists(root / "rotator.log"));
    CHECK(!fs::exists(root / "rotator.log.1"));
    CHECK(logs->read("rotator", 0, false, 0, 0, read).ok());
    CHECK(read.content.empty());

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_invalid_id_and_symlink_rejected() {
    const fs::path root = make_root("edgedesk-log-symlink-test");
    Error error;
    auto logs = LogStore::open(root.string(), error);
    CHECK(logs != nullptr);
    if (!logs) return;

    CHECK(!logs->append("../escape", "x", 4096).ok());
    LogReadResult invalid_read;
    CHECK(!logs->read("a/b", 0, false, 0, 0, invalid_read).ok());

    // O_NOFOLLOW must prevent writing through a planted symlink.
    const fs::path outside = root.parent_path() / ("edgedesk-log-outside-" + std::to_string(::getpid()));
    {
        std::ofstream out(outside);
        out << "do not touch";
    }
    const auto outside_size = fs::file_size(outside);
    std::error_code ignored;
    fs::create_symlink(outside, root / "linked.log", ignored);
    const Error append_error = logs->append("linked", "attack", 4096);
    CHECK(!append_error.ok());
    CHECK_EQ(fs::file_size(outside), outside_size);

    fs::remove(outside, ignored);
    fs::remove_all(root, ignored);
}

} // namespace

int main() {
    test_append_tail_and_cursor();
    test_single_chunk_is_bounded();
    test_rotation_and_reset();
    test_invalid_id_and_symlink_rejected();
    return edgedesk::test::finish("supervisor_log_store_test");
}
