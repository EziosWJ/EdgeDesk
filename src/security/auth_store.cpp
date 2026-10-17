#include "security/auth_store.hpp"

#include "core/atomic_file.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace edgedesk {
namespace {

constexpr const char* kAuthFile = "auth.json";
constexpr const char* kLegacyInitCodeFile = "init-code";
constexpr const char* kLockFile = ".lock";
constexpr std::size_t kMaxAuthFileBytes = 64 * 1024;

bool is_hex(std::string_view text, std::size_t expected_bytes) {
    if (text.size() != expected_bytes * 2) return false;
    for (const char ch : text) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
}

bool parse_record(const std::string& text, AuthRecord& record, std::string& error) {
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error = "auth.json is not a JSON object";
        return false;
    }
    for (const auto& item : parsed.items()) {
        const auto& key = item.key();
        if (key != "schema" && key != "algorithm" && key != "params" && key != "salt" && key != "hash" &&
            key != "credential_version" && key != "created_at") {
            error = "auth.json contains an unknown field: " + key;
            return false;
        }
    }
    if (!parsed.contains("schema") || !parsed["schema"].is_number_integer() || parsed["schema"].get<int>() != 1) {
        error = "auth.json schema is not supported";
        return false;
    }
    if (!parsed.contains("algorithm") || !parsed["algorithm"].is_string() ||
        parsed["algorithm"].get<std::string>() != "argon2id") {
        error = "auth.json algorithm is not supported";
        return false;
    }
    if (!parsed.contains("params") || !parsed["params"].is_object()) {
        error = "auth.json params are missing";
        return false;
    }
    const auto& params = parsed["params"];
    for (const auto& item : params.items()) {
        if (item.key() != "m" && item.key() != "t" && item.key() != "p") {
            error = "auth.json contains an unknown hash parameter";
            return false;
        }
    }
    if (!params.contains("m") || !params["m"].is_number_unsigned() || !params.contains("t") ||
        !params["t"].is_number_unsigned() || !params.contains("p") || !params["p"].is_number_unsigned()) {
        error = "auth.json hash parameters are invalid";
        return false;
    }
    const auto memory = params["m"].get<std::uint64_t>();
    const auto iterations = params["t"].get<std::uint64_t>();
    const auto parallelism = params["p"].get<std::uint64_t>();
    if (memory < 8 || memory > 1024 * 1024 || iterations < 1 || iterations > 64 || parallelism < 1 ||
        parallelism > 16) {
        error = "auth.json hash parameters are out of range";
        return false;
    }
    if (!parsed.contains("salt") || !parsed["salt"].is_string() ||
        !is_hex(parsed["salt"].get<std::string>(), 16)) {
        error = "auth.json salt is invalid";
        return false;
    }
    if (!parsed.contains("hash") || !parsed["hash"].is_string() ||
        !is_hex(parsed["hash"].get<std::string>(), 32)) {
        error = "auth.json hash is invalid";
        return false;
    }
    if (!parsed.contains("credential_version") || !parsed["credential_version"].is_number_unsigned() ||
        parsed["credential_version"].get<std::uint64_t>() == 0) {
        error = "auth.json credential version is invalid";
        return false;
    }
    if (!parsed.contains("created_at") || !parsed["created_at"].is_number_integer()) {
        error = "auth.json created_at is invalid";
        return false;
    }

    record.schema = 1;
    record.algorithm = "argon2id";
    record.memory_kib = static_cast<std::uint32_t>(memory);
    record.iterations = static_cast<std::uint32_t>(iterations);
    record.parallelism = static_cast<std::uint32_t>(parallelism);
    record.salt_hex = parsed["salt"].get<std::string>();
    record.hash_hex = parsed["hash"].get<std::string>();
    record.credential_version = parsed["credential_version"].get<std::uint64_t>();
    record.created_at = parsed["created_at"].get<std::int64_t>();
    error.clear();
    return true;
}

std::string serialize_record(const AuthRecord& record) {
    nlohmann::json params;
    params["m"] = record.memory_kib;
    params["t"] = record.iterations;
    params["p"] = record.parallelism;
    nlohmann::json body;
    body["schema"] = record.schema;
    body["algorithm"] = record.algorithm;
    body["params"] = std::move(params);
    body["salt"] = record.salt_hex;
    body["hash"] = record.hash_hex;
    body["credential_version"] = record.credential_version;
    body["created_at"] = record.created_at;
    return body.dump();
}

} // namespace

const char* auth_state_name(AuthState state) {
    switch (state) {
        case AuthState::uninitialized: return "uninitialized";
        case AuthState::initialized: return "initialized";
        case AuthState::corrupt: return "corrupt";
    }
    return "corrupt";
}

AuthStore::AuthStore(UniqueFd dir_fd, UniqueFd lock_fd, std::string data_dir) noexcept
    : dir_fd_(std::move(dir_fd)), lock_fd_(std::move(lock_fd)), data_dir_(std::move(data_dir)) {}

std::unique_ptr<AuthStore> AuthStore::open(const std::string& data_dir, bool take_lock, std::string& error) {
    auto dir_fd = open_private_dir(data_dir, error);
    if (!dir_fd.valid()) return nullptr;

    UniqueFd lock_fd;
    if (take_lock) {
        lock_fd.reset(::openat(dir_fd.get(), kLockFile, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (!lock_fd.valid()) {
            error = "cannot open auth lock file: " + std::string(std::strerror(errno));
            return nullptr;
        }
        if (::flock(lock_fd.get(), LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                error = "another EdgeDesk process is using the data directory (stop it before recovery)";
            } else {
                error = "cannot lock auth state: " + std::string(std::strerror(errno));
            }
            return nullptr;
        }
    }

    auto store = std::unique_ptr<AuthStore>(new AuthStore(std::move(dir_fd), std::move(lock_fd), data_dir));
    if (!store->reload(error)) {
        // Corruption is a valid state, not an open failure. Only I/O errors that
        // prevent reading the directory itself should fail startup.
        if (store->state_ != AuthState::corrupt) return nullptr;
    }
    return store;
}

bool AuthStore::reload(std::string& error) {
    std::string text;
    bool missing = false;
    if (!read_file_at(dir_fd_.get(), kAuthFile, kMaxAuthFileBytes, text, missing, error)) {
        state_ = AuthState::corrupt;
        last_error_ = error;
        return false;
    }
    if (missing) {
        // Older builds stored a one-time code here; initialization no longer
        // requires it, so remove stale files when possible.
        std::string remove_error;
        remove_file_at(dir_fd_.get(), kLegacyInitCodeFile, remove_error);
        state_ = AuthState::uninitialized;
        last_error_.clear();
        return true;
    }
    AuthRecord record;
    if (!parse_record(text, record, error)) {
        state_ = AuthState::corrupt;
        last_error_ = error;
        return false;
    }
    state_ = AuthState::initialized;
    last_error_.clear();
    return true;
}

bool AuthStore::read_record(AuthRecord& record, std::string& error) const {
    if (state_ != AuthState::initialized) {
        error = "authentication is not initialized";
        return false;
    }
    std::string text;
    bool missing = false;
    if (!read_file_at(dir_fd_.get(), kAuthFile, kMaxAuthFileBytes, text, missing, error)) return false;
    if (missing) {
        error = "auth.json disappeared";
        return false;
    }
    return parse_record(text, record, error);
}

bool AuthStore::save_record(const AuthRecord& record, std::string& error) {
    if (!atomic_write_file_at(dir_fd_.get(), kAuthFile, serialize_record(record), 0600, error)) {
        return false;
    }
    state_ = AuthState::initialized;
    last_error_.clear();
    return true;
}

bool AuthStore::reset_for_recovery(std::string& error) {
    if (!remove_file_at(dir_fd_.get(), kAuthFile, error)) return false;
    // Best-effort cleanup of a legacy one-time code from older builds.
    std::string remove_error;
    remove_file_at(dir_fd_.get(), kLegacyInitCodeFile, remove_error);
    state_ = AuthState::uninitialized;
    last_error_.clear();
    return true;
}

} // namespace edgedesk
