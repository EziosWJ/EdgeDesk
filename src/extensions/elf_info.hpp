#pragma once

#include <string>

// Minimal ELF inspection for native application packages (SPEC-0011 section
// 4.2). This module reads headers only: it must never map and execute package
// content, and it must never follow a symlink.
namespace edgedesk::extensions {

enum class ElfArch {
    unknown = 0,
    x86_64,
    aarch64,
    arm32,
};

const char* elf_arch_name(ElfArch arch);

// Architecture of the running EdgeDesk process. Native packages must match it.
ElfArch host_elf_arch();

// Validates that `path` is a regular, executable, single-architecture ELF file
// and that its e_machine matches `expected`. Scripts (shebang), non-ELF files,
// dynamically mixed objects, symlinks and hard-linked aliases outside the
// extracted revision are rejected.
bool inspect_elf(const std::string& path, ElfArch expected, ElfArch& actual, std::string& error);

} // namespace edgedesk::extensions
