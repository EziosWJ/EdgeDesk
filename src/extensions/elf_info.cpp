#include "extensions/elf_info.hpp"

#include "core/error.hpp"
#include "core/fd.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace edgedesk::extensions {
namespace {

constexpr unsigned char kElfMagic0 = 0x7f;
constexpr unsigned char kElfMag1 = 'E';
constexpr unsigned char kElfMag2 = 'L';
constexpr unsigned char kElfMag3 = 'F';

constexpr std::size_t kIdentSize = 16;
constexpr std::size_t kHeader32Size = 52;
constexpr std::size_t kHeader64Size = 64;

constexpr unsigned char kElfClass32 = 1;
constexpr unsigned char kElfClass64 = 2;
constexpr unsigned char kElfData2Lsb = 1;
constexpr unsigned char kElfVersionCurrent = 1;

constexpr std::uint16_t kEtExec = 2;
constexpr std::uint16_t kEtDyn = 3;

// e_machine values from the ELF specification.
constexpr std::uint16_t kEmArm = 40;
constexpr std::uint16_t kEmX86_64 = 62;
constexpr std::uint16_t kEmAArch64 = 183;

std::uint16_t read_le16(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0] | (static_cast<std::uint16_t>(data[1]) << 8));
}

bool read_full(int fd, void* buffer, std::size_t length, std::string& error) {
    auto* out = static_cast<unsigned char*>(buffer);
    std::size_t done = 0;
    while (done < length) {
        const auto read = ::pread(fd, out + done, length - done, static_cast<off_t>(done));
        if (read < 0) {
            if (errno == EINTR) continue;
            error = "cannot read backend ELF header: " + std::string(std::strerror(errno));
            return false;
        }
        if (read == 0) {
            error = "backend ELF header is truncated";
            return false;
        }
        done += static_cast<std::size_t>(read);
    }
    return true;
}

bool map_machine(std::uint16_t machine, ElfArch& out, std::string& error) {
    switch (machine) {
        case kEmX86_64:
            out = ElfArch::x86_64;
            return true;
        case kEmAArch64:
            out = ElfArch::aarch64;
            return true;
        case kEmArm:
            out = ElfArch::arm32;
            return true;
        default:
            error = "unsupported ELF machine " + std::to_string(machine);
            return false;
    }
}

} // namespace

const char* elf_arch_name(ElfArch arch) {
    switch (arch) {
        case ElfArch::x86_64:
            return "x86_64";
        case ElfArch::aarch64:
            return "aarch64";
        case ElfArch::arm32:
            return "arm32";
        case ElfArch::unknown:
        default:
            return "unknown";
    }
}

ElfArch host_elf_arch() {
#if defined(__x86_64__)
    return ElfArch::x86_64;
#elif defined(__aarch64__)
    return ElfArch::aarch64;
#elif defined(__arm__)
    return ElfArch::arm32;
#else
    return ElfArch::unknown;
#endif
}

// Reads only the ELF identification and the first header fields. Package
// content is never mapped or executed, and the final component is never
// followed through a symlink. Hard-linked aliases are rejected so a package
// cannot point the stable install path at a file outside its own revision.
bool inspect_elf(const std::string& path, ElfArch expected, ElfArch& actual, std::string& error) {
    actual = ElfArch::unknown;
    error.clear();

    UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!fd.valid()) {
        if (errno == ELOOP) {
            error = "backend executable is a symlink";
        } else if (errno == ENOENT) {
            error = "backend executable was not found";
        } else {
            error = "cannot open backend executable: " + std::string(std::strerror(errno));
        }
        return false;
    }

    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        error = "cannot stat backend executable: " + std::string(std::strerror(errno));
        return false;
    }
    if (!S_ISREG(info.st_mode)) {
        error = "backend executable is not a regular file";
        return false;
    }
    if (info.st_nlink > 1) {
        error = "backend executable has multiple hard links";
        return false;
    }
    if ((info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
        error = "backend executable is not marked executable";
        return false;
    }
    if (info.st_size < static_cast<off_t>(kIdentSize)) {
        error = "backend executable is not an ELF file";
        return false;
    }

    unsigned char ident[kIdentSize] = {};
    if (!read_full(fd.get(), ident, sizeof(ident), error)) return false;
    if (ident[0] != kElfMagic0 || ident[1] != kElfMag1 || ident[2] != kElfMag2 || ident[3] != kElfMag3) {
        error = "backend executable is not an ELF file";
        return false;
    }
    const unsigned char elf_class = ident[4];
    if (elf_class != kElfClass32 && elf_class != kElfClass64) {
        error = "unsupported ELF class";
        return false;
    }
    if (ident[5] != kElfData2Lsb) {
        error = "unsupported ELF byte order (little endian is required)";
        return false;
    }
    if (ident[6] != kElfVersionCurrent) {
        error = "unsupported ELF identification version";
        return false;
    }

    const std::size_t header_size = elf_class == kElfClass64 ? kHeader64Size : kHeader32Size;
    if (info.st_size < static_cast<off_t>(header_size)) {
        error = "backend ELF header is truncated";
        return false;
    }
    unsigned char header[kHeader64Size] = {};
    if (!read_full(fd.get(), header, header_size, error)) return false;

    const std::uint16_t type = read_le16(header + 16);
    if (type != kEtExec && type != kEtDyn) {
        if (type == 1) {
            error = "ELF relocatable objects are not executable backends";
        } else {
            error = "unsupported ELF file type " + std::to_string(type);
        }
        return false;
    }
    const std::uint16_t machine = read_le16(header + 18);
    ElfArch parsed = ElfArch::unknown;
    if (!map_machine(machine, parsed, error)) return false;
    const bool class_matches = (parsed == ElfArch::arm32 && elf_class == kElfClass32) ||
                               (parsed != ElfArch::arm32 && elf_class == kElfClass64);
    if (!class_matches) {
        error = "ELF class does not match machine " + std::string(elf_arch_name(parsed));
        return false;
    }

    actual = parsed;
    if (actual != expected) {
        error = "backend architecture " + std::string(elf_arch_name(actual)) + " does not match host " +
                std::string(elf_arch_name(expected));
        return false;
    }
    return true;
}

} // namespace edgedesk::extensions
