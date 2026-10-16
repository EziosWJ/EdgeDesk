#include "security/auth.hpp"

#include "core/types.hpp"
#include "core/util.hpp"

#include <stdexcept>

namespace edgedesk {
namespace {

constexpr std::string_view kSessionVersion = "v1";

} // namespace

Auth::Auth(AuthOptions options) : options_(std::move(options)) {
    secret_ = random_hex(32);
    if (secret_.empty()) {
        // Cookie signing must never run with an empty HMAC key: fail hard
        // rather than accept forgeable sessions.
        throw std::runtime_error("cannot obtain entropy for the session signing key");
    }
}

bool Auth::verify_token(std::string_view candidate) const {
    if (candidate.empty() || options_.token.empty()) return false;
    return constant_time_equal(candidate, options_.token);
}

bool Auth::verify_bearer(std::string_view authorization_header) const {
    const auto trimmed = trim(authorization_header);
    constexpr std::string_view prefix = "Bearer ";
    if (trimmed.size() <= prefix.size()) return false;
    if (trimmed.compare(0, prefix.size(), prefix) != 0) return false;
    return verify_token(std::string_view(trimmed).substr(prefix.size()));
}

std::string Auth::issue_session(std::int64_t now_seconds) const {
    const auto expiry = now_seconds + options_.session_ttl_seconds;
    const std::string payload = std::string(kSessionVersion) + "." + std::to_string(expiry);
    const auto mac = hmac_sha256_hex(secret_, payload);
    return payload + "." + mac;
}

bool Auth::verify_session(std::string_view cookie_value, std::int64_t now_seconds) const {
    const auto trimmed = trim(cookie_value);
    const auto first_dot = trimmed.find('.');
    if (first_dot == std::string_view::npos) return false;
    const auto second_dot = trimmed.find('.', first_dot + 1);
    if (second_dot == std::string_view::npos) return false;
    const auto version = trimmed.substr(0, first_dot);
    if (version != kSessionVersion) return false;
    const auto expiry_text = trimmed.substr(first_dot + 1, second_dot - first_dot - 1);
    std::uint64_t expiry = 0;
    if (!parse_uint64(expiry_text, expiry)) return false;
    if (static_cast<std::int64_t>(expiry) < now_seconds) return false;
    const auto payload = trimmed.substr(0, second_dot);
    const auto expected = hmac_sha256_hex(secret_, payload);
    return constant_time_equal(trimmed.substr(second_dot + 1), expected);
}

std::string Auth::session_cookie(std::string_view value) const {
    std::string cookie = std::string(cookie_name()) + "=" + std::string(value) +
                         "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
                         std::to_string(options_.session_ttl_seconds);
    if (options_.cookie_secure) cookie += "; Secure";
    return cookie;
}

std::string Auth::expired_session_cookie() const {
    std::string cookie = std::string(cookie_name()) + "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0";
    if (options_.cookie_secure) cookie += "; Secure";
    return cookie;
}

} // namespace edgedesk
