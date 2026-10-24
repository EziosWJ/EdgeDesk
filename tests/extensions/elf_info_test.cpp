#include "test_util.hpp"

#include "extensions/elf_info.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace edgedesk::extensions;

namespace {

namespace fs = std::filesystem;

constexpr unsigned char kClass32 = 1;
constexpr unsigned char kClass64 = 2;

std::uint16_t machine_for(ElfArch arch) {
    switch (arch) {
        case ElfArch::x86_64: return 62;
        case ElfArch::aarch64: return 183;
        case ElfArch::arm32: return 40;
        case ElfArch::unknown: break;
    }
    return 0;
}

unsigned char class_for(ElfArch arch) { return arch == ElfArch::arm32 ? kClass32 : kClass64; }

void put_le16(std::string& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<char>(value & 0xff);
    bytes[offset + 1] = static_cast<char>((value >> 8) & 0xff);
}

// Minimal ELF header with no program/section tables. inspect_elf only reads
// the identification and the first header fields.
std::string elf_header(ElfArch arch, std::uint16_t type = 3, unsigned char elf_class = 0,
                       unsigned char endian = 1, std::uint16_t machine = 0) {
    const unsigned char cls = elf_class != 0 ? elf_class : class_for(arch);
    const std::size_t size = cls == kClass64 ? 64 : 52;
    std::string bytes(size, '\0');
    bytes[0] = '\x7f';
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = static_cast<char>(cls);
    bytes[5] = static_cast<char>(endian);
    bytes[6] = 1; // EV_CURRENT
    put_le16(bytes, 16, type);
    put_le16(bytes, 18, machine != 0 ? machine : machine_for(arch));
    put_le16(bytes, 20, 1);
    return bytes;
}

std::string make_dir() {
    const fs::path dir = fs::temp_directory_path() / ("edgedesk-elf-test-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir.string();
}

void write_file(const fs::path& path, const std::string& bytes, mode_t mode) {
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    CHECK_EQ(::chmod(path.c_str(), mode), 0);
}

ElfArch other_arch() { return host_elf_arch() == ElfArch::x86_64 ? ElfArch::aarch64 : ElfArch::x86_64; }

void test_names() {
    CHECK_STR_EQ(elf_arch_name(ElfArch::x86_64), "x86_64");
    CHECK_STR_EQ(elf_arch_name(ElfArch::aarch64), "aarch64");
    CHECK_STR_EQ(elf_arch_name(ElfArch::arm32), "arm32");
    CHECK_STR_EQ(elf_arch_name(ElfArch::unknown), "unknown");
    CHECK(host_elf_arch() != ElfArch::unknown);
}

void test_valid_host_elf(const fs::path& dir) {
    const auto path = dir / "valid";
    write_file(path, elf_header(host_elf_arch()), 0755);
    ElfArch actual = ElfArch::unknown;
    std::string error;
    CHECK(inspect_elf(path.string(), host_elf_arch(), actual, error));
    if (!error.empty()) std::fprintf(stderr, "valid ELF error: %s\n", error.c_str());
    CHECK(actual == host_elf_arch());
    // ET_EXEC is accepted as well as ET_DYN (static PIE).
    const auto exec_path = dir / "valid-exec";
    write_file(exec_path, elf_header(host_elf_arch(), 2), 0755);
    actual = ElfArch::unknown;
    CHECK(inspect_elf(exec_path.string(), host_elf_arch(), actual, error));
    CHECK(actual == host_elf_arch());
}

void test_rejections(const fs::path& dir) {
    ElfArch actual = ElfArch::unknown;
    std::string error;

    const auto non_exec = dir / "non-exec";
    write_file(non_exec, elf_header(host_elf_arch()), 0644);
    CHECK(!inspect_elf(non_exec.string(), host_elf_arch(), actual, error));
    CHECK(error.find("not marked executable") != std::string::npos);
    CHECK(actual == ElfArch::unknown);

    const auto script = dir / "script";
    write_file(script, "#!/bin/sh\necho hi\n", 0755);
    CHECK(!inspect_elf(script.string(), host_elf_arch(), actual, error));
    CHECK(error.find("not an ELF") != std::string::npos);

    const auto text = dir / "text";
    write_file(text, "this is not an ELF file at all, but it is long enough to be one", 0755);
    CHECK(!inspect_elf(text.string(), host_elf_arch(), actual, error));
    CHECK(error.find("not an ELF") != std::string::npos);

    const auto truncated = dir / "truncated";
    write_file(truncated, std::string("\x7f" "ELF\x02\x01\x01", 7), 0755);
    CHECK(!inspect_elf(truncated.string(), host_elf_arch(), actual, error));
    CHECK(!error.empty());

    const auto wrong = dir / "wrong-arch";
    write_file(wrong, elf_header(other_arch()), 0755);
    actual = ElfArch::unknown;
    CHECK(!inspect_elf(wrong.string(), host_elf_arch(), actual, error));
    CHECK(actual == other_arch());
    CHECK(error.find("does not match host") != std::string::npos);
    CHECK(error.find(elf_arch_name(other_arch())) != std::string::npos);

    const auto big_endian = dir / "big-endian";
    write_file(big_endian, elf_header(host_elf_arch(), 3, 0, 2), 0755);
    CHECK(!inspect_elf(big_endian.string(), host_elf_arch(), actual, error));
    CHECK(error.find("byte order") != std::string::npos);

    const auto bad_class = dir / "bad-class";
    write_file(bad_class, elf_header(host_elf_arch(), 3, 3), 0755);
    CHECK(!inspect_elf(bad_class.string(), host_elf_arch(), actual, error));
    CHECK(error.find("ELF class") != std::string::npos);

    const auto bad_ident_version = dir / "bad-ident-version";
    {
        std::string bytes = elf_header(host_elf_arch());
        bytes[6] = 0;
        write_file(bad_ident_version, bytes, 0755);
    }
    CHECK(!inspect_elf(bad_ident_version.string(), host_elf_arch(), actual, error));
    CHECK(error.find("identification version") != std::string::npos);

    const auto relocatable = dir / "relocatable";
    write_file(relocatable, elf_header(host_elf_arch(), 1), 0755);
    CHECK(!inspect_elf(relocatable.string(), host_elf_arch(), actual, error));
    CHECK(error.find("relocatable") != std::string::npos);

    const auto unknown_machine = dir / "unknown-machine";
    write_file(unknown_machine, elf_header(host_elf_arch(), 3, 0, 1, 8), 0755);
    CHECK(!inspect_elf(unknown_machine.string(), host_elf_arch(), actual, error));
    CHECK(error.find("unsupported ELF machine") != std::string::npos);

    // A 32-bit header that claims x86_64 is a class/machine mismatch.
    const auto mismatched = dir / "class-machine-mismatch";
    write_file(mismatched, elf_header(host_elf_arch(), 3, kClass32, 1, 62), 0755);
    CHECK(!inspect_elf(mismatched.string(), host_elf_arch(), actual, error));
    CHECK(error.find("class does not match") != std::string::npos);

    const auto missing = dir / "does-not-exist";
    CHECK(!inspect_elf(missing.string(), host_elf_arch(), actual, error));
    CHECK(error.find("not found") != std::string::npos);

    CHECK(!inspect_elf(dir.string(), host_elf_arch(), actual, error));
    CHECK(error.find("regular file") != std::string::npos);
}

void test_symlink_and_hard_link(const fs::path& dir) {
    const auto target = dir / "real-target";
    write_file(target, elf_header(host_elf_arch()), 0755);
    const auto link = dir / "symlink";
    std::error_code link_error;
    fs::create_symlink(target, link, link_error);
    CHECK(!link_error);
    ElfArch actual = ElfArch::unknown;
    std::string error;
    CHECK(!inspect_elf(link.string(), host_elf_arch(), actual, error));
    CHECK(error.find("symlink") != std::string::npos);

    const auto hard = dir / "hard-link";
    CHECK_EQ(::link(target.c_str(), hard.c_str()), 0);
    CHECK(!inspect_elf(hard.string(), host_elf_arch(), actual, error));
    CHECK(error.find("hard link") != std::string::npos);
}

void test_architecture_mapping(const fs::path& dir) {
    // ARM32 must be reported as a 32-bit ARM binary, independent of the host.
    const auto arm = dir / "arm32";
    write_file(arm, elf_header(ElfArch::arm32), 0755);
    ElfArch actual = ElfArch::unknown;
    std::string error;
    CHECK(inspect_elf(arm.string(), ElfArch::arm32, actual, error));
    CHECK(actual == ElfArch::arm32);

    const auto aarch64 = dir / "aarch64";
    write_file(aarch64, elf_header(ElfArch::aarch64), 0755);
    actual = ElfArch::unknown;
    CHECK(inspect_elf(aarch64.string(), ElfArch::aarch64, actual, error));
    CHECK(actual == ElfArch::aarch64);

    const auto x86 = dir / "x86_64";
    write_file(x86, elf_header(ElfArch::x86_64), 0755);
    actual = ElfArch::unknown;
    CHECK(inspect_elf(x86.string(), ElfArch::x86_64, actual, error));
    CHECK(actual == ElfArch::x86_64);
}

} // namespace

int main() {
    const fs::path dir = make_dir();
    test_names();
    test_valid_host_elf(dir);
    test_rejections(dir);
    test_symlink_and_hard_link(dir);
    test_architecture_mapping(dir);
    fs::remove_all(dir);
    return edgedesk::test::finish("elf_info_test");
}
