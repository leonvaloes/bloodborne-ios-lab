#include "check.h"
#include "boot_probe.h"
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

int main() { return run([] {
    const auto dir = std::filesystem::temp_directory_path() /
        ("boot_probe_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    CHECK(bb::probe_eboot_entry(dir.string()).status == "not_found");

    std::array<std::uint8_t, 4096> elf{};
    std::memcpy(elf.data(), "\x7f" "ELF", 4);
    elf[4] = 2; elf[5] = 1; elf[6] = 1;
    const std::uint16_t machine = 62, phsize = 56, phnum = 1;
    const std::uint32_t type = 1, flags = 5;
    const std::uint64_t entry = 0xa0, phoff = 64, offset = 0x100, filesz = 0x100;
    std::memcpy(elf.data() + 18, &machine, 2);
    std::memcpy(elf.data() + 24, &entry, 8);
    std::memcpy(elf.data() + 32, &phoff, 8);
    std::memcpy(elf.data() + 54, &phsize, 2);
    std::memcpy(elf.data() + 56, &phnum, 2);
    std::memcpy(elf.data() + 64, &type, 4);
    std::memcpy(elf.data() + 68, &flags, 4);
    std::memcpy(elf.data() + 72, &offset, 8);
    std::memcpy(elf.data() + 96, &filesz, 8);
    std::memcpy(elf.data() + 104, &filesz, 8);
    elf[offset + entry] = 0x55;
    elf[offset + entry + 1] = 0x48;
    elf[offset + entry + 2] = 0x89;
    elf[offset + entry + 3] = 0xe5;
    elf[offset + entry + 4] = 0x41;
    const auto file = dir / "eboot.elf";
    auto write = [&]() {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(elf.data()),
            static_cast<std::streamsize>(elf.size()));
        CHECK(out.good());
    };
    write();
    auto result = bb::probe_eboot_entry(dir.string());
    std::cout << "boot probe status: " << result.status << " rip=" << result.entry_rip << "\\n";
    CHECK(result.status == "stopped_after_two_instructions");
    CHECK(result.entry_reached && result.instructions_executed == 2);
    CHECK(result.entry_rip == 0xa0 && result.stop_rip == 0xa4);
    CHECK(result.stop_opcode == 0x41);
    const auto full_filesz = filesz;
    auto set_filesz = [&](std::uint64_t size) {
        std::memcpy(elf.data() + 96, &size, 8);
        std::memcpy(elf.data() + 104, &size, 8);
    };
    // Four prologue bytes alone do not license inspecting the byte beyond PT_LOAD.
    set_filesz(entry + 4); write();
    result = bb::probe_eboot_entry(dir.string());
    CHECK(result.status == "entry_out_of_bounds");
    CHECK(!result.entry_reached && result.instructions_executed == 0);
    set_filesz(full_filesz);
    // The file may end immediately after the inspected byte; no 16-byte tail required.
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(elf.data()), offset + entry + 5);
        CHECK(out.good());
    }
    result = bb::probe_eboot_entry(dir.string());
    CHECK(result.status == "stopped_after_two_instructions");
    CHECK(result.stop_opcode == 0x41);
    elf[offset + entry] = 0xcc; write();
    result = bb::probe_eboot_entry(dir.string());
    CHECK(result.status == "unsupported_entry_opcode");
    CHECK(!result.entry_reached && result.instructions_executed == 0);
    elf[0] = 0; write();
    CHECK(bb::probe_eboot_entry(dir.string()).status == "invalid_elf");

    if (const char* real_dir = std::getenv("BB_TEST_EBOOT_DIR")) {
        result = bb::probe_eboot_entry(real_dir);
        std::cout << "boot probe status: " << result.status << " rip=" << result.entry_rip << "\\n";
    CHECK(result.status == "stopped_after_two_instructions");
        CHECK(result.entry_rip == 0xa0 && result.stop_rip == 0xa4);
        CHECK(result.stop_opcode == 0x41);
    }
}); }
