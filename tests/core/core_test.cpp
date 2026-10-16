#include "test_util.hpp"

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/types.hpp"
#include "core/util.hpp"
#include "security/audit.hpp"
#include "security/auth.hpp"
#include "security/rate_limiter.hpp"

#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace edgedesk;

namespace {

void write_config_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path);
    file << content;
    file.close();
}

void test_config(const std::filesystem::path& tmp) {
    const std::string root = tmp.string();
    {
        const char* argv[] = {"edgedesk", "--host", "127.0.0.1", "--port", "9042", "--file-root", root.c_str(),
                              "--auth-token", "secret"};
        auto config = parse_args(9, argv);
        CHECK_STR_EQ(config.host, "127.0.0.1");
        CHECK_EQ(config.port, 9042);
        CHECK_STR_EQ(config.file_root, root);
        CHECK(!config.auth_token_ephemeral);
        CHECK_STR_EQ(config.auth_token, "secret");
    }
    {
        // Public bind requires an explicit token.
        const char* argv[] = {"edgedesk", "--host", "0.0.0.0", "--file-root", root.c_str()};
        bool threw = false;
        try {
            (void)parse_args(5, argv);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }
    {
        const char* argv[] = {"edgedesk", "--host", "0.0.0.0", "--file-root", root.c_str(), "--auth-token", "secret"};
        auto config = parse_args(7, argv);
        CHECK_STR_EQ(config.host, "0.0.0.0");
    }
    {
        // Ephemeral token is generated when none is configured.
        const char* argv[] = {"edgedesk", "--file-root", root.c_str()};
        auto config = parse_args(3, argv);
        CHECK(config.auth_token_ephemeral);
        CHECK_EQ(config.auth_token.size(), std::size_t{32}); // 16 random bytes hex
    }
    {
        const char* argv[] = {"edgedesk", "--port", "99999", "--file-root", root.c_str()};
        bool threw = false;
        try {
            (void)parse_args(5, argv);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }
    {
        const char* argv[] = {"edgedesk", "--file-root", "/definitely/not/here/edgedesk"};
        bool threw = false;
        try {
            (void)parse_args(3, argv);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }
    {
        // Config file + CLI override + env token.
        const auto config_path = tmp / "edgedesk.conf";
        write_config_file(config_path, "# comment\nport = 9001\nfile-root = " + root +
                                           "\nmetrics-interval = 2000\nno-audit = 1\n");
        ::setenv("EDGEDESK_AUTH_TOKEN", "envtoken", 1);
        const char* argv[] = {"edgedesk", "--config", config_path.c_str(), "--port", "9002"};
        auto config = parse_args(5, argv);
        CHECK_EQ(config.port, 9002);
        CHECK_EQ(config.metrics_interval_ms, 2000);
        CHECK(!config.audit_enabled);
        CHECK_STR_EQ(config.auth_token, "envtoken");
        ::unsetenv("EDGEDESK_AUTH_TOKEN");
    }
    {
        // Token file wins over the environment variable.
        const auto token_path = tmp / "token.txt";
        write_config_file(token_path, "filetoken\n");
        ::setenv("EDGEDESK_AUTH_TOKEN", "envtoken", 1);
        const char* argv[] = {"edgedesk", "--file-root", root.c_str(), "--auth-token-file", token_path.c_str()};
        auto config = parse_args(5, argv);
        CHECK_STR_EQ(config.auth_token, "filetoken");
        ::unsetenv("EDGEDESK_AUTH_TOKEN");
    }
    {
        const char* argv[] = {"edgedesk", "--help"};
        auto config = parse_args(2, argv);
        CHECK(config.help_requested);
        CHECK(help_text().find("--file-root") != std::string::npos);
    }
}

void test_crypto() {
    CHECK_STR_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_STR_EQ(sha256_hex(""),
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_STR_EQ(hmac_sha256_hex("Jefe", "what do ya want for nothing?"),
                 "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK_STR_EQ(hex_encode("ab\x01"), "616201");
    CHECK(constant_time_equal("abc", "abc"));
    CHECK(!constant_time_equal("abc", "abd"));
    CHECK(!constant_time_equal("abc", "ab"));
    const auto random = random_hex(16);
    CHECK_EQ(random.size(), std::size_t{32});
    CHECK(random_hex(16) != random);
}

void test_types() {
    std::uint64_t size = 0;
    CHECK(parse_byte_size("4096", size) && size == 4096);
    CHECK(parse_byte_size("256K", size) && size == 256 * 1024);
    CHECK(parse_byte_size("16M", size) && size == 16 * 1024 * 1024);
    CHECK(parse_byte_size("2G", size) && size == 2ull * 1024 * 1024 * 1024);
    CHECK(!parse_byte_size("12X", size));
    CHECK(!parse_byte_size("", size));
    CHECK(!parse_uint64("-1", size));
    CHECK(is_loopback_host("127.0.0.1"));
    CHECK(is_loopback_host("::1"));
    CHECK(!is_loopback_host("0.0.0.0"));
    CHECK_STR_EQ(trim("  x \t"), "x");
    const auto parts = split("/a//b/c", '/');
    CHECK_EQ(parts.size(), std::size_t{3});
    CHECK_STR_EQ(to_lower_ascii("AbC"), "abc");

    std::string decoded;
    CHECK(url_decode("a%20b%2Fc", decoded));
    CHECK_STR_EQ(decoded, "a b/c");
    CHECK(!url_decode("bad%2", decoded));
    CHECK_STR_EQ(url_encode_component("a b/中文"), "a%20b%2F%E4%B8%AD%E6%96%87");
}

void test_error_mapping() {
    CHECK_EQ(http_status_for(ErrorCode::not_found), 404);
    CHECK_EQ(http_status_for(ErrorCode::permission_denied), 403);
    CHECK_EQ(http_status_for(ErrorCode::too_large), 413);
    CHECK_EQ(http_status_for(ErrorCode::limit_exceeded), 429);
    CHECK_STR_EQ(error_code_slug(ErrorCode::already_exists), "already_exists");
}

void test_auth() {
    AuthOptions options;
    options.token = "s3cret";
    options.session_ttl_seconds = 100;
    Auth auth(options);
    CHECK(auth.verify_token("s3cret"));
    CHECK(!auth.verify_token("wrong"));
    CHECK(!auth.verify_token(""));
    CHECK(auth.verify_bearer("Bearer s3cret"));
    CHECK(!auth.verify_bearer("Bearer wrong"));
    CHECK(!auth.verify_bearer("Basic s3cret"));

    const auto session = auth.issue_session(1000);
    CHECK(auth.verify_session(session, 1000));
    CHECK(auth.verify_session(session, 1099));
    CHECK(!auth.verify_session(session, 1101)); // expired
    CHECK(!auth.verify_session(session + "x", 1000));
    CHECK(!auth.verify_session("v1.9999999999.deadbeef", 1000));

    Auth other(options);
    CHECK(!other.verify_session(session, 1000)); // per-process secret
    CHECK(auth.session_cookie(session).find("HttpOnly") != std::string::npos);
    CHECK(auth.session_cookie(session).find("SameSite=Strict") != std::string::npos);
}

void test_rate_limiter() {
    RateLimiter limiter(10.0, 2.0);
    const std::int64_t now = 100000;
    CHECK(limiter.allow("a", now));
    CHECK(limiter.allow("a", now));
    CHECK(!limiter.allow("a", now)); // burst exhausted
    CHECK(limiter.allow("b", now));  // per-key buckets
    CHECK(limiter.allow("a", now + 100)); // 1 token refilled in 100ms at 10/s
    limiter.sweep(now + 20 * 60 * 1000);
    CHECK(limiter.allow("a", now + 20 * 60 * 1000));

    ConcurrencyGate gate(2);
    CHECK(gate.try_acquire());
    CHECK(gate.try_acquire());
    CHECK(!gate.try_acquire());
    gate.release();
    CHECK(gate.try_acquire());
    CHECK_EQ(gate.in_use(), 2);
}

void test_audit_rotation(const std::filesystem::path& tmp) {
    const auto log_path = tmp / "audit-rotation.log";
    AuditOptions options;
    options.path = log_path.string();
    options.max_bytes = 4096;
    options.max_files = 2;
    Error error;
    auto audit = AuditLog::create(options, error);
    CHECK(audit != nullptr);
    if (!audit) return;
    for (int index = 0; index < 200; ++index) {
        audit->record("unit_test", "ok", std::string(80, 'x') + std::to_string(index), "127.0.0.1", "/tmp");
    }
    CHECK(std::filesystem::exists(log_path));
    CHECK(std::filesystem::exists(log_path.string() + ".1"));
    CHECK(std::filesystem::file_size(log_path) <= options.max_bytes);
    // Disabled audit is a no-op instead of an error.
    AuditOptions disabled;
    disabled.enabled = false;
    Error disabled_error;
    auto noop = AuditLog::create(disabled, disabled_error);
    CHECK(noop != nullptr);
    noop->record("never", "recorded");
}

void test_state_dir(const std::filesystem::path& tmp) {
    const auto state = tmp / "state";
    std::string error;
    CHECK(ensure_state_dir(state.string(), error));
    CHECK(std::filesystem::is_directory(state));
    CHECK(ensure_state_dir(state.string(), error)); // idempotent
    std::string bad_error;
    CHECK(!ensure_state_dir("relative", bad_error));
}

} // namespace

int main() {
    const auto tmp = std::filesystem::temp_directory_path() / ("edgedesk-core-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(tmp);
    test_config(tmp);
    test_crypto();
    test_types();
    test_error_mapping();
    test_auth();
    test_rate_limiter();
    test_audit_rotation(tmp);
    test_state_dir(tmp);
    std::filesystem::remove_all(tmp);
    return edgedesk::test::finish("core_test");
}
