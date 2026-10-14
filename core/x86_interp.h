#pragma once
#include "guest_memory.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bb {

constexpr std::uint64_t FLAG_CF = 0x0001, FLAG_PF = 0x0004, FLAG_AF = 0x0010, FLAG_ZF = 0x0040,
                        FLAG_SF = 0x0080, FLAG_DF = 0x0400, FLAG_OF = 0x0800;

enum class CpuStop {
    none,                // still running (never returned by run())
    step_limit,          // instruction budget exhausted
    unsupported_opcode,  // decoder/interpreter does not implement this encoding
    memory_fault,        // guest access to unmapped/denied memory (or fetch fault)
    trap,                // rip entered a registered trap range (HLE import / exit stub)
    halt,                // HLT / INT3
    divide_error,        // #DE
    undefined            // UD2
};

const char* cpu_stop_name(CpuStop stop);

struct CpuState {
    std::uint64_t gpr[16] = {};   // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
    std::uint64_t rip = 0;
    std::uint64_t rflags = 0x202;
    std::uint64_t xmm[16][2] = {};
    std::uint64_t fs_base = 0;
};

struct CpuRunResult {
    CpuStop stop = CpuStop::none;
    std::uint64_t steps = 0;
    std::uint64_t rip = 0;           // rip of the stopping instruction (or trap target)
    std::uint64_t fault_address = 0; // for memory_fault
    std::uint8_t bytes[16] = {};     // instruction bytes at rip (for unsupported_opcode / diagnostics)
    std::size_t byte_count = 0;
    std::string describe() const;
};

// Interpreter only: never executes guest bytes natively and never needs executable
// host pages, so it is valid for iOS builds without JIT entitlements.
class X86Interpreter {
public:
    explicit X86Interpreter(GuestMemory& memory) : mem_(memory) {}
    CpuState state;

    // rip values in [base, base + size) stop execution with CpuStop::trap before fetching.
    void add_trap_range(std::uint64_t base, std::uint64_t size);
    CpuRunResult run(std::uint64_t max_steps);

private:
    struct Ctx;
    struct Rm;
    bool in_trap(std::uint64_t rip) const;
    void step(CpuRunResult& out);
    GuestMemory& mem_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> traps_;
};

} // namespace bb
