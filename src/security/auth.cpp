#include "security/auth.hpp"

#include "core/types.hpp"
#include "core/util.hpp"

#include <stdexcept>
#include <utility>

namespace edgedesk {
namespace {

constexpr std::string_view kSessionVersion = "v1";

Error auth_unavailable(const std::string& message) {
    return Error::make(ErrorCode::unavailable, message);
}

} // namespace

Auth::Auth(AuthOptions options) : options_(std::move(options)) {
    secret_ = random_hex(32);
    if (secret_.empty()) {
        throw std::runtime_error("cannot obtain entropy for the session signing key");
    }
    std::string error;
    store_ = AuthStore::open(options_.data_dir, true, error);
    if (!store_) {
        throw std::runtime_error(error.empty() ? "cannot open authentication state" : error);
    }
    hasher_ = std::make_unique<PasswordHashService>(1, 4);
}

Auth::~Auth() = default;

AuthState Auth::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_->state();
}

std::string Auth::init_code_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_->init_code_path();
}

std::string Auth::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_->last_error();
}

bool Auth::ensure_init_code(bool rotate, std::string& code, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_->ensure_init_code(rotate, code, error);
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

bool Auth::verify_session(std::string_view cookie_value, std::int64_t now_seconds,
                          std::int64_t* expires_at) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return verify_session_locked(cookie_value, now_seconds, expires_at);
}

bool Auth::verify_session_locked(std::string_view cookie_value, std::int64_t now_seconds,
                                 std::int64_t* expires_at) const {
    if (expires_at != nullptr) *expires_at = 0;
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
    if (!constant_time_equal(trimmed.substr(second_dot + 1), expected)) return false;
    if (expires_at != nullptr) *expires_at = static_cast<std::int64_t>(expiry);
    return true;
}

std::string Auth::issue_session(std::int64_t now_seconds) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return issue_session_locked(now_seconds);
}

std::string Auth::issue_session_locked(std::int64_t now_seconds) const {
    const auto expiry = now_seconds + options_.session_ttl_seconds;
    const std::string payload = std::string(kSessionVersion) + "." + std::to_string(expiry);
    const auto mac = hmac_sha256_hex(secret_, payload);
    return payload + "." + mac;
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

bool Auth::rotate_session_key_locked() {
    const auto next = random_hex(32);
    if (next.empty()) return false;
    secret_ = next;
    return true;
}

bool Auth::setup_async(std::string code, std::string password, SetupCallback callback) {
    std::string validation_error;
    if (code.empty() || password.empty()) {
        if (callback) callback(Error::make(ErrorCode::invalid_argument, "initialization code and password are required"));
        return false;
    }
    if (!validate_password(password, validation_error)) {
        if (callback) callback(Error::make(ErrorCode::invalid_argument, validation_error));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (setup_in_progress_) {
            if (callback) callback(Error::make(ErrorCode::busy, "initialization is already in progress"));
            return false;
        }
        if (store_->state() == AuthState::initialized) {
            if (callback) callback(Error::make(ErrorCode::conflict, "authentication is already initialized"));
            return false;
        }
        if (store_->state() == AuthState::corrupt) {
            if (callback) callback(auth_unavailable("authentication state is corrupt; use local recovery"));
            return false;
        }
        std::string code_error;
        if (!store_->verify_init_code(code, code_error)) {
            if (callback) callback(Error::make(ErrorCode::permission_denied, "initialization code is invalid or expired"));
            return false;
        }
        setup_in_progress_ = true;
    }

    const bool queued = hasher_->hash_async(
        password, [this, code = std::move(code), callback](
                      Error hash_error, std::string salt_hex, std::string hash_hex) mutable {
            if (!hash_error.ok()) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    setup_in_progress_ = false;
                }
                if (callback) callback(hash_error);
                return;
            }
            Error save_error;
            bool saved = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                std::string store_error;
                if (store_->state() != AuthState::uninitialized) {
                    save_error = Error::make(ErrorCode::conflict, "authentication is already initialized");
                } else if (!store_->verify_init_code(code, store_error)) {
                    save_error = Error::make(ErrorCode::permission_denied,
                                             "initialization code is invalid or expired");
                } else {
                    AuthRecord record;
                    record.salt_hex = std::move(salt_hex);
                    record.hash_hex = std::move(hash_hex);
                    record.credential_version = 1;
                    record.created_at = now_unix_seconds();
                    saved = store_->save_record(record, store_error);
                    if (!saved) {
                        save_error = Error::make(ErrorCode::io_error, store_error);
                    } else if (!rotate_session_key_locked()) {
                        saved = false;
                        save_error = Error::make(ErrorCode::io_error, "cannot rotate session signing key");
                    }
                }
                setup_in_progress_ = false;
            }
            if (callback) callback(saved ? Error::none() : save_error);
        });
    if (!queued) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            setup_in_progress_ = false;
        }
        if (callback) callback(auth_unavailable("password hashing queue is full; retry shortly"));
    }
    return queued;
}

bool Auth::login_async(std::string password, LoginCallback callback) {
    AuthRecord record;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (store_->state() == AuthState::uninitialized) {
            if (callback) callback(Error::make(ErrorCode::unavailable, "authentication is not initialized"), {}, 0);
            return false;
        }
        if (store_->state() == AuthState::corrupt) {
            if (callback) callback(auth_unavailable("authentication state is corrupt; use local recovery"), {}, 0);
            return false;
        }
        std::string read_error;
        if (!store_->read_record(record, read_error)) {
            if (callback) callback(auth_unavailable("cannot read authentication state"), {}, 0);
            return false;
        }
    }
    const auto expected_version = record.credential_version;
    const bool queued = hasher_->verify_async(
        password, record,
        [this, expected_version, callback](Error verify_error, bool matches) mutable {
            if (!verify_error.ok()) {
                if (callback) callback(verify_error, {}, 0);
                return;
            }
            if (!matches) {
                if (callback) {
                    callback(Error::make(ErrorCode::permission_denied, "invalid credentials"), {}, 0);
                }
                return;
            }
            Error result = Error::none();
            std::string cookie;
            std::int64_t expires_at = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                AuthRecord current;
                std::string store_error;
                if (store_->state() != AuthState::initialized || !store_->read_record(current, store_error) ||
                    current.credential_version != expected_version) {
                    result = Error::make(ErrorCode::permission_denied, "invalid credentials");
                } else {
                    const auto now = now_unix_seconds();
                    cookie = issue_session_locked(now);
                    expires_at = now + options_.session_ttl_seconds;
                }
            }
            if (callback) callback(result, std::move(cookie), expires_at);
        });
    if (!queued && callback) {
        callback(auth_unavailable("password hashing queue is full; retry shortly"), {}, 0);
    }
    return queued;
}

bool Auth::change_password_async(std::string current_password, std::string new_password,
                                 ChangeCallback callback) {
    std::string validation_error;
    if (!validate_password(new_password, validation_error)) {
        if (callback) callback(Error::make(ErrorCode::invalid_argument, validation_error));
        return false;
    }
    AuthRecord record;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (store_->state() != AuthState::initialized) {
            if (callback) callback(auth_unavailable("authentication is not initialized"));
            return false;
        }
        std::string read_error;
        if (!store_->read_record(record, read_error)) {
            if (callback) callback(auth_unavailable("cannot read authentication state"));
            return false;
        }
    }
    const auto expected_version = record.credential_version;
    const bool queued = hasher_->verify_async(
        current_password, record,
        [this, new_password = std::move(new_password), expected_version,
         callback](Error verify_error, bool matches) mutable {
            if (!verify_error.ok()) {
                if (callback) callback(verify_error);
                return;
            }
            if (!matches) {
                if (callback) callback(Error::make(ErrorCode::permission_denied, "current password is incorrect"));
                return;
            }
            const bool hash_queued = hasher_->hash_async(
                new_password,
                [this, expected_version, callback](
                    Error hash_error, std::string salt_hex, std::string hash_hex) mutable {
                    if (!hash_error.ok()) {
                        if (callback) callback(hash_error);
                        return;
                    }
                    Error save_error;
                    bool saved = false;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        AuthRecord current;
                        std::string store_error;
                        if (store_->state() != AuthState::initialized) {
                            save_error = auth_unavailable("authentication is not initialized");
                        } else if (!store_->read_record(current, store_error)) {
                            save_error = auth_unavailable("cannot read authentication state");
                        } else if (current.credential_version != expected_version) {
                            save_error = Error::make(ErrorCode::conflict,
                                                     "credentials changed concurrently; retry");
                        } else {
                            current.salt_hex = std::move(salt_hex);
                            current.hash_hex = std::move(hash_hex);
                            current.credential_version = expected_version + 1;
                            current.created_at = now_unix_seconds();
                            saved = store_->save_record(current, store_error);
                            if (!saved) {
                                save_error = Error::make(ErrorCode::io_error, store_error);
                            } else if (!rotate_session_key_locked()) {
                                saved = false;
                                save_error = Error::make(ErrorCode::io_error,
                                                         "cannot rotate session signing key");
                            }
                        }
                    }
                    if (callback) callback(saved ? Error::none() : save_error);
                });
            if (!hash_queued && callback) {
                callback(auth_unavailable("password hashing queue is full; retry shortly"));
            }
        });
    if (!queued && callback) {
        callback(auth_unavailable("password hashing queue is full; retry shortly"));
    }
    return queued;
}

} // namespace edgedesk
