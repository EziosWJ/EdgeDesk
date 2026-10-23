#include "extensions/elf_info.hpp"

#include "core/error.hpp"

namespace edgedesk::extensions {

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

// Implemented by APPX-01. The stub keeps the contract compilable while the
// parallel workstreams build against it.
bool inspect_elf(const std::string& path, ElfArch expected, ElfArch& actual, std::string& error) {
    (void)path;
    (void)expected;
    actual = ElfArch::unknown;
    error = "ELF inspection is not implemented";
    return false;
}

} // namespace edgedesk::extensions
