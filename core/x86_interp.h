#pragma once
#include "guest_memory.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

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

// Decodificação estrutural (não executa bytes). length=0 indica instrução truncada.
// supported significa que o interpretador reconhece a família; combinações inválidas
// de prefixos/ModRM ainda podem produzir unsupported_opcode na execução.
struct X86Decoded {
    std::size_t length = 0;
    std::uint32_t opcode = 0; // 0x0fXX, 0x0f38XX, 0x0f3aXX ou 0xVMPPXX
    bool supported = false;
};
X86Decoded decode_x86(const std::uint8_t* bytes, std::size_t available);

struct CpuState {
    std::uint64_t gpr[16] = {};   // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
    std::uint64_t rip = 0;
    std::uint64_t rflags = 0x202;
    std::uint64_t xmm[16][2] = {};
    std::uint64_t ymm_hi[16][2] = {};
    std::uint64_t fs_base = 0;
    std::uint64_t gs_base = 0;
    std::uint32_t mxcsr = 0x1F80;
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

enum class AotExecutionStatus {
    completed,             // Bloco AOT executado completamente ate o fim
    fallback_untranslated, // Instrucao nao traduzida encontrada: fallback seguro para o interpretador
    trap_encountered,      // RIP de trap atingido durante execucao do bloco
    fault                  // Falha de memoria ou execucao
};

struct AotBlockResult {
    AotExecutionStatus status = AotExecutionStatus::completed;
    std::uint64_t steps = 0;
    std::uint64_t fallback_rip = 0;
};

using AotBlockHandler = std::function<AotBlockResult(CpuState& state, GuestMemory& memory)>;

// Interpreter only: never executes guest bytes natively and never needs executable
// host pages, so it is valid for iOS builds without JIT entitlements.
class X86Interpreter {
public:
    explicit X86Interpreter(GuestMemory& memory) : mem_(memory) {}
    CpuState state;
    std::uint64_t tsc() const { return tsc_; }
    void set_tsc(std::uint64_t t) { tsc_ = t; }

    // rip values in [base, base + size) stop execution with CpuStop::trap before fetching.
    void add_trap_range(std::uint64_t base, std::uint64_t size);
    using PauseChecker = std::function<bool()>;
    void set_pause_checker(PauseChecker checker) { pause_checker_ = std::move(checker); }

    // Suporte a blocos de codigo compilados AOT com fallback seguro
    void register_aot_block(std::uint64_t rip, AotBlockHandler handler);
    void unregister_aot_block(std::uint64_t rip);
    bool has_aot_block(std::uint64_t rip) const;
    void clear_aot_blocks();
    std::size_t aot_block_count() const;

    CpuRunResult run(std::uint64_t max_steps);

private:
    struct Ctx;
    struct Rm;
    bool in_trap(std::uint64_t rip) const;
    void step(CpuRunResult& out);
    GuestMemory& mem_;
    PauseChecker pause_checker_{};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> traps_;
    std::unordered_map<std::uint64_t, AotBlockHandler> aot_blocks_;
    std::uint64_t tsc_ = 0;
public:
    struct TraceItem { std::uint64_t rip, rbp, rsp; };
private:
    std::vector<TraceItem> recent_traces_;
    std::size_t recent_rips_count_ = 0;
public:
    std::vector<TraceItem> recent_traces() const {
        if (recent_traces_.empty()) return {};
        std::vector<TraceItem> res;
        std::size_t count = recent_rips_count_ < 1024 ? recent_rips_count_ : 1024;
        res.reserve(count);
        std::size_t start = recent_rips_count_ < 1024 ? 0 : (recent_rips_count_ % 1024);
        for (std::size_t i = 0; i < count; ++i) {
            res.push_back(recent_traces_[(start + i) % 1024]);
        }
        return res;
    }
};

} // namespace bb
