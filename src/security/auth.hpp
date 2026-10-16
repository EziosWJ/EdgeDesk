#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Deployment token + signed cookie sessions (ADR-0003).
// No libhv types here; the HTTP middleware adapts headers/requests.

namespace edgedesk {

struct AuthOptions {
    std::string token;             // resolved deployment access token
    int session_ttl_seconds = 12 * 3600;
    bool cookie_secure = false;    // set when deployments terminate TLS upstream
};

class Auth {
public:
    explicit Auth(AuthOptions options);

    static const char* cookie_name() { return "edgedesk_session"; }

    bool verify_token(std::string_view candidate) const;
    bool verify_bearer(std::string_view authorization_header) const;

    std::string issue_session(std::int64_t now_seconds) const;
    bool verify_session(std::string_view cookie_value, std::int64_t now_seconds) const;

    std::string session_cookie(std::string_view value) const;
    std::string expired_session_cookie() const;

    int ttl_seconds() const { return options_.session_ttl_seconds; }

private:
    AuthOptions options_;
    std::string secret_; // per-process HMAC key; sessions die with the process
};

} // namespace edgedesk
