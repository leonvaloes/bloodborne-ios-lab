#pragma once
#include <cstdint>
#include <string>

namespace bb {
struct BootProbeResult {
    std::string status{"not_found"};
    bool entry_reached{false};
    std::uint64_t file_size{0};
    std::uint64_t entry_rip{0};
    std::uint64_t stop_rip{0};
    std::uint64_t stack_pointer{0};
    std::uint32_t instructions_executed{0};
    std::uint8_t stop_opcode{0};
};

// Bounded x86-64 entry probe. No JIT, external calls, or game loop.
BootProbeResult probe_eboot_entry(const std::string& documents_dir);
} // namespace bb
