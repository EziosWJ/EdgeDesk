#include "test_util.hpp"

#include "extensions/elf_info.hpp"

using namespace edgedesk::extensions;

namespace {

void test_names() {
    CHECK_STR_EQ(elf_arch_name(ElfArch::x86_64), "x86_64");
    CHECK_STR_EQ(elf_arch_name(ElfArch::aarch64), "aarch64");
    CHECK_STR_EQ(elf_arch_name(ElfArch::arm32), "arm32");
    CHECK_STR_EQ(elf_arch_name(ElfArch::unknown), "unknown");
    CHECK(host_elf_arch() != ElfArch::unknown);
}

} // namespace

int main() {
    test_names();
    return edgedesk::test::finish("elf_info_test");
}
