#include "test_util.hpp"

#include "file/file_service.hpp"
#include "file/path_sandbox.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace edgedesk;

namespace {

namespace fs = std::filesystem;

fs::path make_root() {
    const auto root = fs::temp_directory_path() / ("edgedesk-files-test-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "docs");
    fs::create_directories(root / "docs" / "nested");
    {
        std::ofstream file(root / "a.txt");
        file << "alpha";
    }
    {
        std::ofstream file(root / "B.txt");
        file << "bravo-bravo";
    }
    {
        std::ofstream file(root / ".hidden");
        file << "hidden";
    }
    {
        std::ofstream file(root / "docs" / "note.md");
        file << "# note";
    }
    {
        std::ofstream file(root / "docs" / "nested" / "deep.txt");
        file << "deep";
    }
    {
        std::ofstream file(root / "binary.dat");
        file.write("abc\0def", 7);
    }
    return root;
}

struct Fixture {
    fs::path root;
    std::unique_ptr<PathSandbox> sandbox;
    std::unique_ptr<FileService> service;

    explicit Fixture(const fs::path& path) : root(path) {
        Error error;
        sandbox = PathSandbox::create(root.string(), false, error);
        if (!sandbox) std::fprintf(stderr, "sandbox error: %s\n", error.message.c_str());
        FileServiceOptions options;
        options.max_upload_size = 64;
        options.max_text_edit_size = 64;
        service = std::make_unique<FileService>(*sandbox, options);
    }
};

void test_list(Fixture& fixture) {
    ListOptions options;
    options.path = "/";
    ListResult result;
    Error error = fixture.service->list(options, result);
    CHECK(error.ok());
    CHECK_EQ(result.total, std::uint64_t{5});
    CHECK_EQ(result.entries.front().name, "docs"); // directories first
    CHECK(result.entries.front().is_dir);

    bool saw_hidden = false;
    for (const auto& entry : result.entries) {
        if (entry.name == ".hidden") saw_hidden = true;
    }
    CHECK(saw_hidden);

    options.show_hidden = false;
    error = fixture.service->list(options, result);
    CHECK(error.ok());
    CHECK_EQ(result.total, std::uint64_t{4});

    // Sorting by size descending.
    options.show_hidden = true;
    options.sort = "size";
    options.desc = true;
    error = fixture.service->list(options, result);
    CHECK(error.ok());
    CHECK_STR_EQ(result.entries.front().name, "docs"); // dirs stay first
    CHECK_STR_EQ(result.entries[1].name, "B.txt");

    // Paging.
    options.sort = "name";
    options.desc = false;
    options.page = 2;
    options.page_size = 2;
    error = fixture.service->list(options, result);
    CHECK(error.ok());
    CHECK_EQ(result.entries.size(), std::size_t{2});
    CHECK_EQ(result.page, 2);
    CHECK_EQ(result.total, std::uint64_t{5});

    // Listing a file is an error.
    error = fixture.service->list(ListOptions{"/a.txt", 1, 10, "name", false, true}, result);
    CHECK(!error.ok());

    // Directories only (on-demand directory-tree browsing): files are
    // filtered out, only real subdirectories remain.
    ListOptions dirs_options;
    dirs_options.path = "/";
    dirs_options.dirs_only = true;
    error = fixture.service->list(dirs_options, result);
    CHECK(error.ok());
    CHECK_EQ(result.total, std::uint64_t{1});
    CHECK_EQ(result.entries.size(), std::size_t{1});
    CHECK_STR_EQ(result.entries.front().name, "docs");
    CHECK(result.entries.front().is_dir);

    dirs_options.path = "/docs";
    error = fixture.service->list(dirs_options, result);
    CHECK(error.ok());
    CHECK_EQ(result.total, std::uint64_t{1});
    CHECK_STR_EQ(result.entries.front().name, "nested");

    // A symlink to a directory is not a real subdirectory: the tree never
    // follows links, so it must not be listed as an expandable node.
    const fs::path dir_link = fixture.root / "docs-link";
    std::error_code link_error;
    fs::create_symlink(fixture.root / "docs", dir_link, link_error);
    CHECK(!link_error);
    dirs_options.path = "/";
    error = fixture.service->list(dirs_options, result);
    CHECK(error.ok());
    CHECK_EQ(result.total, std::uint64_t{1});
    CHECK_STR_EQ(result.entries.front().name, "docs");

    // Traversal is rejected.
    error = fixture.service->list(ListOptions{"/../etc", 1, 10, "name", false, true}, result);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::invalid_argument));
}

void test_stat(Fixture& fixture) {
    FileEntry entry;
    Error error = fixture.service->stat("/a.txt", entry);
    CHECK(error.ok());
    CHECK_STR_EQ(entry.name, "a.txt");
    CHECK_EQ(entry.size, std::uint64_t{5});
    CHECK(entry.text_editable);
    CHECK_STR_EQ(entry.mime, "text/plain");

    error = fixture.service->stat("/missing", entry);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::not_found));
}

void test_mkdir_rename_delete(Fixture& fixture) {
    Error error = fixture.service->mkdir("/newdir");
    CHECK(error.ok());
    CHECK(fs::is_directory(fixture.root / "newdir"));

    error = fixture.service->mkdir("/newdir");
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::already_exists));

    error = fixture.service->mkdir("/../escape");
    CHECK(!error.ok());

    // rename: no overwrite
    error = fixture.service->rename("/a.txt", "/B.txt", false);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::already_exists));

    error = fixture.service->rename("/a.txt", "/renamed.txt", false);
    CHECK(error.ok());
    CHECK(fs::exists(fixture.root / "renamed.txt"));

    // rename with overwrite
    error = fixture.service->rename("/renamed.txt", "/B.txt", true);
    CHECK(error.ok());
    CHECK(!fs::exists(fixture.root / "renamed.txt"));

    // rename across directories
    error = fixture.service->rename("/B.txt", "/docs/B.txt", false);
    CHECK(error.ok());
    CHECK(fs::exists(fixture.root / "docs" / "B.txt"));

    // delete batch: recursive directory + single file + escape attempt
    std::vector<std::string> paths = {"/docs", "/.hidden"};
    RemoveResult removed;
    error = fixture.service->remove(paths, removed);
    CHECK(error.ok());
    CHECK(!fs::exists(fixture.root / "docs"));
    CHECK(!fs::exists(fixture.root / ".hidden"));
    CHECK_EQ(removed.removed_dirs, std::uint32_t{2}); // docs, docs/nested
    CHECK_EQ(removed.removed_files, std::uint32_t{4}); // note.md, deep.txt, B.txt, .hidden

    std::vector<std::string> escape = {"/../etc/passwd"};
    error = fixture.service->remove(escape, removed);
    CHECK(!error.ok());

    std::vector<std::string> root_delete = {"/"};
    error = fixture.service->remove(root_delete, removed);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::permission_denied));
}

void test_text_roundtrip(Fixture& fixture) {
    std::string content;
    FileEntry info;
    Error error = fixture.service->read_text("/binary.dat", content, info);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::unsupported));

    const auto path = fixture.root / "docs2";
    fs::create_directories(path);
    {
        std::ofstream file(path / "edit.txt");
        file << "first";
    }
    fs::permissions(path / "edit.txt", fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read,
                    fs::perm_options::replace);

    error = fixture.service->read_text("/docs2/edit.txt", content, info);
    CHECK(error.ok());
    CHECK_STR_EQ(content, "first");
    const auto mode_before = fs::status(path / "edit.txt").permissions();

    error = fixture.service->write_text("/docs2/edit.txt", "second\nline\n", true, info);
    CHECK(error.ok());
    CHECK_EQ(info.size, std::uint64_t{12});
    const auto mode_after = fs::status(path / "edit.txt").permissions();
    CHECK(mode_before == mode_after); // atomic save keeps permissions

    error = fixture.service->read_text("/docs2/edit.txt", content, info);
    CHECK(error.ok());
    CHECK_STR_EQ(content, "second\nline\n");

    // Oversized content is rejected.
    error = fixture.service->write_text("/docs2/edit.txt", std::string(128, 'x'), true, info);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::too_large));

    // Missing file without create flag.
    error = fixture.service->write_text("/docs2/missing.txt", "x", false, info);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::not_found));
}

void test_upload(Fixture& fixture) {
    FileService::Upload upload;
    Error error = fixture.service->begin_upload("/docs2/upload.bin", upload);
    CHECK(error.ok());
    CHECK(upload.active());
    const std::string payload = "0123456789abcdef";
    CHECK(upload.append(payload.data(), payload.size()).ok());
    FileEntry info;
    error = upload.finish(info);
    CHECK(error.ok());
    CHECK_EQ(info.size, std::uint64_t{16});
    std::ifstream file(fixture.root / "docs2" / "upload.bin", std::ios::binary);
    const std::string read((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK_STR_EQ(read, payload);

    // Aborted upload leaves no temp file behind.
    FileService::Upload aborted;
    error = fixture.service->begin_upload("/docs2/aborted.bin", aborted);
    CHECK(error.ok());
    CHECK(aborted.append("xx", 2).ok());
    aborted.abort();
    CHECK(!fs::exists(fixture.root / "docs2" / "aborted.bin"));
    for (const auto& entry : fs::directory_iterator(fixture.root / "docs2")) {
        CHECK(entry.path().filename().string().rfind(".edgedesk-upload", 0) != 0);
    }

    // Over-limit upload fails and never creates the destination.
    FileService::Upload oversized;
    error = fixture.service->begin_upload("/docs2/oversized.bin", oversized);
    CHECK(error.ok());
    std::string big(700, 'y');
    error = oversized.append(big.data(), big.size());
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::too_large));
    oversized.abort();
    CHECK(!fs::exists(fixture.root / "docs2" / "oversized.bin"));

    // Upload into a directory path is rejected.
    FileService::Upload into_dir;
    error = fixture.service->begin_upload("/docs2", into_dir);
    CHECK(!error.ok());
}

void test_open_for_read(Fixture& fixture) {
    FileService::ReadHandle handle;
    Error error = fixture.service->open_for_read("/docs2/upload.bin", handle);
    CHECK(error.ok());
    CHECK_EQ(handle.size, std::uint64_t{16});
    CHECK(handle.fd.valid());
    CHECK_STR_EQ(handle.mime, "application/octet-stream");

    error = fixture.service->open_for_read("/docs2", handle);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::is_a_directory));
}

void test_ranges() {
    ContentRange range;
    CHECK_EQ(static_cast<int>(parse_range_header("bytes=0-4", 100, range)), static_cast<int>(RangeStatus::partial));
    CHECK_EQ(range.start, std::uint64_t{0});
    CHECK_EQ(range.end, std::uint64_t{4});
    CHECK_EQ(range.total, std::uint64_t{100});

    CHECK_EQ(static_cast<int>(parse_range_header("bytes=10-", 100, range)),
             static_cast<int>(RangeStatus::partial));
    CHECK_EQ(range.end, std::uint64_t{99});

    CHECK_EQ(static_cast<int>(parse_range_header("bytes=-10", 100, range)),
             static_cast<int>(RangeStatus::partial));
    CHECK_EQ(range.start, std::uint64_t{90});
    CHECK_EQ(range.end, std::uint64_t{99});

    CHECK_EQ(static_cast<int>(parse_range_header("bytes=0-999", 100, range)),
             static_cast<int>(RangeStatus::partial));
    CHECK_EQ(range.end, std::uint64_t{99});

    CHECK_EQ(static_cast<int>(parse_range_header("bytes=-0", 100, range)),
             static_cast<int>(RangeStatus::unsatisfiable));
    CHECK_EQ(static_cast<int>(parse_range_header("bytes=100-200", 100, range)),
             static_cast<int>(RangeStatus::unsatisfiable));
    CHECK_EQ(static_cast<int>(parse_range_header("bytes=abc", 100, range)),
             static_cast<int>(RangeStatus::malformed));
    CHECK_EQ(static_cast<int>(parse_range_header("bytes=0-1,3-4", 100, range)),
             static_cast<int>(RangeStatus::malformed));
    CHECK_EQ(static_cast<int>(parse_range_header("items=0-1", 100, range)),
             static_cast<int>(RangeStatus::malformed));

    // Suffix larger than the file returns the whole file.
    CHECK_EQ(static_cast<int>(parse_range_header("bytes=-500", 100, range)),
             static_cast<int>(RangeStatus::partial));
    CHECK_EQ(range.start, std::uint64_t{0});
}

void test_mime_helpers() {
    CHECK_STR_EQ(mime_for_file_name("a.txt"), "text/plain");
    CHECK_STR_EQ(mime_for_file_name("a.sh"), "text/x-shellscript");
    CHECK_STR_EQ(mime_for_file_name("a.bin"), "application/octet-stream");
    CHECK(is_text_mime("text/plain"));
    CHECK(is_text_mime("application/json"));
    CHECK(!is_text_mime("application/octet-stream"));
    CHECK(looks_like_utf8_text("hello 中文"));
    CHECK(looks_like_utf8_text(""));
    CHECK(!looks_like_utf8_text(std::string_view("a\0b", 3)));
    CHECK(!looks_like_utf8_text("\xff\xfe"));
}

void test_protected_paths(const fs::path& root, FileService& admin) {
    fs::create_directories(root / "guard" / "nested");
    fs::create_directories(root / "guard-open");
    fs::create_directories(root / "guard-tree");
    {
        std::ofstream file(root / "guard" / "secret.txt");
        file << "top-secret";
    }
    {
        std::ofstream file(root / "guard" / "nested" / "extensions.db");
        file << "sqlite-data";
    }
    {
        std::ofstream file(root / "guard-open" / "visible.txt");
        file << "visible";
    }
    fs::create_hard_link(root / "guard" / "secret.txt", root / "guard-open" / "alias.txt");
    fs::create_hard_link(root / "guard" / "secret.txt", root / "guard-tree" / "deep-alias.txt");
    fs::create_symlink("../guard/secret.txt", root / "guard-open" / "file-link");

    const fs::path lazy_token = root / "guard-open" / "lazy-token";
    PathSandbox::Options sandbox_options;
    sandbox_options.protected_paths = {(root / "guard").string(), lazy_token.string()};

    Error error;
    auto sandbox = PathSandbox::create(root.string(), false, sandbox_options, error);
    CHECK(sandbox != nullptr);
    if (!sandbox) return;
    FileServiceOptions options;
    options.max_upload_size = 1024;
    options.max_text_edit_size = 1024;
    FileService service(*sandbox, options);

    auto denied = [](const Error& result) {
        CHECK(!result.ok());
        CHECK_EQ(static_cast<int>(result.code), static_cast<int>(ErrorCode::permission_denied));
    };

    const auto snapshot_path = root / "guard" / "secret.txt";
    const auto mtime_before = fs::last_write_time(snapshot_path);
    const auto perms_before = fs::status(snapshot_path).permissions();
    auto content_unchanged = [&]() {
        std::ifstream file(snapshot_path);
        const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        CHECK_STR_EQ(content, "top-secret");
        CHECK_EQ(fs::file_size(snapshot_path), std::uintmax_t{10});
        CHECK(fs::last_write_time(snapshot_path) == mtime_before);
        CHECK(fs::status(snapshot_path).permissions() == perms_before);
    };

    // Listing a protected directory, including path variants and nesting.
    ListOptions list;
    list.path = "/guard";
    ListResult listed;
    denied(service.list(list, listed));
    list.path = "/guard/nested";
    denied(service.list(list, listed));
    list.path = "/guard-open/../guard";
    denied(service.list(list, listed));
    list.path = "/guard-open";
    CHECK(service.list(list, listed).ok());
    for (const auto& item : listed.entries) CHECK(item.name != "alias.txt");
    ListOptions root_list;
    root_list.path = "/";
    ListResult root_entries;
    CHECK(service.list(root_list, root_entries).ok());
    for (const auto& item : root_entries.entries) CHECK(item.name != "guard");

    // stat: direct, nested, hard link, symlink target with follow enabled.
    FileEntry entry;
    denied(service.stat("/guard/secret.txt", entry));
    denied(service.stat("/guard/nested/extensions.db", entry));
    denied(service.stat("/guard-open/alias.txt", entry));
    denied(service.stat("//guard//secret.txt", entry));

    // Read: direct, variants, nested, hard link.
    std::string content;
    denied(service.read_text("/guard/secret.txt", content, entry));
    denied(service.read_text("/guard//secret.txt", content, entry));
    denied(service.read_text("/./guard/secret.txt", content, entry));
    denied(service.read_text("/guard-open/../guard/secret.txt", content, entry));
    denied(service.read_text("/guard/nested/extensions.db", content, entry));
    denied(service.read_text("/guard-open/alias.txt", content, entry));

    // Write/create/overwrite must fail before any mutation.
    denied(service.write_text("/guard/secret.txt", "tampered", true, entry));
    content_unchanged();
    denied(service.write_text("/guard/new.txt", "new", true, entry));
    CHECK(!fs::exists(root / "guard" / "new.txt"));
    denied(service.write_text("/guard-open/alias.txt", "tampered", true, entry));
    content_unchanged();

    // mkdir.
    denied(service.mkdir("/guard/newdir"));
    CHECK(!fs::exists(root / "guard" / "newdir"));
    denied(service.mkdir("/guard"));

    // rename: source protected, destination protected, hard-link alias.
    denied(service.rename("/guard/secret.txt", "/guard-open/moved.txt", false));
    CHECK(fs::exists(snapshot_path));
    denied(service.rename("/guard-open/visible.txt", "/guard/moved.txt", false));
    CHECK(fs::exists(root / "guard-open" / "visible.txt"));
    CHECK(!fs::exists(root / "guard" / "moved.txt"));
    denied(service.rename("/guard-open/alias.txt", "/guard-open/alias2.txt", false));
    CHECK(fs::exists(root / "guard-open" / "alias.txt"));
    CHECK(!fs::exists(root / "guard-open" / "alias2.txt"));

    // delete: protected file, protected directory and aliases.
    RemoveResult removed;
    denied(service.remove({"/guard/secret.txt"}, removed));
    content_unchanged();
    denied(service.remove({"/guard"}, removed));
    content_unchanged();
    denied(service.remove({"/guard-open/alias.txt"}, removed));
    CHECK(fs::exists(root / "guard-open" / "alias.txt"));
    // Recursive deletion of an ordinary directory containing a protected
    // hard-link alias also refuses before unlinking the alias.
    denied(service.remove({"/guard-tree"}, removed));
    CHECK(fs::exists(root / "guard-tree" / "deep-alias.txt"));
    content_unchanged();

    // Upload begin and download open.
    FileService::Upload upload;
    denied(service.begin_upload("/guard/secret.txt", upload));
    CHECK(!upload.active());
    denied(service.begin_upload("/guard/new-upload.bin", upload));
    CHECK(!upload.active());
    denied(service.begin_upload("/guard-open/alias.txt", upload));
    FileService::ReadHandle handle;
    denied(service.open_for_read("/guard/secret.txt", handle));
    CHECK(!handle.fd.valid());
    denied(service.open_for_read("/guard-open/alias.txt", handle));

    // Canonical guard for a credential file that did not exist when the
    // sandbox was constructed.
    denied(service.write_text("/guard-open/lazy-token", "secret", true, entry));
    CHECK(!fs::exists(lazy_token));
    denied(service.begin_upload("/guard-open/lazy-token", upload));
    CHECK(!upload.active());
    {
        std::ofstream file(lazy_token);
        file << "lazy-secret";
    }
    denied(service.read_text("/guard-open/lazy-token", content, entry));
    denied(service.stat("/guard-open/lazy-token", entry));
    denied(service.open_for_read("/guard-open/lazy-token", handle));

    // Ordinary operations still work through the protected-aware service.
    CHECK(service.read_text("/guard-open/visible.txt", content, entry).ok());
    CHECK_STR_EQ(content, "visible");
    CHECK(service.write_text("/guard-open/ordinary.txt", "ordinary", true, entry).ok());
    FileService::Upload ordinary_upload;
    CHECK(service.begin_upload("/guard-open/ordinary.bin", ordinary_upload).ok());
    CHECK(ordinary_upload.append("data", 4).ok());
    CHECK(ordinary_upload.finish(entry).ok());
    CHECK(service.mkdir("/guard-open/newdir").ok());
    CHECK(service.rename("/guard-open/visible.txt", "/guard-open/renamed.txt", false).ok());

    // The administrator file manager keeps its existing semantics.
    CHECK(admin.read_text("/guard/secret.txt", content, entry).ok());
    CHECK_STR_EQ(content, "top-secret");
    CHECK(admin.stat("/guard/secret.txt", entry).ok());
    ListOptions admin_list;
    admin_list.path = "/guard";
    ListResult admin_entries;
    CHECK(admin.list(admin_list, admin_entries).ok());

    // Symlink aliases into protected paths are rejected in follow mode. The
    // extension sandbox inherits --follow-symlinks from the server config.
    Error follow_error;
    auto follow_sandbox = PathSandbox::create(root.string(), true, sandbox_options, follow_error);
    CHECK(follow_sandbox != nullptr);
    if (follow_sandbox) {
        FileService follow_service(*follow_sandbox, options);
        FileService::ReadHandle follow_handle;
        denied(follow_service.open_for_read("/guard-open/file-link", follow_handle));
        denied(follow_service.read_text("/guard-open/file-link", content, entry));
    }
}

} // namespace

int main() {
    const auto root = make_root();
    Fixture fixture(root);
    if (!fixture.sandbox || !fixture.service) return edgedesk::test::finish("file_service_test");
    test_list(fixture);
    test_stat(fixture);
    test_mkdir_rename_delete(fixture);
    test_text_roundtrip(fixture);
    test_upload(fixture);
    test_open_for_read(fixture);
    test_ranges();
    test_mime_helpers();
    test_protected_paths(root, *fixture.service);
    fs::remove_all(root);
    return edgedesk::test::finish("file_service_test");
}
