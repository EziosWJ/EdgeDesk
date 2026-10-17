#include "security/password_hash.hpp"

#include "core/util.hpp"

#include <argon2.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace edgedesk {
namespace {

bool decode_hex(std::string_view text, std::string& out) {
    if (text.size() % 2 != 0) return false;
    out.resize(text.size() / 2);
    for (std::size_t index = 0; index < out.size(); ++index) {
        const auto byte = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        const int high = byte(text[index * 2]);
        const int low = byte(text[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        out[index] = static_cast<char>((high << 4) | low);
    }
    return true;
}

// Strict UTF-8 decoder. Returns false for overlong forms, surrogates and
// codepoints above U+10FFFF. Calls `accept` for each decoded codepoint.
template <typename Accept>
bool decode_utf8(std::string_view text, Accept accept, std::size_t& codepoints) {
    codepoints = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::uint32_t value = 0;
        std::size_t length = 0;
        if (first < 0x80) {
            value = first;
            length = 1;
        } else if ((first & 0xE0) == 0xC0) {
            value = first & 0x1F;
            length = 2;
        } else if ((first & 0xF0) == 0xE0) {
            value = first & 0x0F;
            length = 3;
        } else if ((first & 0xF8) == 0xF0) {
            value = first & 0x07;
            length = 4;
        } else {
            return false;
        }
        if (index + length > text.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0) != 0x80) return false;
            value = (value << 6) | (next & 0x3F);
        }
        if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
            (length == 4 && value < 0x10000) || value > 0x10FFFF ||
            (value >= 0xD800 && value <= 0xDFFF)) {
            return false;
        }
        if (!accept(value)) return false;
        ++codepoints;
        index += length;
    }
    return true;
}

} // namespace

bool validate_password(std::string_view password, std::string& error) {
    if (password.size() > kPasswordMaxBytes) {
        error = "password must not exceed 1024 UTF-8 bytes";
        return false;
    }
    std::size_t codepoints = 0;
    const bool valid = decode_utf8(
        password,
        [](std::uint32_t value) {
            // Printable Unicode only: reject C0/C1 controls and DEL. Space is
            // explicitly allowed; tabs and newlines are not.
            return value >= 0x20 && !(value >= 0x7F && value <= 0x9F);
        },
        codepoints);
    if (!valid) {
        error = "password must be valid printable UTF-8 text";
        return false;
    }
    if (codepoints < kPasswordMinCodepoints) {
        error = "password must contain at least 12 characters";
        return false;
    }
    error.clear();
    return true;
}

bool hash_password(std::string_view password, std::uint32_t memory_kib, std::uint32_t iterations,
                   std::uint32_t parallelism, std::string& salt_hex, std::string& hash_hex, Error& error) {
    if (memory_kib < 8 || iterations < 1 || parallelism < 1 ||
        parallelism > static_cast<std::uint32_t>(std::thread::hardware_concurrency() == 0
                                                     ? 1
                                                     : std::thread::hardware_concurrency())) {
        error = Error::make(ErrorCode::invalid_argument, "invalid password hash parameters");
        return false;
    }
    const std::string salt = random_bytes(16);
    if (salt.size() != 16) {
        error = Error::make(ErrorCode::io_error, "cannot obtain entropy for password salt");
        return false;
    }
    std::string hash(32, '\0');
    const int result = argon2id_hash_raw(iterations, memory_kib, parallelism, password.data(), password.size(),
                                         salt.data(), salt.size(), hash.data(), hash.size());
    if (result != ARGON2_OK) {
        error = Error::make(ErrorCode::io_error, std::string("argon2id failed: ") + argon2_error_message(result));
        return false;
    }
    salt_hex = hex_encode(salt);
    hash_hex = hex_encode(hash);
    error = Error::none();
    return true;
}

bool verify_password(std::string_view password, const AuthRecord& record, bool& matches, Error& error) {
    matches = false;
    std::string salt;
    std::string expected;
    if (!decode_hex(record.salt_hex, salt) || salt.size() != 16 ||
        !decode_hex(record.hash_hex, expected) || expected.size() != 32) {
        error = Error::make(ErrorCode::io_error, "stored password hash is malformed");
        return false;
    }
    std::string actual(32, '\0');
    const int result = argon2id_hash_raw(record.iterations, record.memory_kib, record.parallelism,
                                         password.data(), password.size(), salt.data(), salt.size(),
                                         actual.data(), actual.size());
    if (result != ARGON2_OK) {
        error = Error::make(ErrorCode::io_error, std::string("argon2id failed: ") + argon2_error_message(result));
        return false;
    }
    matches = constant_time_equal(actual, expected);
    error = Error::none();
    return true;
}

bool password_needs_rehash(const AuthRecord& record) {
    return record.memory_kib != kPasswordMemoryKiB || record.iterations != kPasswordIterations ||
           record.parallelism != kPasswordParallelism;
}

struct PasswordHashService::Impl {
    int worker_count = 1;
    std::size_t queue_limit = 4;
    mutable std::mutex mutex;
    std::condition_variable wakeup;
    std::deque<std::function<void()>> queue;
    std::vector<std::thread> workers;
    bool stopping = false;

    explicit Impl(int count, std::size_t limit) : worker_count(std::max(1, count)), queue_limit(std::max<std::size_t>(1, limit)) {
        workers.reserve(static_cast<std::size_t>(worker_count));
        for (int index = 0; index < worker_count; ++index) {
            workers.emplace_back([this] { run(); });
        }
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            // Drop queued jobs on shutdown; a job already running is allowed
            // to finish so its password buffers are cleared normally.
            queue.clear();
        }
        wakeup.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }

    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wakeup.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            try {
                job();
            } catch (...) {
                // A callback must never terminate the process. Route callbacks
                // are expected to write an error response themselves.
            }
        }
    }

    bool submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || queue.size() >= queue_limit) return false;
            queue.push_back(std::move(job));
        }
        wakeup.notify_one();
        return true;
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }
};

PasswordHashService::PasswordHashService(int workers, std::size_t queue_limit)
    : impl_(std::make_unique<Impl>(workers, queue_limit)) {}

PasswordHashService::~PasswordHashService() = default;

bool PasswordHashService::hash_async(std::string password, HashCallback callback) {
    return impl_->submit([this, password = std::move(password), callback = std::move(callback)]() mutable {
        std::string salt;
        std::string hash;
        Error error;
        const bool ok = hash_password(password, kPasswordMemoryKiB, kPasswordIterations, kPasswordParallelism,
                                      salt, hash, error);
        if (callback) callback(ok ? Error::none() : error, std::move(salt), std::move(hash));
    });
}

bool PasswordHashService::verify_async(std::string password, AuthRecord record, VerifyCallback callback) {
    return impl_->submit([password = std::move(password), record = std::move(record),
                          callback = std::move(callback)]() mutable {
        bool matches = false;
        Error error;
        const bool ok = verify_password(password, record, matches, error);
        if (callback) callback(ok ? Error::none() : error, matches);
    });
}

std::size_t PasswordHashService::pending() const { return impl_->pending(); }

} // namespace edgedesk
