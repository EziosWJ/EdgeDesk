#include "test_util.hpp"

#include "file/path_sandbox.hpp"

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>

using namespace edgedesk;

namespace {

namespace fs = std::filesystem;

fs::path make_root() {
    const auto root = fs::temp_directory_path() / ("edgedesk-sandbox-test-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "a" / "b");
    fs::create_directories(root / "a" / "dir" / "sub");
    fs::create_directories(root / "data2");
    {
        std::ofstream file(root / "f.txt");
        file << "hello";
    }
    {
        std::ofstream file(root / "data2" / "other.txt");
        file << "other";
    }
    {
        std::ofstream file(root / "a" / "dir" / "sub" / "file.txt");
        file << "nested";
    }
    {
        std::ofstream file(root / "a" / "b" / "target.txt");
        file << "target";
    }
    fs::create_symlink("/etc", root / "escape");
    fs::create_symlink("a", root / "into");
    fs::create_symlink("l2", root / "l1");
    fs::create_symlink("l1", root / "l2");
    // Relative targets are resolved against the directory containing the link.
    fs::create_symlink("dir", root / "a" / "alias");
    fs::create_symlink("b/target.txt", root / "a" / "file-link");
    fs::create_symlink("../..", root / "a" / "b" / "back");
    fs::create_symlink("../../../etc", root / "a" / "b" / "escape-rel");
    return root;
}

std::unique_ptr<PathSandbox> make_sandbox(const fs::path& root, bool follow, bool& ok) {
    Error error;
    auto sandbox = PathSandbox::create(root.string(), follow, error);
    ok = sandbox != nullptr;
    if (!ok) std::fprintf(stderr, "sandbox error: %s\n", error.message.c_str());
    return sandbox;
}

std::unique_ptr<PathSandbox> make_sandbox_with(const fs::path& root, bool follow,
                                               const PathSandbox::Options& options, bool& ok) {
    Error error;
    auto sandbox = PathSandbox::create(root.string(), follow, options, error);
    ok = sandbox != nullptr;
    if (!ok) std::fprintf(stderr, "sandbox error: %s\n", error.message.c_str());
    return sandbox;
}

void test_normalize() {
    std::string out;
    std::string message;
    CHECK(PathSandbox::normalize("/", out, message));
    CHECK_STR_EQ(out, "/");
    CHECK(PathSandbox::normalize("/a/b/", out, message));
    CHECK_STR_EQ(out, "/a/b");
    CHECK(PathSandbox::normalize("/a/./b", out, message));
    CHECK_STR_EQ(out, "/a/b");
    CHECK(PathSandbox::normalize("/a/../b", out, message));
    CHECK_STR_EQ(out, "/b");
    CHECK(!PathSandbox::normalize("a/b", out, message));
    CHECK(!PathSandbox::normalize("/../etc/passwd", out, message));
    CHECK(!PathSandbox::normalize("/a/../../etc/passwd", out, message));
    CHECK(!PathSandbox::normalize(std::string("/a/") + std::string(5000, 'x'), out, message));
    CHECK(!PathSandbox::normalize(std::string("/a\0b", 4), out, message));
}

void test_basics(const fs::path& root) {
    bool ok = false;
    auto sandbox = make_sandbox(root, false, ok);
    CHECK(ok);
    if (!sandbox) return;

    UniqueFd dir;
    std::string name;
    Error error = sandbox->open_parent("/f.txt", dir, name);
    CHECK(error.ok());
    CHECK(dir.valid());
    CHECK_STR_EQ(name, "f.txt");

    UniqueFd file;
    error = sandbox->open_file("/f.txt", O_RDONLY, file);
    CHECK(error.ok() && file.valid());
    if (file.valid()) {
        char buffer[16] = {0};
        const auto count = ::read(file.get(), buffer, sizeof(buffer));
        CHECK_EQ(count, 5L);
        CHECK_STR_EQ(std::string(buffer, 5), "hello");
    }

    error = sandbox->open_file("/f.txt", O_RDONLY | O_CREAT | O_EXCL, file);
    CHECK(!error.ok()); // EEXIST because the file is present
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::already_exists));

    struct stat info {};
    error = sandbox->stat("/f.txt", info);
    CHECK(error.ok() && S_ISREG(info.st_mode));
    error = sandbox->stat("/", info);
    CHECK(error.ok() && S_ISDIR(info.st_mode));
    error = sandbox->stat("/missing.txt", info);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::not_found));

    error = sandbox->open_dir("/a/b", file);
    CHECK(error.ok());
    error = sandbox->open_dir("/f.txt", file);
    CHECK(!error.ok());
}

void test_escape_attempts(const fs::path& root) {
    bool ok = false;
    auto sandbox = make_sandbox(root, false, ok);
    CHECK(ok);
    if (!sandbox) return;

    UniqueFd file;
    auto error = sandbox->open_file("/../etc/passwd", O_RDONLY, file);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::invalid_argument));

    error = sandbox->open_file("/a/../../etc/passwd", O_RDONLY, file);
    CHECK(!error.ok());

    // Symlink that leaves the root: blocked outright (default policy).
    error = sandbox->open_file("/escape/shadow", O_RDONLY, file);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::permission_denied));

    error = sandbox->open_dir("/escape", file);
    CHECK(!error.ok());

    struct stat info {};
    error = sandbox->stat("/escape", info);
    CHECK(error.ok());
    CHECK(S_ISLNK(info.st_mode)); // stat() reports the link itself

    // Mid-path symlink to an inside directory is also blocked by default.
    error = sandbox->open_dir("/into/b", file);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::permission_denied));
}

void test_follow_symlinks(const fs::path& root) {
    bool ok = false;
    auto sandbox = make_sandbox(root, true, ok);
    CHECK(ok);
    if (!sandbox) return;

    UniqueFd file;
    // Inside symlink resolves.
    auto error = sandbox->open_dir("/into/b", file);
    CHECK(error.ok());

    // Escape symlink is re-resolved *inside* the sandbox and therefore missing,
    // never reaching the real /etc.
    error = sandbox->open_file("/escape/shadow", O_RDONLY, file);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::not_found));

    // Symlink loop is detected.
    error = sandbox->open_file("/l1/x", O_RDONLY, file);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::permission_denied));

    // Relative directory targets resolve against the link's own directory.
    // These links are intermediate components, so they exercise PathSandbox::walk.
    error = sandbox->open_file("/a/alias/sub/file.txt", O_RDONLY, file);
    CHECK(error.ok());
    if (error.ok()) {
        char buffer[16] = {0};
        const auto count = ::read(file.get(), buffer, sizeof(buffer));
        CHECK_EQ(count, 6L);
        CHECK_STR_EQ(std::string(buffer, 6), "nested");
    }

    // A relative directory target that climbs out of its directory but stays
    // inside the sandbox root ("/a/b" + "../.." => "/") must resolve.
    error = sandbox->open_file("/a/b/back/f.txt", O_RDONLY, file);
    CHECK(error.ok());
    if (error.ok()) {
        char buffer[16] = {0};
        const auto count = ::read(file.get(), buffer, sizeof(buffer));
        CHECK_EQ(count, 5L);
        CHECK_STR_EQ(std::string(buffer, 5), "hello");
    }

    // Relative final-component targets keep working as well.
    error = sandbox->open_file("/a/file-link", O_RDONLY, file);
    CHECK(error.ok());

    // A relative target that actually escapes the root is still rejected.
    error = sandbox->open_file("/a/b/escape-rel/passwd", O_RDONLY, file);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::permission_denied));
}

void test_prefix_collision(const fs::path& root) {
    bool ok = false;
    auto sandbox = make_sandbox(root, false, ok);
    CHECK(ok);
    if (!sandbox) return;
    UniqueFd file;
    // /data2 lives outside the "data" component named in the request; the
    // FD-based walk never confuses prefixes.
    auto error = sandbox->open_file("/data/other.txt", O_RDONLY, file);
    CHECK(!error.ok());
    error = sandbox->open_file("/data2/other.txt", O_RDONLY, file);
    CHECK(error.ok());
}

void test_protected_paths(const fs::path& root) {
    fs::create_directories(root / "guard" / "nested");
    fs::create_directories(root / "guard-open");
    {
        std::ofstream file(root / "guard" / "secret.txt");
        file << "top-secret";
    }
    {
        std::ofstream file(root / "guard" / "nested" / "deep.txt");
        file << "deep-secret";
    }
    {
        std::ofstream file(root / "guard-open" / "visible.txt");
        file << "visible";
    }
    fs::create_hard_link(root / "guard" / "secret.txt", root / "guard-open" / "alias.txt");
    fs::create_symlink("../guard/secret.txt", root / "guard-open" / "file-link");
    fs::create_symlink("../guard", root / "guard-open" / "dir-link");
    fs::create_symlink("visible.txt", root / "guard-open" / "visible-link");

    // The token file does not exist yet at sandbox construction. The canonical
    // guard must still block creating and later reading it.
    const fs::path lazy_token = root / "guard-open" / "lazy-token";
    PathSandbox::Options options;
    options.protected_paths = {(root / "guard").string(), lazy_token.string()};

    auto denied = [](const Error& error) {
        CHECK(!error.ok());
        CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::permission_denied));
    };

    bool ok = false;
    auto sandbox = make_sandbox_with(root, false, options, ok);
    CHECK(ok);
    if (!sandbox) return;

    UniqueFd fd;
    denied(sandbox->open_dir("/guard", fd));
    denied(sandbox->open_file("/guard/secret.txt", O_RDONLY, fd));
    denied(sandbox->open_file("/guard/nested/deep.txt", O_RDONLY, fd));
    denied(sandbox->open_file("//guard//secret.txt", O_RDONLY, fd));
    denied(sandbox->open_file("/./guard/secret.txt", O_RDONLY, fd));
    denied(sandbox->open_file("/guard-open/../guard/secret.txt", O_RDONLY, fd));
    denied(sandbox->open_file("/guard-open/alias.txt", O_RDONLY, fd)); // hard link

    struct stat info {};
    denied(sandbox->stat("/guard", info));
    denied(sandbox->stat("/guard/secret.txt", info));
    denied(sandbox->stat("/guard/nested/deep.txt", info));
    denied(sandbox->stat("/guard-open/alias.txt", info));

    // On-disk content must be untouched by all refused attempts.
    {
        std::ifstream file(root / "guard" / "secret.txt");
        const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        CHECK_STR_EQ(content, "top-secret");
    }

    denied(sandbox->open_file("/guard-open/lazy-token", O_WRONLY | O_CREAT | O_EXCL, fd, 0600));
    CHECK(!fs::exists(lazy_token));
    {
        std::ofstream file(lazy_token);
        file << "lazy-secret";
    }
    denied(sandbox->open_file("/guard-open/lazy-token", O_RDONLY, fd));
    denied(sandbox->stat("/guard-open/lazy-token", info));

    // Ordinary paths remain accessible.
    Error error = sandbox->open_file("/guard-open/visible.txt", O_RDONLY, fd);
    CHECK(error.ok());
    if (error.ok()) {
        char buffer[16] = {0};
        const auto count = ::read(fd.get(), buffer, sizeof(buffer));
        CHECK_EQ(count, 7L);
        CHECK_STR_EQ(std::string(buffer, 7), "visible");
    }
    error = sandbox->open_dir("/guard-open", fd);
    CHECK(error.ok());

    // With symlink following enabled, both final and intermediate symlinks to
    // protected entries are rejected; unrelated links still resolve.
    auto followed = make_sandbox_with(root, true, options, ok);
    CHECK(ok);
    if (followed) {
        denied(followed->open_file("/guard-open/file-link", O_RDONLY, fd));
        denied(followed->open_dir("/guard-open/dir-link", fd));
        denied(followed->open_file("/guard-open/dir-link/secret.txt", O_RDONLY, fd));
        error = followed->open_file("/guard-open/visible-link", O_RDONLY, fd);
        CHECK(error.ok());
    }
}

} // namespace

int main() {
    const auto root = make_root();
    test_normalize();
    test_basics(root);
    test_escape_attempts(root);
    test_follow_symlinks(root);
    test_prefix_collision(root);
    test_protected_paths(root);
    fs::remove_all(root);
    return edgedesk::test::finish("path_sandbox_test");
}
