#pragma once

#include "core/error.hpp"
#include "security/auth_store.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// Password policy and Argon2id hashing. All expensive work goes through the
// bounded PasswordHashService worker pool; callers must not run it inline on
// the libhv event loop.

namespace edgedesk {

constexpr std::uint32_t kPasswordMemoryKiB = 64 * 1024;
constexpr std::uint32_t kPasswordIterations = 3;
constexpr std::uint32_t kPasswordParallelism = 1;
constexpr std::size_t kPasswordMinCodepoints = 12;
constexpr std::size_t kPasswordMaxBytes = 1024;

// Validates UTF-8, printable characters and the 12..1024 length policy.
bool validate_password(std::string_view password, std::string& error);

// Synchronous primitives used by tests and the worker pool.
bool hash_password(std::string_view password, std::uint32_t memory_kib, std::uint32_t iterations,
                   std::uint32_t parallelism, std::string& salt_hex, std::string& hash_hex, Error& error);
bool verify_password(std::string_view password, const AuthRecord& record, bool& matches, Error& error);
bool password_needs_rehash(const AuthRecord& record);

class PasswordHashService {
public:
    using HashCallback = std::function<void(Error error, std::string salt_hex, std::string hash_hex)>;
    using VerifyCallback = std::function<void(Error error, bool matches)>;

    explicit PasswordHashService(int workers = 1, std::size_t queue_limit = 4);
    ~PasswordHashService();

    PasswordHashService(const PasswordHashService&) = delete;
    PasswordHashService& operator=(const PasswordHashService&) = delete;

    // Returns false when the bounded queue is full or the pool is stopping.
    bool hash_async(std::string password, HashCallback callback);
    bool verify_async(std::string password, AuthRecord record, VerifyCallback callback);

    std::size_t pending() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace edgedesk
