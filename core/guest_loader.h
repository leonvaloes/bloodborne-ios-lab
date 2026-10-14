#pragma once
#include "guest_memory.h"
#include "dispatcher.h"
#include "hle.h"
#include "elf_parser.h"
#include <cstdint>
#include <string>

namespace bb {

struct GuestContext {
    std::uint64_t rax = 0;
    std::uint64_t rcx = 0;
    std::uint64_t rdx = 0;
    std::uint64_t rbx = 0;
    std::uint64_t rsp = 0;
    std::uint64_t rbp = 0;
    std::uint64_t rsi = 0;
    std::uint64_t rdi = 0;
    std::uint64_t r8  = 0;
    std::uint64_t r9  = 0;
    std::uint64_t r10 = 0;
    std::uint64_t r11 = 0;
    std::uint64_t r12 = 0;
    std::uint64_t r13 = 0;
    std::uint64_t r14 = 0;
    std::uint64_t r15 = 0;
    std::uint64_t rip = 0;
    std::uint64_t rflags = 0x202; // IF enabled
};

struct BootReport {
    bool success = false;
    std::uint64_t entry_point = 0;
    std::uint64_t code_base = 0;
    std::uint64_t code_size = 0;
    std::uint64_t data_base = 0;
    std::uint64_t data_size = 0;
    std::uint64_t stack_pointer = 0;
    std::uint32_t modules_mapped = 0;
    bool first_instruction_executed = false;
    std::string diagnostic_message;
};

class GuestLoader {
public:
    static BootReport load_and_boot(
        GuestMemory& memory,
        const std::uint8_t* elf_data,
        std::size_t size,
        IndirectDispatcher& dispatcher,
        const HleManager& hle
    );
};

} // namespace bb
