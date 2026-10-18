#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <zlib.h>

// Minimal ZIP writer for tests. Produces stored or raw-deflate entries, with
// optional Unix mode attributes so the reader's symlink rejection is testable.

namespace edgedesk::test {

struct ZipItem {
    std::string name;
    std::string content;
    std::uint32_t mode = 0; // Unix st_mode, 0 = unspecified
    bool deflate = false;
    std::string local_name; // overrides the local header name when set
};

inline void put_u16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

inline void put_u32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

inline std::string deflate_raw(const std::string& data) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    std::string out;
    out.resize(static_cast<std::size_t>(compressBound(static_cast<uLong>(data.size()))) + 16);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&stream, Z_FINISH);
    const std::size_t produced = out.size() - stream.avail_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END) return {};
    out.resize(produced);
    return out;
}

inline bool write_zip(const std::string& path, const std::vector<ZipItem>& items) {
    struct Central {
        ZipItem item;
        std::uint32_t crc = 0;
        std::uint64_t compressed = 0;
        std::uint64_t uncompressed = 0;
        std::uint32_t offset = 0;
    };
    std::vector<Central> central;
    std::string body;
    for (const auto& item : items) {
        const bool directory = !item.name.empty() && item.name.back() == '/';
        const std::string stored = item.deflate && !directory ? deflate_raw(item.content) : item.content;
        Central record;
        record.item = item;
        record.crc = static_cast<std::uint32_t>(
            crc32(0L, reinterpret_cast<const Bytef*>(item.content.data()), static_cast<uInt>(item.content.size())));
        record.compressed = stored.size();
        record.uncompressed = item.content.size();
        record.offset = static_cast<std::uint32_t>(body.size());
        const std::uint16_t method = item.deflate && !directory ? 8 : 0;
        put_u32(body, 0x04034b50u);
        put_u16(body, 20);
        put_u16(body, 0x0800); // UTF-8 names
        put_u16(body, method);
        put_u16(body, 0);
        put_u16(body, 0);
        put_u32(body, record.crc);
        put_u32(body, static_cast<std::uint32_t>(stored.size()));
        put_u32(body, static_cast<std::uint32_t>(item.content.size()));
        const std::string& local_name = item.local_name.empty() ? item.name : item.local_name;
        put_u16(body, static_cast<std::uint16_t>(local_name.size()));
        put_u16(body, 0);
        body += local_name;
        body += stored;
        central.push_back(std::move(record));
    }
    const std::uint32_t directory_offset = static_cast<std::uint32_t>(body.size());
    for (const auto& record : central) {
        put_u32(body, 0x02014b50u);
        put_u16(body, 0x031e); // made by Unix
        put_u16(body, 20);
        put_u16(body, 0x0800);
        put_u16(body, record.item.deflate ? 8 : 0);
        put_u16(body, 0);
        put_u16(body, 0);
        put_u32(body, record.crc);
        put_u32(body, static_cast<std::uint32_t>(record.compressed));
        put_u32(body, static_cast<std::uint32_t>(record.uncompressed));
        put_u16(body, static_cast<std::uint16_t>(record.item.name.size()));
        put_u16(body, 0);
        put_u16(body, 0);
        put_u16(body, 0);
        put_u16(body, 0);
        put_u32(body, record.item.mode != 0 ? (record.item.mode << 16) : 0);
        put_u32(body, record.offset);
        body += record.item.name;
    }
    const std::uint32_t directory_size = static_cast<std::uint32_t>(body.size()) - directory_offset;
    put_u32(body, 0x06054b50u);
    put_u16(body, 0);
    put_u16(body, 0);
    put_u16(body, static_cast<std::uint16_t>(central.size()));
    put_u16(body, static_cast<std::uint16_t>(central.size()));
    put_u32(body, directory_size);
    put_u32(body, directory_offset);
    put_u16(body, 0);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(file);
}

} // namespace edgedesk::test
