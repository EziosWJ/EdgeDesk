#pragma once

#include "core/fd.hpp"

#include <cstdint>
#include <memory>
#include <string>

// Persistent authentication state (auth.json).
//
// AuthStore is intentionally not thread-safe; Auth serializes access with its
// own mutex. Recovery uses the same .lock file to prove the server is stopped.

namespace edgedesk {

enum class AuthState { uninitialized, initialized, corrupt };

const char* auth_state_name(AuthState state);

struct AuthRecord {
    int schema = 1;
    std::string algorithm = "argon2id";
    std::uint32_t memory_kib = 64 * 1024;
    std::uint32_t iterations = 3;
    std::uint32_t parallelism = 1;
    std::string salt_hex;
    std::string hash_hex;
    std::uint64_t credential_version = 1;
    std::int64_t created_at = 0;
};

class AuthStore {
public:
    // Opens data_dir as a private 0700 directory (creating it if needed).
    // When `take_lock` is true, acquires the non-blocking `.lock` file; a busy
    // lock means another server or recovery process is using the directory.
    static std::unique_ptr<AuthStore> open(const std::string& data_dir, bool take_lock, std::string& error);

    AuthStore(const AuthStore&) = delete;
    AuthStore& operator=(const AuthStore&) = delete;

    AuthState state() const { return state_; }
    const std::string& data_dir() const { return data_dir_; }
    const std::string& last_error() const { return last_error_; }

    bool read_record(AuthRecord& record, std::string& error) const;

    // Atomically replaces auth.json with a new credential record and marks the
    // store initialized.
    bool save_record(const AuthRecord& record, std::string& error);

    // Used by --reset-admin-password: removes auth.json, keeps settings/audit/
    // business data and returns to the uninitialized state.
    bool reset_for_recovery(std::string& error);

private:
    AuthStore(UniqueFd dir_fd, UniqueFd lock_fd, std::string data_dir) noexcept;
    bool reload(std::string& error);

    UniqueFd dir_fd_;
    UniqueFd lock_fd_;
    std::string data_dir_;
    AuthState state_ = AuthState::uninitialized;
    std::string last_error_;
};

} // namespace edgedesk
