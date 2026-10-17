#pragma once

#include "core/error.hpp"
#include "security/auth_store.hpp"
#include "security/password_hash.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

// Single-admin password authentication + optional Bearer API token.
//
// Session signing keys are per-process and random. Password changes rotate the
// key under the same mutex, invalidating every existing browser cookie while
// leaving the independently configured Bearer token untouched.

namespace edgedesk {

struct AuthOptions {
    std::string token;             // optional Bearer API token
    int session_ttl_seconds = 12 * 3600;
    bool cookie_secure = false;
    std::string data_dir;
};

class Auth {
public:
    using SetupCallback = std::function<void(Error error)>;
    using LoginCallback = std::function<void(Error error, std::string cookie, std::int64_t expires_at)>;
    using ChangeCallback = std::function<void(Error error)>;

    explicit Auth(AuthOptions options);
    ~Auth();

    Auth(const Auth&) = delete;
    Auth& operator=(const Auth&) = delete;

    static const char* cookie_name() { return "edgedesk_session"; }

    AuthState state() const;
    std::string last_error() const;

    bool token_configured() const { return !options_.token.empty(); }
    bool verify_token(std::string_view candidate) const;
    bool verify_bearer(std::string_view authorization_header) const;

    // Cheap session verification used by middleware.
    bool verify_session(std::string_view cookie_value, std::int64_t now_seconds,
                        std::int64_t* expires_at = nullptr) const;
    std::string issue_session(std::int64_t now_seconds) const;
    std::string session_cookie(std::string_view value) const;
    std::string expired_session_cookie() const;
    int ttl_seconds() const { return options_.session_ttl_seconds; }

    // Asynchronous flows. Callbacks run on the bounded password worker pool.
    bool setup_async(std::string password, SetupCallback callback);
    bool login_async(std::string password, LoginCallback callback);
    bool change_password_async(std::string current_password, std::string new_password,
                               ChangeCallback callback);

private:
    bool verify_session_locked(std::string_view cookie_value, std::int64_t now_seconds,
                               std::int64_t* expires_at) const;
    std::string issue_session_locked(std::int64_t now_seconds) const;
    bool rotate_session_key_locked();

    AuthOptions options_;
    std::unique_ptr<AuthStore> store_;
    std::unique_ptr<PasswordHashService> hasher_;
    mutable std::mutex mutex_;
    std::string secret_;
    bool setup_in_progress_ = false;
};

} // namespace edgedesk
