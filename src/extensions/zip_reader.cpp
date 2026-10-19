#include "extensions/zip_reader.hpp"

#include "core/fd.hpp"
#include "core/utf8.hpp"
#include "core/util.hpp"
#include "file/file_types.hpp"
#include "file/path_sandbox.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <zlib.h>

namespace edgedesk::extensions {
namespace {

constexpr std::uint32_t kEocdSignature = 0x06054b50u;
constexpr std::uint32_t kEocd64Signature = 0x06064b50u;
constexpr std::uint32_t kEocd64LocatorSignature = 0x07064b50u;
constexpr std::uint32_t kCentralSignature = 0x02014b50u;
constexpr std::uint32_t kLocalSignature = 0x04034b50u;
constexpr std::size_t kCopyChunk = 64 * 1024;
constexpr std::size_t kInflateChunk = 256 * 1024;
constexpr std::uint64_t kDiskSafetyMargin = 1024 * 1024;

std::uint16_t read_u16(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0] | (static_cast<std::uint16_t>(data[1]) << 8));
}

std::uint32_t read_u32(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t read_u64(const unsigned char* data) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | data[i];
    return value;
}

Error io_error(const std::string& context, int code = errno) {
    switch (code) {
        case ENOENT:
            return Error::make(ErrorCode::not_found, context, code);
        case EACCES:
        case EPERM:
        case EROFS:
            return Error::make(ErrorCode::permission_denied, context + ": " + std::strerror(code), code);
        case ENOSPC:
        case EDQUOT:
            return Error::make(ErrorCode::too_large, context + " (no space left)", code);
        case EEXIST:
            return Error::make(ErrorCode::already_exists, context, code);
        case ELOOP:
            return Error::make(ErrorCode::permission_denied, context + " (symlink rejected)", code);
        default:
            return Error::make(ErrorCode::io_error, context + ": " + std::strerror(code), code);
    }
}

bool pread_full(int fd, void* buffer, std::size_t length, std::uint64_t offset) {
    auto* out = static_cast<unsigned char*>(buffer);
    std::size_t done = 0;
    while (done < length) {
        const auto read = ::pread(fd, out + done, length - done, static_cast<off_t>(offset + done));
        if (read < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (read == 0) return false;
        done += static_cast<std::size_t>(read);
    }
    return true;
}

bool write_full(int fd, const void* buffer, std::size_t length) {
    const auto* data = static_cast<const unsigned char*>(buffer);
    std::size_t done = 0;
    while (done < length) {
        const auto written = ::write(fd, data + done, length - done);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        done += static_cast<std::size_t>(written);
    }
    return true;
}

bool parse_zip64_extra(const unsigned char* extra, std::size_t size, std::uint64_t& uncompressed,
                       std::uint64_t& compressed, std::uint64_t& local_offset) {
    std::size_t offset = 0;
    while (offset + 4 <= size) {
        const std::uint16_t tag = read_u16(extra + offset);
        const std::uint16_t length = read_u16(extra + offset + 2);
        offset += 4;
        if (offset + length > size) return false;
        if (tag == 0x0001) {
            std::size_t cursor = offset;
            const std::size_t end = offset + length;
            if (uncompressed == 0xFFFFFFFFull) {
                if (cursor + 8 > end) return false;
                uncompressed = read_u64(extra + cursor);
                cursor += 8;
            }
            if (compressed == 0xFFFFFFFFull) {
                if (cursor + 8 > end) return false;
                compressed = read_u64(extra + cursor);
                cursor += 8;
            }
            if (local_offset == 0xFFFFFFFFull) {
                if (cursor + 8 > end) return false;
                local_offset = read_u64(extra + cursor);
            }
            return true;
        }
        offset += length;
    }
    return true;
}

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

bool unix_mode_is_link(std::uint32_t attributes) {
    const std::uint32_t mode = attributes >> 16;
    if (mode == 0) return false;
    const std::uint32_t type = mode & S_IFMT;
    return type == S_IFLNK;
}

bool unix_mode_is_supported(std::uint32_t attributes) {
    const std::uint32_t mode = attributes >> 16;
    if (mode == 0) return true;
    const std::uint32_t type = mode & S_IFMT;
    return type == 0 || type == S_IFREG || type == S_IFDIR;
}

Error ensure_parent_dir(int root_fd, const std::string& relative, UniqueFd& out_parent,
                        std::string& out_name) {
    const auto slash = relative.rfind('/');
    out_name = slash == std::string::npos ? relative : relative.substr(slash + 1);
    if (slash == std::string::npos) {
        out_parent.reset(duplicate_fd(root_fd));
        if (!out_parent.valid()) return io_error("duplicate directory descriptor");
        return Error::none();
    }
    std::string current;
    std::size_t start = 0;
    while (start < slash) {
        const auto next = relative.find('/', start);
        const std::string part = relative.substr(start, next - start);
        if (!current.empty()) current.push_back('/');
        current += part;
        int next_fd = ::openat(out_parent.valid() ? out_parent.get() : root_fd, part.c_str(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next_fd < 0) {
            if (errno != ENOENT) return io_error("open directory " + current);
            if (::mkdirat(out_parent.valid() ? out_parent.get() : root_fd, part.c_str(), 0700) != 0 &&
                errno != EEXIST) {
                return io_error("create directory " + current);
            }
            next_fd = ::openat(out_parent.valid() ? out_parent.get() : root_fd, part.c_str(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next_fd < 0) return io_error("open created directory " + current);
        }
        out_parent.reset(next_fd);
        start = next + 1;
    }
    return Error::none();
}

} // namespace

bool validate_entry_name(std::string_view name, std::string& normalized, bool& directory,
                         std::string& error) {
    normalized.clear();
    directory = false;
    if (name.empty()) {
        error = "empty ZIP entry name";
        return false;
    }
    if (name.size() > 4096) {
        error = "ZIP entry name is too long";
        return false;
    }
    if (name.front() == '/') {
        error = "absolute ZIP entry path rejected";
        return false;
    }
    if (name.find('\\') != std::string_view::npos) {
        error = "backslash in ZIP entry name rejected";
        return false;
    }
    for (const char ch : name) {
        const auto value = static_cast<unsigned char>(ch);
        if (value == 0 || value < 0x20 || value == 0x7f) {
            error = "control character in ZIP entry name rejected";
            return false;
        }
    }
    if (!is_valid_utf8(name)) {
        error = "ZIP entry name is not valid UTF-8";
        return false;
    }
    std::string_view body = name;
    if (body.back() == '/') {
        directory = true;
        body.remove_suffix(1);
    }
    if (body.empty()) {
        error = "empty ZIP entry path";
        return false;
    }
    std::size_t start = 0;
    for (;;) {
        const auto slash = body.find('/', start);
        const auto part = body.substr(start, slash == std::string_view::npos ? body.size() - start : slash - start);
        if (part.empty()) {
            error = "empty path component in ZIP entry";
            return false;
        }
        if (part == "." || part == "..") {
            error = "path traversal in ZIP entry rejected";
            return false;
        }
        if (!normalized.empty()) normalized.push_back('/');
        normalized.append(part);
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    error.clear();
    return true;
}

std::unique_ptr<ZipReader> ZipReader::open(const std::string& path, Error& error) {
    UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!fd.valid()) {
        error = io_error("open application package " + path);
        return nullptr;
    }
    struct stat info {};
    if (::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
        error = Error::make(ErrorCode::invalid_argument, "application package must be a regular file");
        return nullptr;
    }
    auto reader = std::unique_ptr<ZipReader>(new ZipReader(path, static_cast<std::uint64_t>(info.st_size)));
    error = reader->load_entries(fd.get());
    if (!error.ok()) return nullptr;
    return reader;
}

Error ZipReader::load_entries(int fd) {
    if (file_size_ < 22) return Error::make(ErrorCode::invalid_argument, "not a ZIP archive (too short)");
    const std::uint64_t tail_size = std::min<std::uint64_t>(file_size_, 22 + 65535 + 20);
    std::vector<unsigned char> tail(static_cast<std::size_t>(tail_size));
    if (!pread_full(fd, tail.data(), tail.size(), file_size_ - tail_size)) {
        return io_error("read ZIP end record");
    }
    std::int64_t eocd_index = -1;
    for (std::int64_t i = static_cast<std::int64_t>(tail.size()) - 22; i >= 0; --i) {
        if (read_u32(tail.data() + i) == kEocdSignature) {
            const std::uint16_t comment = read_u16(tail.data() + i + 20);
            if (static_cast<std::uint64_t>(i) + 22 + comment <= tail.size()) {
                eocd_index = i;
                break;
            }
        }
    }
    if (eocd_index < 0) return Error::make(ErrorCode::invalid_argument, "ZIP end record not found");
    const unsigned char* eocd = tail.data() + eocd_index;
    const std::uint16_t disk = read_u16(eocd + 4);
    const std::uint16_t cd_disk = read_u16(eocd + 6);
    std::uint64_t entry_count = read_u16(eocd + 10);
    std::uint64_t cd_size = read_u32(eocd + 12);
    std::uint64_t cd_offset = read_u32(eocd + 16);
    if (disk != 0 || cd_disk != 0 || read_u16(eocd + 8) != read_u16(eocd + 10)) {
        return Error::make(ErrorCode::unsupported, "multi-disk ZIP archives are not supported");
    }
    const std::uint64_t eocd_absolute = file_size_ - tail_size + static_cast<std::uint64_t>(eocd_index);
    const bool needs_zip64 = entry_count == 0xFFFFu || cd_size == 0xFFFFFFFFull || cd_offset == 0xFFFFFFFFull;
    if (needs_zip64) {
        if (eocd_absolute < 20) return Error::make(ErrorCode::invalid_argument, "ZIP64 locator missing");
        unsigned char locator[20];
        if (!pread_full(fd, locator, sizeof(locator), eocd_absolute - 20)) {
            return io_error("read ZIP64 locator");
        }
        if (read_u32(locator) != kEocd64LocatorSignature) {
            return Error::make(ErrorCode::invalid_argument, "invalid ZIP64 locator");
        }
        if (read_u32(locator + 4) != 0 || read_u32(locator + 16) != 1) {
            return Error::make(ErrorCode::unsupported, "multi-disk ZIP64 archives are not supported");
        }
        const std::uint64_t record_offset = read_u64(locator + 8);
        unsigned char record[56];
        if (record_offset > file_size_ || file_size_ - record_offset < sizeof(record) ||
            !pread_full(fd, record, sizeof(record), record_offset)) {
            return Error::make(ErrorCode::invalid_argument, "invalid ZIP64 end record");
        }
        if (read_u32(record) != kEocd64Signature) {
            return Error::make(ErrorCode::invalid_argument, "invalid ZIP64 end record signature");
        }
        const std::uint64_t record_size = read_u64(record + 4);
        if (record_size < 44 || record_offset + 12 + record_size > file_size_) {
            return Error::make(ErrorCode::invalid_argument, "invalid ZIP64 end record size");
        }
        if (read_u32(record + 16) != 0 || read_u32(record + 20) != 0) {
            return Error::make(ErrorCode::unsupported, "multi-disk ZIP64 archives are not supported");
        }
        entry_count = read_u64(record + 32);
        cd_size = read_u64(record + 40);
        cd_offset = read_u64(record + 48);
    }
    if (cd_offset > file_size_ || cd_size > file_size_ - cd_offset) {
        return Error::make(ErrorCode::invalid_argument, "ZIP central directory lies outside the file");
    }

    std::set<std::string> seen_exact;
    std::set<std::string> seen_folded;
    std::uint64_t offset = cd_offset;
    const std::uint64_t cd_end = cd_offset + cd_size;
    entries_.clear();
    for (std::uint64_t index = 0; index < entry_count; ++index) {
        if (offset + 46 > cd_end) {
            return Error::make(ErrorCode::invalid_argument, "truncated ZIP central directory");
        }
        unsigned char header[46];
        if (!pread_full(fd, header, sizeof(header), offset)) return io_error("read ZIP central header");
        if (read_u32(header) != kCentralSignature) {
            return Error::make(ErrorCode::invalid_argument, "invalid ZIP central directory entry");
        }
        ZipEntry entry;
        entry.flags = read_u16(header + 8);
        entry.method = read_u16(header + 10);
        entry.crc32 = read_u32(header + 16);
        entry.compressed_size = read_u32(header + 20);
        entry.uncompressed_size = read_u32(header + 24);
        const std::uint16_t name_length = read_u16(header + 28);
        const std::uint16_t extra_length = read_u16(header + 30);
        const std::uint16_t comment_length = read_u16(header + 32);
        entry.external_attributes = read_u32(header + 38);
        entry.local_header_offset = read_u32(header + 42);
        const std::uint64_t variable = static_cast<std::uint64_t>(name_length) + extra_length + comment_length;
        if (offset + 46 + variable > cd_end) {
            return Error::make(ErrorCode::invalid_argument, "truncated ZIP central directory variable data");
        }
        std::vector<unsigned char> name_bytes(name_length);
        if (name_length > 0 && !pread_full(fd, name_bytes.data(), name_length, offset + 46)) {
            return io_error("read ZIP entry name");
        }
        std::vector<unsigned char> extra(extra_length);
        if (extra_length > 0 && !pread_full(fd, extra.data(), extra_length, offset + 46 + name_length)) {
            return io_error("read ZIP extra field");
        }
        const std::string raw_name(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size());
        bool directory = false;
        std::string name_error;
        if (!validate_entry_name(raw_name, entry.name, directory, name_error)) {
            return Error::make(ErrorCode::invalid_argument, name_error + " (" + raw_name + ")");
        }
        entry.directory = directory;
        if (entry.flags & 0x0001u) {
            return Error::make(ErrorCode::unsupported, "encrypted ZIP entries are not supported");
        }
        if (entry.flags & 0x0040u) {
            return Error::make(ErrorCode::unsupported, "strongly encrypted ZIP entries are not supported");
        }
        if (entry.method != 0 && entry.method != 8) {
            return Error::make(ErrorCode::unsupported, "unsupported ZIP compression method");
        }
        if (!parse_zip64_extra(extra.data(), extra.size(), entry.uncompressed_size, entry.compressed_size,
                               entry.local_header_offset)) {
            return Error::make(ErrorCode::invalid_argument, "invalid ZIP64 extra field");
        }
        if (!unix_mode_is_supported(entry.external_attributes)) {
            return Error::make(ErrorCode::unsupported, "special files are not allowed in application packages");
        }
        if (unix_mode_is_link(entry.external_attributes)) {
            return Error::make(ErrorCode::unsupported, "symlinks are not allowed in application packages");
        }
        if (entry.directory && entry.uncompressed_size != 0) {
            return Error::make(ErrorCode::invalid_argument, "directory ZIP entry has content");
        }
        if (!seen_exact.insert(entry.name).second) {
            return Error::make(ErrorCode::invalid_argument, "duplicate ZIP entry: " + entry.name);
        }
        if (!seen_folded.insert(ascii_lower(entry.name)).second) {
            return Error::make(ErrorCode::invalid_argument,
                               "duplicate ZIP entry (case-insensitive): " + entry.name);
        }
        entries_.push_back(std::move(entry));
        offset += 46 + variable;
    }
    return Error::none();
}

Error ZipReader::extract_all(int dir_fd, std::vector<IntegrityEntry>& integrity,
                             std::uint64_t& total_bytes) const {
    integrity.clear();
    total_bytes = 0;
    integrity.reserve(entries_.size());

    struct statvfs stats {};
    if (::fstatvfs(dir_fd, &stats) == 0) {
        const std::uint64_t available = static_cast<std::uint64_t>(stats.f_bavail) *
                                       static_cast<std::uint64_t>(stats.f_frsize);
        std::uint64_t declared = 0;
        for (const auto& entry : entries_) {
            if (entry.directory) continue;
            if (entry.uncompressed_size > std::numeric_limits<std::uint64_t>::max() - declared) {
                return Error::make(ErrorCode::too_large, "ZIP declared size overflows");
            }
            declared += entry.uncompressed_size;
        }
        if (declared > available || (declared > 0 && available - declared < kDiskSafetyMargin)) {
            return Error::make(ErrorCode::too_large, "not enough free disk space for the application package");
        }
    }

    UniqueFd source(::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!source.valid()) return io_error("open application package " + path_);

    std::vector<char> input(kCopyChunk);
    std::vector<unsigned char> output(kInflateChunk);

    for (const auto& entry : entries_) {
        if (entry.directory) {
            UniqueFd parent;
            std::string name;
            Error error = ensure_parent_dir(dir_fd, entry.name + "/placeholder", parent, name);
            if (!error.ok()) return error;
            if (::mkdirat(parent.get(), name.c_str(), 0700) != 0 && errno != EEXIST) {
                return io_error("create package directory " + entry.name);
            }
            continue;
        }
        if (entry.local_header_offset > file_size_ || file_size_ - entry.local_header_offset < 30) {
            return Error::make(ErrorCode::invalid_argument, "ZIP local header outside the file");
        }
        unsigned char local[30];
        if (!pread_full(source.get(), local, sizeof(local), entry.local_header_offset)) {
            return io_error("read ZIP local header");
        }
        if (read_u32(local) != kLocalSignature) {
            return Error::make(ErrorCode::invalid_argument, "invalid ZIP local header");
        }
        const std::uint16_t local_flags = read_u16(local + 6);
        const std::uint16_t local_method = read_u16(local + 8);
        const std::uint16_t local_name_length = read_u16(local + 26);
        const std::uint16_t local_extra_length = read_u16(local + 28);
        if (local_flags != entry.flags || local_method != entry.method) {
            return Error::make(ErrorCode::invalid_argument,
                               "ZIP local and central headers disagree for " + entry.name);
        }
        std::vector<char> local_name(local_name_length);
        if (local_name_length > 0 &&
            !pread_full(source.get(), local_name.data(), local_name_length, entry.local_header_offset + 30)) {
            return io_error("read ZIP local name");
        }
        if (std::string_view(local_name.data(), local_name.size()) != entry.name &&
            std::string_view(local_name.data(), local_name.size()) != entry.name + "/") {
            return Error::make(ErrorCode::invalid_argument,
                               "ZIP local and central names disagree for " + entry.name);
        }
        const std::uint64_t data_offset =
            entry.local_header_offset + 30 + local_name_length + local_extra_length;
        if (data_offset > file_size_ || entry.compressed_size > file_size_ - data_offset) {
            return Error::make(ErrorCode::invalid_argument, "ZIP entry data lies outside the file");
        }

        UniqueFd parent;
        std::string final_name;
        Error error = ensure_parent_dir(dir_fd, entry.name, parent, final_name);
        if (!error.ok()) return error;
        UniqueFd output_fd(::openat(parent.get(), final_name.c_str(),
                                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!output_fd.valid()) return io_error("create package file " + entry.name);

        Sha256 hasher;
        std::uint64_t produced_total = 0;
        uLong crc = crc32(0L, Z_NULL, 0);
        bool stream_end = false;

        if (entry.method == 0) {
            if (entry.compressed_size != entry.uncompressed_size) {
                return Error::make(ErrorCode::invalid_argument, "stored ZIP entry has inconsistent sizes");
            }
            std::uint64_t remaining = entry.compressed_size;
            std::uint64_t offset = data_offset;
            while (remaining > 0) {
                const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, input.size()));
                if (!pread_full(source.get(), input.data(), chunk, offset)) return io_error("read ZIP entry data");
                if (!write_full(output_fd.get(), input.data(), chunk)) return io_error("write package file");
                hasher.update(input.data(), chunk);
                crc = crc32(crc, reinterpret_cast<const Bytef*>(input.data()), static_cast<uInt>(chunk));
                produced_total += chunk;
                offset += chunk;
                remaining -= chunk;
            }
            stream_end = true;
        } else {
            z_stream stream{};
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
                return Error::make(ErrorCode::io_error, "cannot initialize ZIP decompressor");
            }
            std::uint64_t remaining = entry.compressed_size;
            std::uint64_t offset = data_offset;
            bool stream_error = false;
            while (remaining > 0 && !stream_error) {
                const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, input.size()));
                if (!pread_full(source.get(), input.data(), chunk, offset)) {
                    stream_error = true;
                    break;
                }
                stream.next_in = reinterpret_cast<Bytef*>(input.data());
                stream.avail_in = static_cast<uInt>(chunk);
                while (stream.avail_in > 0) {
                    stream.next_out = output.data();
                    stream.avail_out = static_cast<uInt>(output.size());
                    const int result = inflate(&stream, Z_NO_FLUSH);
                    if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
                        stream_error = true;
                        break;
                    }
                    const std::size_t made = output.size() - stream.avail_out;
                    if (made > 0) {
                        if (produced_total + made > entry.uncompressed_size) {
                            stream_error = true;
                            break;
                        }
                        if (!write_full(output_fd.get(), output.data(), made)) {
                            stream_error = true;
                            break;
                        }
                        hasher.update(output.data(), made);
                        crc = crc32(crc, output.data(), static_cast<uInt>(made));
                        produced_total += made;
                    }
                    if (result == Z_STREAM_END) {
                        stream_end = true;
                        break;
                    }
                    if (made == 0 && stream.avail_in == 0) break;
                }
                offset += chunk;
                remaining -= chunk;
                if (stream_end) break;
            }
            if (!stream_error && !stream_end) {
                // Drain the final block; no further input is available.
                for (;;) {
                    stream.next_in = Z_NULL;
                    stream.avail_in = 0;
                    stream.next_out = output.data();
                    stream.avail_out = static_cast<uInt>(output.size());
                    const int result = inflate(&stream, Z_FINISH);
                    const std::size_t made = output.size() - stream.avail_out;
                    if (made > 0) {
                        if (produced_total + made > entry.uncompressed_size) {
                            stream_error = true;
                            break;
                        }
                        if (!write_full(output_fd.get(), output.data(), made)) {
                            stream_error = true;
                            break;
                        }
                        hasher.update(output.data(), made);
                        crc = crc32(crc, output.data(), static_cast<uInt>(made));
                        produced_total += made;
                    }
                    if (result == Z_STREAM_END) {
                        stream_end = true;
                        break;
                    }
                    if (result != Z_OK && result != Z_BUF_ERROR) {
                        stream_error = true;
                        break;
                    }
                    if (made == 0) break;
                }
            }
            if (!stream_error && !stream_end) stream_error = true;
            if (!stream_error && stream.total_in != entry.compressed_size) stream_error = true;
            inflateEnd(&stream);
            if (stream_error) {
                return Error::make(ErrorCode::invalid_argument, "cannot decompress ZIP entry " + entry.name);
            }
        }

        if (!stream_end || produced_total != entry.uncompressed_size ||
            static_cast<std::uint32_t>(crc) != entry.crc32) {
            return Error::make(ErrorCode::invalid_argument, "ZIP entry integrity check failed: " + entry.name);
        }
        if (::fsync(output_fd.get()) != 0) return io_error("flush package file " + entry.name);
        output_fd.reset();

        integrity.push_back(IntegrityEntry{entry.name, produced_total, hasher.finish_hex()});
        total_bytes += produced_total;
    }

    // Make directory entries durable before the SQLite commit activates them.
    if (::fsync(dir_fd) != 0) return io_error("flush package directory");
    return Error::none();
}

Error ZipReader::extract_all(int dir_fd, std::vector<ExtractedFile>& files,
                             std::vector<IntegrityEntry>& integrity, std::uint64_t& total_bytes) const {
    Error error = extract_all(dir_fd, integrity, total_bytes);
    if (!error.ok()) {
        files.clear();
        return error;
    }
    files.clear();
    files.reserve(integrity.size());
    for (const auto& entry : integrity) {
        ExtractedFile file;
        file.path = entry.path;
        file.size = entry.size;
        file.sha256 = entry.sha256;
        file.mime = mime_for_file_name(entry.path);
        files.push_back(std::move(file));
    }
    return Error::none();
}

Error ZipReader::verify_revision(const std::string& revision_root,
                                 const std::vector<IntegrityEntry>& entries,
                                 std::string& mismatch_path) {
    mismatch_path.clear();
    Error sandbox_error;
    auto sandbox = PathSandbox::create(revision_root, false, sandbox_error);
    if (!sandbox) return sandbox_error;
    std::vector<char> buffer(kCopyChunk);
    for (const auto& entry : entries) {
        UniqueFd fd;
        Error error = sandbox->open_file("/" + entry.path, O_RDONLY, fd);
        if (!error.ok()) {
            mismatch_path = entry.path;
            return Error::make(error.code, "integrity check cannot open " + entry.path + ": " + error.message);
        }
        struct stat info {};
        if (::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
            static_cast<std::uint64_t>(info.st_size) != entry.size) {
            mismatch_path = entry.path;
            return Error::make(ErrorCode::conflict, "integrity mismatch (size): " + entry.path);
        }
        Sha256 hasher;
        std::uint64_t offset = 0;
        for (;;) {
            const auto read = ::pread(fd.get(), buffer.data(), buffer.size(), static_cast<off_t>(offset));
            if (read < 0) {
                if (errno == EINTR) continue;
                mismatch_path = entry.path;
                return io_error("read revision file " + entry.path);
            }
            if (read == 0) break;
            hasher.update(buffer.data(), static_cast<std::size_t>(read));
            offset += static_cast<std::uint64_t>(read);
        }
        if (offset != entry.size || hasher.finish_hex() != entry.sha256) {
            mismatch_path = entry.path;
            return Error::make(ErrorCode::conflict, "integrity mismatch (sha256): " + entry.path);
        }
    }
    return Error::none();
}

} // namespace edgedesk::extensions
