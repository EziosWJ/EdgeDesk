#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace edgedesk {

using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;
using Milliseconds = std::chrono::milliseconds;

// Byte size helpers keep unit conversions in one place.
constexpr std::uint64_t kKiB = 1024ull;
constexpr std::uint64_t kMiB = 1024ull * kKiB;
constexpr std::uint64_t kGiB = 1024ull * kMiB;

// Parses "4096", "256K", "16M", "2G" (case-insensitive suffix).
bool parse_byte_size(std::string_view text, std::uint64_t& out);

// Parses a non-negative decimal integer; rejects empty and trailing garbage.
bool parse_uint64(std::string_view text, std::uint64_t& out);

// True when host is a loopback literal (127.0.0.1, ::1, localhost variants).
bool is_loopback_host(std::string_view host);

std::string trim(std::string_view text);
std::vector<std::string> split(std::string_view text, char separator);
std::string to_lower_ascii(std::string_view text);

// Pseudo file system entries that must never be served through the file API.
bool is_reserved_dir_name(std::string_view name);

} // namespace edgedesk
