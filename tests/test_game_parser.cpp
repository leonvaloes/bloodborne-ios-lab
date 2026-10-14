#include "check.h"
#include "elf_parser.h"
#include <cstring>
#include <vector>

int main() { return run([] {
    using namespace bb;

    // 1. Synthetic SFO Test Vector
    std::vector<std::uint8_t> sfo_buf(64, 0);
    sfo_buf[0] = 0; sfo_buf[1] = 'P'; sfo_buf[2] = 'S'; sfo_buf[3] = 'F'; // magic
    std::uint32_t val_ver = 0x00000101, val_keys = 36, val_vals = 48, val_count = 1;
    std::memcpy(sfo_buf.data() + 4, &val_ver, 4);
    std::memcpy(sfo_buf.data() + 8, &val_keys, 4);
    std::memcpy(sfo_buf.data() + 12, &val_vals, 4);
    std::memcpy(sfo_buf.data() + 16, &val_count, 4);
    // Entry 0: key_off=0, fmt=0x0204, len=9, max=16, off=0
    std::uint16_t e_key = 0, e_fmt = 0x0204;
    std::uint32_t e_len = 9, e_max = 16, e_off = 0;
    std::memcpy(sfo_buf.data() + 20, &e_key, 2);
    std::memcpy(sfo_buf.data() + 22, &e_fmt, 2);
    std::memcpy(sfo_buf.data() + 24, &e_len, 4);
    std::memcpy(sfo_buf.data() + 28, &e_max, 4);
    std::memcpy(sfo_buf.data() + 32, &e_off, 4);
    std::memcpy(sfo_buf.data() + 36, "TITLE", 5);
    std::memcpy(sfo_buf.data() + 48, "TestGame", 8);

    auto parsed_meta = SafeGameParser::parse_sfo(sfo_buf.data(), sfo_buf.size());
    CHECK(parsed_meta.title == "TestGame");

    // 2. Synthetic ELF64 Test Vector
    std::vector<std::uint8_t> elf_buf(128, 0);
    elf_buf[0] = 0x7f; elf_buf[1] = 'E'; elf_buf[2] = 'L'; elf_buf[3] = 'F';
    elf_buf[4] = 2; // 64-bit
    elf_buf[5] = 1; // Little endian
    elf_buf[6] = 1; // Version
    std::uint16_t e_type = 2, e_mach = EM_X86_64, e_ehsize = 64, e_phentsize = 56, e_phnum = 1;
    std::uint32_t e_version = 1;
    std::uint64_t e_entry = 0x400000, e_phoff = 64;
    std::memcpy(elf_buf.data() + 16, &e_type, 2);
    std::memcpy(elf_buf.data() + 18, &e_mach, 2);
    std::memcpy(elf_buf.data() + 20, &e_version, 4);
    std::memcpy(elf_buf.data() + 24, &e_entry, 8);
    std::memcpy(elf_buf.data() + 32, &e_phoff, 8);
    std::memcpy(elf_buf.data() + 52, &e_ehsize, 2);
    std::memcpy(elf_buf.data() + 54, &e_phentsize, 2);
    std::memcpy(elf_buf.data() + 56, &e_phnum, 2);

    // PH 0: type=1 (PT_LOAD), flags=5 (R-X), off=0, vaddr=0x400000, filesz=0x1000, memsz=0x1000, align=0x1000
    std::uint32_t p_type = 1, p_flags = 5;
    std::uint64_t p_off = 0, p_vaddr = 0x400000, p_paddr = 0, p_filesz = 0x1000, p_memsz = 0x1000, p_align = 0x1000;
    std::memcpy(elf_buf.data() + 64, &p_type, 4);
    std::memcpy(elf_buf.data() + 68, &p_flags, 4);
    std::memcpy(elf_buf.data() + 72, &p_off, 8);
    std::memcpy(elf_buf.data() + 80, &p_vaddr, 8);
    std::memcpy(elf_buf.data() + 88, &p_paddr, 8);
    std::memcpy(elf_buf.data() + 96, &p_filesz, 8);
    std::memcpy(elf_buf.data() + 104, &p_memsz, 8);
    std::memcpy(elf_buf.data() + 112, &p_align, 8);

    auto insp = SafeGameParser::inspect_executable(elf_buf.data(), elf_buf.size());
    CHECK(insp.is_valid);
    CHECK(insp.machine == EM_X86_64);
    CHECK(insp.entry_point == 0x400000);
    CHECK(insp.program_headers.size() == 1);
    CHECK(insp.code_size == 0x1000);
    CHECK(insp.data_size == 0);

    auto json_out = insp.to_json();
    CHECK(json_out.find("is_valid") != std::string::npos);
    CHECK(json_out.find("machine") != std::string::npos);

    std::cout << "game_parser: synthetic SFO metadata, ELF64 headers and bounds checks passed\n";
}); }
