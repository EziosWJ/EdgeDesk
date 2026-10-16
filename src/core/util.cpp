#include "core/util.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(__linux__)
#include <sys/random.h>
#include <unistd.h>
#endif

namespace edgedesk {
namespace {

inline std::uint32_t rotr32(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

const std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

void sha256_transform(std::uint32_t state[8], const unsigned char block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const auto s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const auto s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const auto ch = (e & f) ^ ((~e) & g);
        const auto temp1 = h + s1 + ch + kSha256K[i] + w[i];
        const auto s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const auto maj = (a & b) ^ (a & c) ^ (b & c);
        const auto temp2 = s0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

std::array<unsigned char, 32> sha256_digest(std::string_view data) {
    std::uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const auto length = data.size();
    std::size_t offset = 0;
    unsigned char block[64];
    while (offset + 64 <= length) {
        std::memcpy(block, data.data() + offset, 64);
        sha256_transform(state, block);
        offset += 64;
    }
    unsigned char tail[128];
    std::memset(tail, 0, sizeof(tail));
    const auto remaining = length - offset;
    if (remaining > 0) std::memcpy(tail, data.data() + offset, remaining);
    tail[remaining] = 0x80;
    const auto tail_size = remaining < 56 ? std::size_t{64} : std::size_t{128};
    const std::uint64_t bit_length = static_cast<std::uint64_t>(length) * 8u;
    for (int i = 0; i < 8; ++i) {
        tail[tail_size - 1 - i] = static_cast<unsigned char>((bit_length >> (i * 8)) & 0xffu);
    }
    for (std::size_t pos = 0; pos < tail_size; pos += 64) {
        sha256_transform(state, tail + pos);
    }
    std::array<unsigned char, 32> digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<unsigned char>((state[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = static_cast<unsigned char>((state[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = static_cast<unsigned char>((state[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = static_cast<unsigned char>(state[i] & 0xffu);
    }
    return digest;
}

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr char kHexUpper[] = "0123456789ABCDEF";

} // namespace

std::string hex_encode(std::string_view bytes) {
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto value = static_cast<unsigned char>(bytes[i]);
        out[i * 2] = kHexDigits[value >> 4];
        out[i * 2 + 1] = kHexDigits[value & 0x0f];
    }
    return out;
}

bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
}

std::string sha256_hex(std::string_view data) {
    const auto digest = sha256_digest(data);
    return hex_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

std::string hmac_sha256_hex(std::string_view key, std::string_view data) {
    constexpr std::size_t kBlockSize = 64;
    unsigned char key_block[kBlockSize];
    std::memset(key_block, 0, sizeof(key_block));
    if (key.size() > kBlockSize) {
        const auto digest = sha256_digest(key);
        std::memcpy(key_block, digest.data(), digest.size());
    } else if (!key.empty()) {
        std::memcpy(key_block, key.data(), key.size());
    }
    std::string inner;
    inner.reserve(kBlockSize + data.size());
    for (std::size_t i = 0; i < kBlockSize; ++i) {
        inner.push_back(static_cast<char>(key_block[i] ^ 0x36));
    }
    inner.append(data.data(), data.size());
    const auto inner_digest = sha256_digest(inner);

    std::string outer;
    outer.reserve(kBlockSize + inner_digest.size());
    for (std::size_t i = 0; i < kBlockSize; ++i) {
        outer.push_back(static_cast<char>(key_block[i] ^ 0x5c));
    }
    outer.append(reinterpret_cast<const char*>(inner_digest.data()), inner_digest.size());
    const auto digest = sha256_digest(outer);
    return hex_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

std::string random_bytes(std::size_t count) {
    std::string out;
    out.resize(count);
    if (count == 0) return out;
#if defined(__linux__)
    std::size_t filled = 0;
    while (filled < count) {
        const auto chunk = ::getrandom(out.data() + filled, count - filled, 0);
        if (chunk > 0) {
            filled += static_cast<std::size_t>(chunk);
            continue;
        }
        if (chunk < 0 && errno == EINTR) continue;
        break;
    }
    if (filled == count) return out;
    // Fallback: /dev/urandom (older kernels or blocked getrandom).
    std::FILE* file = std::fopen("/dev/urandom", "rb");
    if (file == nullptr) return {};
    const auto read = std::fread(out.data(), 1, count, file);
    std::fclose(file);
    if (read != count) return {};
    return out;
#else
    std::FILE* file = std::fopen("/dev/urandom", "rb");
    if (file == nullptr) return {};
    const auto read = std::fread(out.data(), 1, count, file);
    std::fclose(file);
    if (read != count) return {};
    return out;
#endif
}

std::string random_hex(std::size_t byte_count) {
    const auto bytes = random_bytes(byte_count);
    if (bytes.empty()) return {};
    return hex_encode(bytes);
}

std::int64_t now_unix_seconds() {
    return static_cast<std::int64_t>(::time(nullptr));
}

std::int64_t now_unix_millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::int64_t steady_millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string format_iso8601_utc(std::int64_t unix_seconds) {
    std::tm tm{};
    const std::time_t stamp = static_cast<std::time_t>(unix_seconds);
#if defined(_WIN32)
    gmtime_s(&tm, &stamp);
#else
    if (::gmtime_r(&stamp, &tm) == nullptr) return {};
#endif
    char buffer[32];
    const auto written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    if (written == 0) return {};
    return std::string(buffer, written);
}

bool url_decode(std::string_view text, std::string& out) {
    out.clear();
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '%') {
            if (i + 2 >= text.size()) return false;
            const auto hex_value = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int high = hex_value(text[i + 1]);
            const int low = hex_value(text[i + 2]);
            if (high < 0 || low < 0) return false;
            out.push_back(static_cast<char>((high << 4) | low));
            i += 2;
        } else if (ch == '+') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return true;
}

std::string url_encode_component(std::string_view text) {
    std::string out;
    out.reserve(text.size() * 3);
    for (const char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                                byte == '.' || byte == '~';
        if (unreserved) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHexUpper[byte >> 4]);
            out.push_back(kHexUpper[byte & 0x0f]);
        }
    }
    return out;
}

} // namespace edgedesk
