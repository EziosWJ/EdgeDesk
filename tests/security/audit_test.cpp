#include "core/utf8.hpp"
#include "security/audit.hpp"
#include "security/redaction.hpp"
#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace edgedesk;

namespace {

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

void test_header_redaction() {
    const std::string bearer = "BEARER-SECRET-8c1f";
    auto out = redact_sensitive("Authorization: Bearer " + bearer);
    CHECK(!contains(out, bearer));
    CHECK(contains(out, "Authorization:"));
    CHECK(contains(out, "Bearer"));
    CHECK(contains(out, kAuditRedactionMarker));

    const std::string basic = "QmFzaWNTZWNyZXQ=";
    out = redact_sensitive("authorization: Basic " + basic);
    CHECK(!contains(out, basic));
    CHECK(contains(out, "Basic"));

    const std::string cookie = "COOKIE-SECRET-77";
    out = redact_sensitive("Cookie: edgedesk_session=" + cookie + "; theme=dark");
    CHECK(!contains(out, cookie));
    CHECK(contains(out, "Cookie:"));

    const std::string set_cookie = "SET-COOKIE-SECRET-88";
    out = redact_sensitive("Set-Cookie: edgedesk_session=" + set_cookie + "; Path=/; HttpOnly");
    CHECK(!contains(out, set_cookie));

    const std::string api_key = "API-KEY-SECRET-99";
    out = redact_sensitive("X-Api-Key: " + api_key);
    CHECK(!contains(out, api_key));
    CHECK(contains(out, "X-Api-Key:"));

    // Multi-line text: a header value stops at the newline, so the following
    // non-sensitive diagnostic line survives.
    out = redact_sensitive("Authorization: Bearer " + bearer + "\nnext line kept");
    CHECK(!contains(out, bearer));
    CHECK(contains(out, "next line kept"));
}

void test_pair_redaction() {
    static constexpr const char* kKeys[] = {
        "token", "access_token", "refresh_token", "id_token", "api_key", "apikey", "secret",
        "password", "passwd", "pwd", "session", "cookie", "authorization",
    };
    int index = 0;
    for (const char* key : kKeys) {
        const std::string secret = "PAIR-SECRET-" + std::to_string(index++) + "-xyz";
        const std::string input = std::string("user=bob&") + key + "=" + secret + "&page=2";
        const std::string out = redact_sensitive(input);
        CHECK(!contains(out, secret));
        CHECK(contains(out, key));
        CHECK(contains(out, "user=bob"));
        CHECK(contains(out, "page=2"));
    }

    const std::string json_secret = "JSON-SECRET-123";
    auto out = redact_sensitive(
        "{\"Access_Token\":\"" + json_secret + "\",\"user\":\"bob\",\"theme\":\"dark\"}");
    CHECK(!contains(out, json_secret));
    CHECK(contains(out, "Access_Token"));
    CHECK(contains(out, "user"));
    CHECK(contains(out, "dark"));

    const std::string single_quoted = "SINGLE-QUOTED-SECRET";
    out = redact_sensitive("'PASSWORD': '" + single_quoted + "' rest");
    CHECK(!contains(out, single_quoted));
    CHECK(contains(out, "PASSWORD"));
    CHECK(contains(out, "rest"));

    const std::string mixed = "MiXeD-CaSe-SeCrEt";
    out = redact_sensitive("ToKeN = " + mixed);
    CHECK(!contains(out, mixed));

    // A secret value with punctuation that is normally a pair terminator is
    // safely contained when quoted.
    const std::string quoted_complex = "abc&def;ghi";
    out = redact_sensitive("secret=\"" + quoted_complex + "\"");
    CHECK(!contains(out, "abc&def"));
}

void test_url_redaction() {
    const std::string query_secret = "URL-QUERY-SECRET";
    const std::string fragment_secret = "URL-FRAGMENT-SECRET";
    const std::string userinfo_secret = "URL-USERINFO-SECRET";
    const std::string url = "https://user:" + userinfo_secret + "@example.com:8443/cb?foo=1&access%5Ftoken=" +
                            query_secret + "&state=ok#access_token=" + fragment_secret + "&section=top";
    const std::string out = redact_sensitive(url);
    CHECK(!contains(out, query_secret));
    CHECK(!contains(out, fragment_secret));
    CHECK(!contains(out, userinfo_secret));
    CHECK(contains(out, "https://"));
    CHECK(contains(out, "user:"));
    CHECK(contains(out, "example.com:8443/cb"));
    CHECK(contains(out, "foo=1"));
    CHECK(contains(out, "state=ok"));
    CHECK(contains(out, "section=top"));

    // Opaque fragments cannot be classified, so they are dropped entirely.
    const std::string opaque = "OPAQUE-FRAGMENT-SECRET";
    auto opaque_out = redact_sensitive("https://example.com/cb#" + opaque);
    CHECK(!contains(opaque_out, opaque));
    CHECK(contains(opaque_out, "https://example.com/cb"));

    const std::string relative = "RELATIVE-FRAGMENT-SECRET";
    auto relative_out = redact_sensitive("/oauth/callback#" + relative);
    CHECK(!contains(relative_out, relative));
    CHECK(contains(relative_out, "/oauth/callback"));

    // URL-encoded values are removed as a whole.
    auto encoded = redact_sensitive("https://example.com/cb?token=abc%20def%26ghi&x=1");
    CHECK(!contains(encoded, "abc%20def"));
    CHECK(!contains(encoded, "abc def"));
    CHECK(contains(encoded, "x=1"));
}

void test_bare_bearer_and_idempotency() {
    const std::string bare = "BARE-BEARER-SECRET";
    auto out = redact_sensitive("request failed: Bearer " + bare + " after");
    CHECK(!contains(out, bare));
    CHECK(contains(out, "request failed:"));
    CHECK(contains(out, "after"));

    const std::string sample =
        "Authorization: Bearer header-secret\n"
        "Cookie: edgedesk_session=cookie-secret; theme=dark\n"
        "login url=https://u:pw-secret@example.com/cb?access_token=q-secret#f-secret\n"
        "{\"api_key\":\"json-secret\",\"note\":\"password=form-secret\"}\n"
        "theme=dark bare Bearer bare-secret end";
    const std::string once = redact_sensitive(sample);
    const std::string twice = redact_sensitive(once);
    CHECK_STR_EQ(once, twice);
    for (const char* secret : {"header-secret", "cookie-secret", "pw-secret", "q-secret", "f-secret",
                               "json-secret", "form-secret", "bare-secret"}) {
        CHECK(!contains(once, secret));
    }
    CHECK(contains(once, "theme=dark"));
    CHECK(contains(once, "example.com"));
    CHECK(contains(once, "end"));
}

void test_empty_multiline_and_long() {
    CHECK_STR_EQ(redact_sensitive(""), "");
    CHECK_STR_EQ(redact_sensitive("token="), "token=");
    CHECK_STR_EQ(redact_sensitive("password=\"\""), "password=\"\"");

    const std::string multiline = "line1\npassword=MULTI-SECRET\nline3";
    const std::string out = redact_sensitive(multiline);
    CHECK(!contains(out, "MULTI-SECRET"));
    CHECK(contains(out, "line1"));
    CHECK(contains(out, "line3"));

    const std::string long_input = "prefix " + std::string(20000, 'x') + " token=LONG-SECRET";
    const std::string long_out = redact_sensitive(long_input);
    CHECK(!contains(long_out, "LONG-SECRET"));
    CHECK(contains(long_out, "prefix"));
}

void test_truncation_utf8() {
    const std::string emoji = "\xF0\x9F\x98\x80"; // U+1F600, four bytes
    const std::string input = std::string(10, 'a') + emoji + std::string(20, 'z');
    const std::size_t max_bytes = 22; // keep = 11 lands inside the emoji
    const std::string truncated = truncate_utf8(input, max_bytes);
    CHECK(truncated.size() <= max_bytes);
    CHECK(is_valid_utf8(truncated));
    CHECK(truncated.rfind(kAuditTruncationMarker) != std::string::npos);
    CHECK_STR_EQ(truncated.substr(0, 10), std::string(10, 'a'));

    CHECK_STR_EQ(truncate_utf8("fits", 16), "fits");

    std::string invalid = "ok ";
    invalid.push_back(static_cast<char>(0xff));
    const std::string sanitized = sanitize_audit_field(invalid);
    CHECK(is_valid_utf8(sanitized));

    const std::string long_secret = "token=LONG-AUDIT-SECRET " + std::string(9000, 'x');
    const std::string bounded = sanitize_audit_field(long_secret);
    CHECK(!contains(bounded, "LONG-AUDIT-SECRET"));
    CHECK(bounded.size() <= kAuditFieldMaxBytes);
    CHECK(is_valid_utf8(bounded));
    CHECK(bounded.rfind(kAuditTruncationMarker) ==
          bounded.size() - kAuditTruncationMarker.size());
}

void test_quote_audit_field() {
    const std::string quoted = quote_audit_field("a\nb\"c\\d\tend");
    CHECK_STR_EQ(quoted, "\"a\\nb\\\"c\\\\d\\tend\"");
    CHECK(!contains(quoted, "\n"));
    CHECK(is_valid_utf8(quoted));

    const std::string control = quote_audit_field(std::string("x") + '\x01' + "y");
    CHECK(contains(control, "\\u0001"));
    CHECK(!contains(control, "\x01"));
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    return lines;
}

nlohmann::json parse_or_fail(const std::string& line, int index) {
    try {
        return nlohmann::json::parse(line);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED audit line %d is not valid JSON: %s\n  line: %s\n", index,
                     error.what(), line.c_str());
        ++test::g_failures;
        return nlohmann::json::object();
    }
}

void test_audit_sink(const std::filesystem::path& tmp) {
    const auto log_path = tmp / "audit-redaction.log";
    AuditOptions options;
    options.path = log_path.string();
    options.max_bytes = 1ull * 1024 * 1024;
    Error error;
    auto audit = AuditLog::create(options, error);
    CHECK(audit != nullptr);
    if (!audit) return;

    const std::string bearer = "SINK-BEARER-SECRET";
    const std::string cookie = "SINK-COOKIE-SECRET";
    const std::string password = "SINK-PASSWORD-SECRET";
    const std::string query = "SINK-QUERY-SECRET";
    const std::string detail = "type=\"error\" message=\"Authorization: Bearer " + bearer +
                               "\nCookie: edgedesk_session=" + cookie + "\npassword=" + password +
                               "\" url=\"https://example.com/cb?token=" + query + "&page=3\"";
    audit->record("extension.error", "reported", detail, "127.0.0.1",
                  "/api/example?access_token=" + query + "&page=1");

    const std::string long_detail = "prefix " + std::string(9000, 'a');
    const std::string long_path = "/files/" + std::string(9000, 'b');
    audit->record("extension.error", "reported", long_detail, "127.0.0.1", long_path);

    // Emoji straddles the 8192-byte boundary.
    const std::string emoji_detail = std::string(8179, 'u') + "\xF0\x9F\x98\x80" +
                                     std::string(64, 'v');
    audit->record("extension.error", "reported", emoji_detail, "127.0.0.1", "");

    const auto lines = read_lines(log_path);
    CHECK_EQ(lines.size(), std::size_t{3});
    if (lines.size() != 3) return;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        // A raw newline would have split the record. Every physical line must
        // still be a complete JSON object in the original field order.
        CHECK(lines[index].rfind("{\"ts\":\"", 0) == 0);
        CHECK(lines[index].back() == '}');
        const auto parsed = parse_or_fail(lines[index], static_cast<int>(index));
        CHECK(parsed.contains("ts"));
        CHECK(parsed.contains("event"));
        CHECK(parsed.contains("outcome"));
        CHECK(parsed.contains("ip"));
        CHECK(parsed.contains("detail"));
        if (parsed.contains("path")) {
            CHECK(parsed["path"].is_string());
        }
        if (parsed.contains("detail")) {
            CHECK(parsed["detail"].is_string());
        }
    }

    std::string whole;
    for (const auto& line : lines) whole += line + "\n";
    for (const std::string& secret : {bearer, cookie, password, query}) {
        CHECK(!contains(whole, secret));
    }

    const auto first = nlohmann::json::parse(lines[0]);
    const std::string first_detail = first["detail"].get<std::string>();
    const std::string first_path = first["path"].get<std::string>();
    CHECK(contains(first_detail, "Authorization:"));
    CHECK(contains(first_detail, "Cookie:"));
    CHECK(contains(first_detail, "example.com/cb"));
    CHECK(contains(first_detail, "page=3"));
    CHECK(contains(first_detail, kAuditRedactionMarker));
    CHECK(contains(first_path, "/api/example"));
    CHECK(contains(first_path, "page=1"));
    CHECK(contains(first_path, kAuditRedactionMarker));

    const auto second = nlohmann::json::parse(lines[1]);
    const std::string second_detail = second["detail"].get<std::string>();
    const std::string second_path = second["path"].get<std::string>();
    CHECK(second_detail.size() <= kAuditFieldMaxBytes);
    CHECK(second_path.size() <= kAuditFieldMaxBytes);
    CHECK(second_detail.rfind(kAuditTruncationMarker) ==
          second_detail.size() - kAuditTruncationMarker.size());
    CHECK(second_path.rfind(kAuditTruncationMarker) ==
          second_path.size() - kAuditTruncationMarker.size());
    CHECK(contains(second_detail, "prefix"));

    const auto third = nlohmann::json::parse(lines[2]);
    const std::string third_detail = third["detail"].get<std::string>();
    CHECK(third_detail.size() <= kAuditFieldMaxBytes);
    CHECK(is_valid_utf8(third_detail));
    CHECK(third_detail.rfind(kAuditTruncationMarker) ==
          third_detail.size() - kAuditTruncationMarker.size());
}

} // namespace

int main() {
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("edgedesk-audit-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    test_header_redaction();
    test_pair_redaction();
    test_url_redaction();
    test_bare_bearer_and_idempotency();
    test_empty_multiline_and_long();
    test_truncation_utf8();
    test_quote_audit_field();
    test_audit_sink(tmp);

    std::filesystem::remove_all(tmp);
    return test::finish("audit_test");
}
