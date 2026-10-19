#include "test_util.hpp"

#include "extensions/package_manager.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

// REL-02 / T-05: package installation and static asset serving must not scale
// memory with the size of extracted content.
//
// The parent builds the ZIP files and then execs this binary for each measured
// operation, so VmHWM (peak RSS) starts from a clean baseline instead of the
// parent's ZIP-building allocations. Both operations run in a child and report
// baseline/peak VmHWM; the parent asserts a generous bound.
//
// A second scenario injects a write failure with RLIMIT_FSIZE (SIGXFSZ
// ignored) to exercise interrupted extraction and verify cleanup. True ENOSPC
// cannot be injected portably; EFBIG follows the same write-failure path.

namespace {

namespace fs = std::filesystem;

using namespace edgedesk;
using namespace edgedesk::extensions;

constexpr std::uint64_t kLargeAssetSize = 64ull * 1024 * 1024;
constexpr std::uint64_t kFailureEntrySize = 4ull * 1024 * 1024;
constexpr std::uint64_t kPeakDeltaLimitKb = 24ull * 1024; // generous bound
constexpr const char* kAppId = "io.example.memory";
constexpr const char* kVersion = "1.0.0";

std::uint64_t peak_rss_kb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            unsigned long long value = 0;
            if (std::sscanf(line.c_str(), "VmHWM: %llu", &value) == 1) {
                return static_cast<std::uint64_t>(value);
            }
        }
    }
    return 0;
}

const char* kIndexHtml = "<!doctype html><title>memory</title>";

std::string manifest_text() {
    return std::string("{\"id\":\"") + kAppId + "\",\"name\":\"Memory Test\",\"version\":\"" + kVersion +
           "\",\"entry\":\"index.html\",\"minHostApiVersion\":\"1.0.0\",\"maxHostApiVersion\":\"1.9.9\","
           "\"permissions\":[],\"window\":{\"defaultWidth\":800,\"defaultHeight\":600,\"minWidth\":320,"
           "\"minHeight\":240,\"instances\":\"single\"}}";
}

void put_u16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void put_u32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

struct CentralEntry {
    std::string name;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint64_t compressed = 0;
    std::uint64_t uncompressed = 0;
    std::uint64_t offset = 0;
};

std::string local_header(const std::string& name, std::uint16_t method, std::uint32_t crc,
                         std::uint64_t compressed, std::uint64_t uncompressed) {
    std::string out;
    put_u32(out, 0x04034b50u);
    put_u16(out, 20);
    put_u16(out, 0x0800); // UTF-8 names
    put_u16(out, method);
    put_u16(out, 0);
    put_u16(out, 0);
    put_u32(out, crc);
    put_u32(out, static_cast<std::uint32_t>(compressed));
    put_u32(out, static_cast<std::uint32_t>(uncompressed));
    put_u16(out, static_cast<std::uint16_t>(name.size()));
    put_u16(out, 0);
    out += name;
    return out;
}

// Streams a stored or raw-deflated entry without ever materializing the whole
// content. The fill pattern is deterministic and highly compressible.
bool stream_entry(std::ofstream& out, const std::string& name, std::uint64_t size, bool deflate_entry,
                  std::uint32_t& crc_out, std::uint64_t& compressed_out, std::uint64_t& offset_out) {
    std::string pattern = "EdgeDesk memory test pattern 0123456789\n";
    std::vector<char> input(64 * 1024);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = pattern[i % pattern.size()];
    }

    offset_out = static_cast<std::uint64_t>(out.tellp());
    // Header sizes are patched below for deflate, known up front for stored.
    out << local_header(name, deflate_entry ? 8 : 0, 0, 0, 0);
    if (!out) return false;

    std::uint32_t crc = static_cast<std::uint32_t>(crc32(0L, Z_NULL, 0));
    std::uint64_t compressed = 0;
    std::uint64_t remaining = size;
    if (!deflate_entry) {
        while (remaining > 0) {
            const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, input.size()));
            crc = static_cast<std::uint32_t>(crc32(crc, reinterpret_cast<const Bytef*>(input.data()),
                                                  static_cast<uInt>(chunk)));
            out.write(input.data(), static_cast<std::streamsize>(chunk));
            if (!out) return false;
            compressed += chunk;
            remaining -= chunk;
        }
    } else {
        z_stream stream{};
        if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            return false;
        }
        std::vector<char> output(64 * 1024);
        auto flush = [&](int mode) -> bool {
            for (;;) {
                stream.next_out = reinterpret_cast<Bytef*>(output.data());
                stream.avail_out = static_cast<uInt>(output.size());
                const int result = deflate(&stream, mode);
                if (result != Z_OK && result != Z_STREAM_END) return false;
                const std::size_t made = output.size() - stream.avail_out;
                if (made > 0) {
                    out.write(output.data(), static_cast<std::streamsize>(made));
                    if (!out) return false;
                    compressed += made;
                }
                if (result == Z_STREAM_END) return true;
                if (made == 0) return false;
            }
        };
        while (remaining > 0) {
            const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, input.size()));
            crc = static_cast<std::uint32_t>(crc32(crc, reinterpret_cast<const Bytef*>(input.data()),
                                                  static_cast<uInt>(chunk)));
            stream.next_in = reinterpret_cast<Bytef*>(input.data());
            stream.avail_in = static_cast<uInt>(chunk);
            while (stream.avail_in > 0) {
                stream.next_out = reinterpret_cast<Bytef*>(output.data());
                stream.avail_out = static_cast<uInt>(output.size());
                const int result = deflate(&stream, Z_NO_FLUSH);
                if (result != Z_OK) {
                    deflateEnd(&stream);
                    return false;
                }
                const std::size_t made = output.size() - stream.avail_out;
                if (made > 0) {
                    out.write(output.data(), static_cast<std::streamsize>(made));
                    if (!out) {
                        deflateEnd(&stream);
                        return false;
                    }
                    compressed += made;
                }
            }
            remaining -= chunk;
        }
        stream.next_in = Z_NULL;
        stream.avail_in = 0;
        const bool finished = flush(Z_FINISH);
        deflateEnd(&stream);
        if (!finished) return false;
    }

    if (!deflate_entry) compressed = size;
    const auto end = out.tellp();
    out.seekp(static_cast<std::streamoff>(offset_out) + 14);
    std::string patch;
    put_u32(patch, crc);
    put_u32(patch, static_cast<std::uint32_t>(compressed));
    put_u32(patch, static_cast<std::uint32_t>(size));
    out.write(patch.data(), static_cast<std::streamsize>(patch.size()));
    out.seekp(end);
    if (!out) return false;
    crc_out = crc;
    compressed_out = compressed;
    return true;
}

bool write_package_zip(const std::string& path, std::uint64_t large_size, bool deflate_large,
                       bool include_large) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    std::vector<CentralEntry> central;

    auto add_small = [&](const std::string& name, const std::string& data) -> bool {
        CentralEntry entry;
        entry.name = name;
        entry.method = 0;
        entry.crc = static_cast<std::uint32_t>(
            crc32(0L, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size())));
        entry.compressed = data.size();
        entry.uncompressed = data.size();
        entry.offset = static_cast<std::uint64_t>(out.tellp());
        out << local_header(name, 0, entry.crc, entry.compressed, entry.uncompressed);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
        central.push_back(std::move(entry));
        return true;
    };

    if (!add_small("manifest.json", manifest_text())) return false;
    if (!add_small("index.html", kIndexHtml)) return false;
    if (include_large) {
        CentralEntry entry;
        entry.name = "assets/large.bin";
        entry.method = deflate_large ? 8 : 0;
        if (!stream_entry(out, entry.name, large_size, deflate_large, entry.crc, entry.compressed,
                          entry.offset)) {
            return false;
        }
        entry.uncompressed = large_size;
        central.push_back(std::move(entry));
    }

    const std::uint64_t directory_offset = static_cast<std::uint64_t>(out.tellp());
    std::string directory;
    for (const auto& entry : central) {
        put_u32(directory, 0x02014b50u);
        put_u16(directory, 0x031e); // made by Unix
        put_u16(directory, 20);
        put_u16(directory, 0x0800);
        put_u16(directory, entry.method);
        put_u16(directory, 0);
        put_u16(directory, 0);
        put_u32(directory, entry.crc);
        put_u32(directory, static_cast<std::uint32_t>(entry.compressed));
        put_u32(directory, static_cast<std::uint32_t>(entry.uncompressed));
        put_u16(directory, static_cast<std::uint16_t>(entry.name.size()));
        put_u16(directory, 0);
        put_u16(directory, 0);
        put_u16(directory, 0);
        put_u16(directory, 0);
        put_u32(directory, 0);
        put_u32(directory, static_cast<std::uint32_t>(entry.offset));
        directory += entry.name;
    }
    out.write(directory.data(), static_cast<std::streamsize>(directory.size()));
    std::string eocd;
    put_u32(eocd, 0x06054b50u);
    put_u16(eocd, 0);
    put_u16(eocd, 0);
    put_u16(eocd, static_cast<std::uint16_t>(central.size()));
    put_u16(eocd, static_cast<std::uint16_t>(central.size()));
    put_u32(eocd, static_cast<std::uint32_t>(directory.size()));
    put_u32(eocd, static_cast<std::uint32_t>(directory_offset));
    put_u16(eocd, 0);
    out.write(eocd.data(), static_cast<std::streamsize>(eocd.size()));
    return static_cast<bool>(out);
}

struct ChildResult {
    int exit_code = -1;
    std::string output;
};

// argv[0] of the test process. Under qemu-user /proc/self/exe resolves to the
// host emulator, so re-exec must name the guest binary explicitly.
std::string self_executable_path;

// Cross-compiled builds run this test through CMAKE_CROSSCOMPILING_EMULATOR.
// The emulator only wraps the top-level process, so a bare exec of
// /proc/self/exe fails (ENOEXEC/127) on ARM64 hosts without binfmt. CMake
// passes the emulator command in EDGEDESK_TEST_EXEC_PREFIX; when unset the
// child is executed directly as on a native host.
std::vector<std::string> child_exec_prefix() {
    std::vector<std::string> tokens;
    const char* raw = std::getenv("EDGEDESK_TEST_EXEC_PREFIX");
    if (raw == nullptr) return tokens;
    std::string token;
    for (const char* cursor = raw; *cursor != '\0'; ++cursor) {
        if (*cursor == ' ') {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token.push_back(*cursor);
        }
    }
    if (!token.empty()) tokens.push_back(token);
    return tokens;
}

ChildResult run_child(const std::vector<std::string>& arguments) {
    ChildResult result;
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) return result;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return result;
    }
    if (pid == 0) {
        ::close(pipe_fds[0]);
        if (::dup2(pipe_fds[1], STDOUT_FILENO) < 0) _exit(127);
        ::close(pipe_fds[1]);
        std::vector<std::string> child_arguments = child_exec_prefix();
        child_arguments.push_back(self_executable_path.empty() ? "/proc/self/exe"
                                                               : self_executable_path);
        child_arguments.insert(child_arguments.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        argv.reserve(child_arguments.size() + 1);
        for (const auto& argument : child_arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(argv.front(), argv.data());
        _exit(127);
    }
    ::close(pipe_fds[1]);
    char buffer[4096];
    for (;;) {
        const auto read = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (read < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (read == 0) break;
        result.output.append(buffer, static_cast<std::size_t>(read));
    }
    ::close(pipe_fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

std::string field(const std::string& output, const std::string& key) {
    const std::string prefix = key + "=";
    const auto start = output.find(prefix);
    if (start == std::string::npos) return {};
    const auto value_start = start + prefix.size();
    const auto end = output.find_first_of(" \n", value_start);
    return output.substr(value_start, end == std::string::npos ? std::string::npos : end - value_start);
}

int install_child(const std::string& zip_path, const std::string& root) {
    Error error;
    auto manager = PackageManager::create(root, error);
    if (!manager) {
        std::fprintf(stderr, "manager: %s\n", error.message.c_str());
        return 1;
    }
    const std::uint64_t baseline = peak_rss_kb();
    InstallResult result;
    error = manager->install(zip_path, {}, result);
    const std::uint64_t peak = peak_rss_kb();
    if (!error.ok()) {
        std::fprintf(stderr, "install: %s\n", error.message.c_str());
        return 1;
    }
    AppRecord app;
    error = manager->approve(kAppId, {}, true, app);
    if (!error.ok()) {
        std::fprintf(stderr, "approve: %s\n", error.message.c_str());
        return 1;
    }
    std::printf("baseline_kb=%llu peak_kb=%llu revision=%s file_count=%llu total_bytes=%llu\n",
                static_cast<unsigned long long>(baseline), static_cast<unsigned long long>(peak),
                result.revision.revision_id.c_str(),
                static_cast<unsigned long long>(result.revision.file_count),
                static_cast<unsigned long long>(result.revision.total_bytes));
    return 0;
}

int serve_child(const std::string& root, const std::string& revision_id) {
    Error error;
    auto manager = PackageManager::create(root, error);
    if (!manager) {
        std::fprintf(stderr, "manager: %s\n", error.message.c_str());
        return 1;
    }
    const std::uint64_t baseline = peak_rss_kb();
    StaticAssetHandle asset;
    error = manager->open_static(kAppId, kVersion, revision_id, "assets/large.bin", asset);
    if (!error.ok()) {
        std::fprintf(stderr, "open_static: %s\n", error.message.c_str());
        return 1;
    }
    std::vector<char> buffer(128 * 1024);
    std::uint64_t offset = 0;
    while (offset < asset.size) {
        const std::size_t wanted =
            static_cast<std::size_t>(std::min<std::uint64_t>(asset.size - offset, buffer.size()));
        const auto read = ::pread(asset.fd.get(), buffer.data(), wanted, static_cast<off_t>(offset));
        if (read < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "pread: %s\n", std::strerror(errno));
            return 1;
        }
        if (read == 0) break;
        offset += static_cast<std::uint64_t>(read);
    }
    const std::uint64_t peak = peak_rss_kb();
    std::printf("baseline_kb=%llu peak_kb=%llu served=%llu size=%llu etag=%s\n",
                static_cast<unsigned long long>(baseline), static_cast<unsigned long long>(peak),
                static_cast<unsigned long long>(offset), static_cast<unsigned long long>(asset.size),
                asset.etag.c_str());
    return offset == asset.size ? 0 : 1;
}

int failure_child(const std::string& zip_path, const std::string& root, std::uint64_t file_limit) {
    Error error;
    auto manager = PackageManager::create(root, error);
    if (!manager) {
        std::fprintf(stderr, "manager: %s\n", error.message.c_str());
        return 1;
    }
    std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit limit {};
    limit.rlim_cur = static_cast<rlim_t>(file_limit);
    limit.rlim_max = static_cast<rlim_t>(file_limit);
    if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
        std::fprintf(stderr, "setrlimit: %s\n", std::strerror(errno));
        return 1;
    }
    InstallResult result;
    error = manager->install(zip_path, {}, result);
    if (error.ok()) {
        std::fprintf(stderr, "install unexpectedly succeeded with a file size limit\n");
        return 1;
    }
    std::printf("failed_as_expected=1 code=%d\n", static_cast<int>(error.code));
    return 0;
}

int run_parent() {
    const fs::path base =
        fs::temp_directory_path() / ("edgedesk-memory-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create test directory: %s\n", ec.message().c_str());
        return 1;
    }
    const std::string apps_root = (base / "apps").string();
    const std::string zip_path = (base / "app.zip").string();
    CHECK(write_package_zip(zip_path, kLargeAssetSize, true, true));

    const ChildResult install = run_child({"--install-child", zip_path, apps_root});
    if (install.exit_code != 0) {
        std::fprintf(stderr, "install child output: %s\n", install.output.c_str());
    }
    CHECK_EQ(install.exit_code, 0);
    const std::string revision = field(install.output, "revision");
    const std::uint64_t install_baseline = std::strtoull(field(install.output, "baseline_kb").c_str(), nullptr, 10);
    const std::uint64_t install_peak = std::strtoull(field(install.output, "peak_kb").c_str(), nullptr, 10);
    CHECK(!revision.empty());
    CHECK_EQ(field(install.output, "file_count"), std::string("3"));
    const std::uint64_t expected_total = kLargeAssetSize + manifest_text().size() + std::strlen(kIndexHtml);
    CHECK_EQ(field(install.output, "total_bytes"), std::to_string(expected_total));
    CHECK(install_peak >= install_baseline);
    const std::uint64_t install_delta = install_peak > install_baseline ? install_peak - install_baseline : 0;
    std::printf("[memory] install: asset=%llu bytes baseline=%llu KiB peak=%llu KiB delta=%llu KiB\n",
                static_cast<unsigned long long>(kLargeAssetSize),
                static_cast<unsigned long long>(install_baseline),
                static_cast<unsigned long long>(install_peak),
                static_cast<unsigned long long>(install_delta));
    CHECK(install_delta < kPeakDeltaLimitKb);

    const ChildResult serve = run_child({"--serve-child", apps_root, revision});
    if (serve.exit_code != 0) {
        std::fprintf(stderr, "serve child output: %s\n", serve.output.c_str());
    }
    CHECK_EQ(serve.exit_code, 0);
    const std::uint64_t serve_baseline = std::strtoull(field(serve.output, "baseline_kb").c_str(), nullptr, 10);
    const std::uint64_t serve_peak = std::strtoull(field(serve.output, "peak_kb").c_str(), nullptr, 10);
    CHECK_EQ(field(serve.output, "served"), std::to_string(kLargeAssetSize));
    CHECK_EQ(field(serve.output, "size"), std::to_string(kLargeAssetSize));
    CHECK(!field(serve.output, "etag").empty());
    CHECK(serve_peak >= serve_baseline);
    const std::uint64_t serve_delta = serve_peak > serve_baseline ? serve_peak - serve_baseline : 0;
    std::printf("[memory] static: asset=%llu bytes baseline=%llu KiB peak=%llu KiB delta=%llu KiB\n",
                static_cast<unsigned long long>(kLargeAssetSize),
                static_cast<unsigned long long>(serve_baseline),
                static_cast<unsigned long long>(serve_peak),
                static_cast<unsigned long long>(serve_delta));
    CHECK(serve_delta < kPeakDeltaLimitKb);

    // Interrupted extraction: a stored entry larger than RLIMIT_FSIZE makes
    // write(2) fail with EFBIG (SIGXFSZ ignored) after the operation starts.
    const std::string fail_root = (base / "apps-failure").string();
    const std::string fail_zip = (base / "failure.zip").string();
    CHECK(write_package_zip(fail_zip, kFailureEntrySize, false, true));
    const ChildResult failure = run_child({"--failure-child", fail_zip, fail_root, "524288"});
    if (failure.exit_code != 0) {
        std::fprintf(stderr, "failure child output: %s\n", failure.output.c_str());
    }
    CHECK_EQ(failure.exit_code, 0);
    CHECK_EQ(field(failure.output, "failed_as_expected"), std::string("1"));

    // Inspect the filesystem before recover() can hide leftovers.
    const fs::path tmp_dir = fs::path(fail_root) / "tmp";
    const fs::path revisions_dir = fs::path(fail_root) / "revisions";
    std::size_t tmp_entries = 0;
    std::size_t revision_entries = 0;
    if (fs::exists(tmp_dir)) {
        for (const auto& ignored : fs::directory_iterator(tmp_dir)) {
            (void)ignored;
            ++tmp_entries;
        }
    }
    if (fs::exists(revisions_dir)) {
        for (const auto& ignored : fs::directory_iterator(revisions_dir)) {
            (void)ignored;
            ++revision_entries;
        }
    }
    CHECK_EQ(tmp_entries, std::size_t{0});
    CHECK_EQ(revision_entries, std::size_t{0});

    Error error;
    auto manager = PackageManager::create(fail_root, error);
    CHECK(manager != nullptr);
    if (manager) {
        std::vector<OperationRecord> pending;
        CHECK(manager->store().list_pending_operations(pending).ok());
        CHECK(pending.empty());
        std::vector<AppRecord> apps;
        CHECK(manager->store().list_apps(apps, true).ok());
        bool found = false;
        for (const auto& app : apps) {
            if (app.app_id == kAppId) found = true;
        }
        CHECK(!found);
    }

    fs::remove_all(base, ec);
    return edgedesk::test::finish("extension_memory_test");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 0 && argv[0] != nullptr) {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(argv[0], ec);
        if (!ec) self_executable_path = absolute.string();
    }
    if (argc >= 2) {
        const std::string mode = argv[1];
        if (mode == "--install-child" && argc >= 4) return install_child(argv[2], argv[3]);
        if (mode == "--serve-child" && argc >= 4) return serve_child(argv[2], argv[3]);
        if (mode == "--failure-child" && argc >= 5) {
            return failure_child(argv[2], argv[3], std::strtoull(argv[4], nullptr, 10));
        }
        std::fprintf(stderr, "unknown child mode\n");
        return 1;
    }
    return run_parent();
}
