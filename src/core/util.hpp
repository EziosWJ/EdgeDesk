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
std::string sha256_hex(std::string_view data);
std::string hmac_sha256_hex(std::string_view key, std::string_view data);

std::int64_t now_unix_seconds();
std::int64_t now_unix_millis();
std::int64_t steady_millis();

// "2026-10-16T12:34:56Z" (UTC)
std::string format_iso8601_utc(std::int64_t unix_seconds);

// Percent-decodes a URL component. Returns false on malformed input.
bool url_decode(std::string_view text, std::string& out);
std::string url_encode_component(std::string_view text);

} // namespace edgedesk
