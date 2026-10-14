#include "boot_probe.h"
#include "elf_parser.h"
#include "guest_memory.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>

namespace bb {
BootProbeResult probe_eboot_entry(const std::string& documents_dir) {
    BootProbeResult result;
    const auto path = std::filesystem::path(documents_dir) / "eboot.elf";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return result;
    if (ec || std::filesystem::is_symlink(path, ec) ||
        !std::filesystem::is_regular_file(path, ec)) {
        result.status = "invalid_file";
        return result;
    }
    result.file_size = std::filesystem::file_size(path, ec);
    if (ec || result.file_size < 64 || result.file_size > 128ull * 1024 * 1024) {
        result.status = "invalid_size";
        return result;
    }
    std::ifstream file(path, std::ios::binary);
    std::array<std::uint8_t, 4096> header{};
    file.read(reinterpret_cast<char*>(header.data()), header.size());
    const auto prefix_size = static_cast<std::size_t>(file.gcount());
    if (prefix_size < 64) {
        result.status = "read_error";
        return result;
    }
    // The ELF program header table must fit in this bounded prefix.
    const auto inspection = SafeGameParser::inspect_executable(header.data(), prefix_size);
    if (!inspection.is_valid || inspection.is_self || inspection.machine != EM_X86_64) {
        result.status = "invalid_elf";
        return result;
    }
    result.entry_rip = inspection.entry_point;
    const ElfProgramHeader* entry = nullptr;
    for (const auto& ph : inspection.program_headers) {
        if (ph.type == 1 && ph.is_executable() &&
            result.entry_rip >= ph.vaddr && result.entry_rip - ph.vaddr < ph.filesz) {
            entry = &ph;
            break;
        }
    }
    if (!entry) {
        result.status = "entry_unmapped";
        return result;
    }
    const auto delta = result.entry_rip - entry->vaddr;
    if (entry->offset > result.file_size || delta > result.file_size - entry->offset ||
        result.file_size - entry->offset - delta < 4 ||
        entry->offset + delta > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        result.status = "entry_out_of_bounds";
        return result;
    }
    file.seekg(static_cast<std::streamoff>(entry->offset + delta));
    std::array<std::uint8_t, 16> code{};
    if (!file.read(reinterpret_cast<char*>(code.data()), code.size())) {
        result.status = "read_error";
        return result;
    }
    constexpr std::uint64_t stack_base = 0x70000000;
    GuestMemory memory(128 * 1024);
    if (memory.map(stack_base, 64 * 1024, bb::read | bb::write) != MemoryError::ok) {
        result.status = "memory_error";
        return result;
    }
    std::uint64_t rsp = (stack_base + 64 * 1024 - 128) & ~std::uint64_t(15);
    std::uint64_t rbp = 0;
    std::size_t ip = 0;
    // Interpret only two well-defined instructions, with fail-closed dispatch.
    if (code[ip] == 0x55) { // push rbp
        rsp -= 8;
        if (memory.store(rsp, &rbp, sizeof(rbp)) != MemoryError::ok) {
            result.status = "memory_error";
            return result;
        }
        ++ip;
        ++result.instructions_executed;
    }
    if (result.instructions_executed == 1 && code[ip] == 0x48 &&
        code[ip + 1] == 0x89 && code[ip + 2] == 0xe5) { // mov rbp, rsp
        rbp = rsp;
        ip += 3;
        ++result.instructions_executed;
    }
    result.stop_rip = result.entry_rip + ip;
    result.stack_pointer = rsp;
    result.stop_opcode = code[ip];
    result.entry_reached = result.instructions_executed > 0;
    result.status = result.instructions_executed == 2 ? "stopped_after_two_instructions" :
        "unsupported_entry_opcode";
    return result;
}
} // namespace bb
