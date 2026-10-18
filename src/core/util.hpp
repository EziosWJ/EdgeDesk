#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Small dependency-free utilities: randomness, hashing, time formatting.
// No libhv / POSIX-specific types leak through this header.

namespace edgedesk {

// Cryptographically random bytes; empty string on failure (callers must treat
// that as fatal for token generation).
std::string random_bytes(std::size_t count);
std::string random_hex(std::size_t byte_count);

std::string hex_encode(std::string_view bytes);
bool constant_time_equal(std::string_view a, std::string_view b);

// Self-contained SHA-256 / HMAC-SHA256 (RFC 6234 / RFC 2104).
//
// Sha256 supports streaming updates so package installation can hash each
// extracted file without buffering it in memory.
class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t length);
    void update(std::string_view data) { update(data.data(), data.size()); }
    // Returns lowercase hex. The object must not be updated afterwards.
    std::string finish_hex();

private:
    std::uint32_t state_[8];
    unsigned char block_[64];
    std::size_t block_size_ = 0;
    std::uint64_t length_ = 0;
    bool finished_ = false;
};

std::string sha256_hex(std::string_view data);
std::string hmac_sha256_hex(std::string_view key, std::string_view data);

// Standard and URL-safe (unpadded) Base64. Decoding accepts both alphabets and
// optional padding; returns false on malformed input.
std::string base64_encode(std::string_view data);
std::string base64url_encode(std::string_view data);
bool base64_decode(std::string_view text, std::string& out);

std::int64_t now_unix_seconds();
std::int64_t now_unix_millis();
std::int64_t steady_millis();

// "2026-10-16T12:34:56Z" (UTC)
std::string format_iso8601_utc(std::int64_t unix_seconds);
// Strict parser for the format produced by format_iso8601_utc.
bool parse_iso8601_utc(std::string_view text, std::int64_t& unix_seconds);

// Percent-decodes a URL component. Returns false on malformed input.
bool url_decode(std::string_view text, std::string& out);
std::string url_encode_component(std::string_view text);

} // namespace edgedesk
