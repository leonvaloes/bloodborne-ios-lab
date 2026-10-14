#include "check.h"
#include "guest_loader.h"
#include <vector>
#include <cstring>

int main() { return run([] {
    using namespace bb;

    // Create a valid synthetic ELF image with PT_LOAD segments and entry point
    std::vector<std::uint8_t> elf(256, 0);
    elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 2; elf[5] = 1; elf[6] = 1;
    std::uint16_t e_type = 2, e_mach = EM_X86_64, e_ehsz = 64, e_phent = 56, e_phnum = 2;
    std::uint32_t e_ver = 1;
    std::uint64_t e_entry = 0x400000, e_phoff = 64;

    std::memcpy(elf.data() + 16, &e_type, 2);
    std::memcpy(elf.data() + 18, &e_mach, 2);
    std::memcpy(elf.data() + 20, &e_ver, 4);
    std::memcpy(elf.data() + 24, &e_entry, 8);
    std::memcpy(elf.data() + 32, &e_phoff, 8);
    std::memcpy(elf.data() + 52, &e_ehsz, 2);
    std::memcpy(elf.data() + 54, &e_phent, 2);
    std::memcpy(elf.data() + 56, &e_phnum, 2);

    // PH 0: Code segment (PT_LOAD, R-X)
    std::uint32_t ph0_type = 1, ph0_flags = 5;
    std::uint64_t ph0_off = 0, ph0_vaddr = 0x400000, ph0_paddr = 0, ph0_filesz = 0x1000, ph0_memsz = 0x1000, ph0_align = 0x1000;
    std::memcpy(elf.data() + 64, &ph0_type, 4);
    std::memcpy(elf.data() + 68, &ph0_flags, 4);
    std::memcpy(elf.data() + 72, &ph0_off, 8);
    std::memcpy(elf.data() + 80, &ph0_vaddr, 8);
    std::memcpy(elf.data() + 88, &ph0_paddr, 8);
    std::memcpy(elf.data() + 96, &ph0_filesz, 8);
    std::memcpy(elf.data() + 104, &ph0_memsz, 8);
    std::memcpy(elf.data() + 112, &ph0_align, 8);

    // PH 1: Data segment (PT_LOAD, RW-)
    std::uint32_t ph1_type = 1, ph1_flags = 6;
    std::uint64_t ph1_off = 0x1000, ph1_vaddr = 0x402000, ph1_paddr = 0, ph1_filesz = 0x1000, ph1_memsz = 0x2000, ph1_align = 0x1000;
    std::memcpy(elf.data() + 120, &ph1_type, 4);
    std::memcpy(elf.data() + 124, &ph1_flags, 4);
    std::memcpy(elf.data() + 128, &ph1_off, 8);
    std::memcpy(elf.data() + 136, &ph1_vaddr, 8);
    std::memcpy(elf.data() + 144, &ph1_paddr, 8);
    std::memcpy(elf.data() + 152, &ph1_filesz, 8);
    std::memcpy(elf.data() + 160, &ph1_memsz, 8);
    std::memcpy(elf.data() + 168, &ph1_align, 8);

    GuestMemory memory(512 * 1024); // 512 KB budget
    IndirectDispatcher dispatcher;
    auto hle = HleManager::create_standard();

    auto report = GuestLoader::load_and_boot(memory, elf.data(), elf.size(), dispatcher, hle);

    CHECK(report.success);
    CHECK(report.first_instruction_executed);
    CHECK(report.entry_point == 0x400000);
    CHECK(report.code_base == 0x400000);
    CHECK(report.code_size == 0x1000);
    CHECK(report.data_base == 0x402000);
    CHECK(report.data_size == 0x2000);
    CHECK(report.modules_mapped == 2);
    CHECK((report.stack_pointer & 0xF) == 0); // Must be 16-byte aligned

    std::cout << "guest_boot: ELF loader, memory segment mapping, stack setup and first guest instruction executed successfully\n";
}); }
