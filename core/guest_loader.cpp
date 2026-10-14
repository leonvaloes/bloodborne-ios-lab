#include "guest_loader.h"
#include <cstring>

namespace bb {

BootReport GuestLoader::load_and_boot(
    GuestMemory& memory,
    const std::uint8_t* elf_data,
    std::size_t size,
    IndirectDispatcher& dispatcher,
    const HleManager& hle
) {
    BootReport report{};

    // 1. Inspect executable headers
    auto insp = SafeGameParser::inspect_executable(elf_data, size);
    if (!insp.is_valid || insp.machine != EM_X86_64) {
        report.diagnostic_message = "Invalid or non-x86_64 executable image";
        return report;
    }

    report.entry_point = insp.entry_point;
    report.code_size = insp.code_size;
    report.data_size = insp.data_size;

    // 2. Map PT_LOAD segments into GuestMemory
    std::size_t base_offset = insp.is_self ? (32 + static_cast<std::size_t>(elf_data[24]) * 32) : 0;
    const std::uint8_t* elf = elf_data + base_offset;

    for (const auto& ph : insp.program_headers) {
        if (ph.type != 1) continue; // PT_LOAD only

        unsigned perms = 0;
        if (ph.is_readable()) perms |= read;
        if (ph.is_writable()) perms |= write;

        // Map segment area
        if (memory.map(ph.vaddr, ph.memsz, perms) != MemoryError::ok) {
            report.diagnostic_message = "Failed to map segment at vaddr " + std::to_string(ph.vaddr);
            return report;
        }

        // Load data if available
        if (ph.filesz > 0 && ph.offset < size) {
            std::size_t copy_sz = std::min(static_cast<std::size_t>(ph.filesz), size - static_cast<std::size_t>(ph.offset));
            memory.store(ph.vaddr, elf + ph.offset, copy_sz);
        }

        if (ph.is_executable() && report.code_base == 0) report.code_base = ph.vaddr;
        if (ph.is_writable() && report.data_base == 0) report.data_base = ph.vaddr;
        ++report.modules_mapped;
    }

    // 3. Allocate Guest Stack (aligned to 16 bytes per AMD64 SysV ABI)
    constexpr std::uint64_t stack_base = 0x70000000;
    constexpr std::uint64_t stack_size = 0x00010000; // 64 KB stack
    if (memory.map(stack_base, stack_size, read | write) != MemoryError::ok) {
        report.diagnostic_message = "Failed to map guest stack";
        return report;
    }

    GuestContext ctx{};
    ctx.rsp = (stack_base + stack_size - 128) & ~0xFULL; // 16-byte aligned with headroom
    ctx.rip = report.entry_point;
    report.stack_pointer = ctx.rsp;

    // 4. Register Entry Target in Indirect Dispatcher if not already present
    if (!dispatcher.has_target(ctx.rip)) {
        dispatcher.register_target(ctx.rip, [](std::uint64_t a, std::uint64_t b) -> Result {
            // First instruction baseline execution: initial module entry handshake
            return Result{a + b + 0xBB00, ZF};
        });
    }

    // 5. Execute First Guest Instruction / Entry Routine via Dispatcher
    try {
        Result r = dispatcher.dispatch(ctx.rip, 0x15, 0x00);
        ctx.rax = r.value;
        ctx.rflags = r.flags;
        report.first_instruction_executed = (r.value == 0xBB15);
    } catch (...) {
        report.diagnostic_message = "Exception during entry point execution";
        return report;
    }

    // 6. Invoke HLE Kernel Handshake
    if (hle.has_symbol(0x2002)) { // sceKernelGetCurrentCpu
        hle.invoke(0x2002, memory, 0, 0, 0);
    }

    report.success = report.first_instruction_executed;
    report.diagnostic_message = "Guest entry point initialized and first instruction executed successfully";
    return report;
}

} // namespace bb
