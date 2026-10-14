#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <map>
#include <set>
#include <memory>

namespace bb {

// ============================================================================
// 1. Existing Legacy Fixture API (100% Backwards Compatible)
// ============================================================================
std::vector<std::uint8_t> parse_hex(const std::string& input);
std::string translate(const std::vector<std::uint8_t>& bytes, const std::string& symbol);

// ============================================================================
// 2. Binary Lifting & Instruction Representation (x86-64 -> arm64)
// ============================================================================
enum class RegId : std::uint8_t {
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15,
    RIP, NONE
};

std::string reg_name(RegId reg, std::uint8_t size = 8);

enum class OperandType : std::uint8_t {
    NONE = 0,
    REG,
    IMM,
    MEM,
    REL_BRANCH
};

struct Operand {
    OperandType type = OperandType::NONE;
    RegId reg = RegId::NONE;
    std::uint8_t size = 8; // 1, 2, 4, 8 bytes
    std::int64_t imm = 0;
    RegId mem_base = RegId::NONE;
    RegId mem_index = RegId::NONE;
    std::uint8_t mem_scale = 1;
    std::int64_t mem_disp = 0;
    bool is_rip_relative = false;
    std::uint64_t rip_target = 0;

    std::string to_cpp_read() const;
    std::string to_cpp_write(const std::string& val_expr) const;
    std::string to_cpp_addr() const;
};

enum class Opcode {
    UNKNOWN = 0,
    PUSH, POP,
    MOV, MOVZX, MOVSX, LEA, XCHG, CMOV, SETCC,
    ADD, ADC, SUB, SBB, AND, OR, XOR, CMP, TEST,
    NOT, NEG, INC, DEC,
    SHL, SHR, SAR, ROL, ROR,
    IMUL, MUL, IDIV, DIV,
    JMP, JCC, CALL, RET,
    SYSCALL,
    NOP, TRAP
};

enum class ConditionCode : std::uint8_t {
    O = 0, NO, B, AE, E, NE, BE, A,
    S, NS, P, NP, L, GE, LE, G,
    ALWAYS
};

bool eval_condition(ConditionCode cc, std::uint64_t flags);
std::string condition_code_name(ConditionCode cc);

struct DecodedInstruction {
    std::uint64_t address = 0;
    std::size_t length = 0;
    Opcode opcode = Opcode::UNKNOWN;
    ConditionCode condition = ConditionCode::ALWAYS;
    std::string mnemonic;
    Operand op1;
    Operand op2;
    Operand op3;
    std::uint64_t target_vaddr = 0;
    bool is_terminator = false;
    bool is_call = false;
    bool is_indirect = false;

    std::string to_cpp_statement() const;
};

struct BasicBlock {
    std::uint64_t start_vaddr = 0;
    std::uint64_t end_vaddr = 0;
    std::vector<DecodedInstruction> instructions;
    std::vector<std::uint64_t> successors;
};

struct LiftedFunction {
    std::string name;
    std::uint64_t vaddr = 0;
    std::size_t byte_size = 0;
    std::vector<BasicBlock> blocks;
    std::vector<std::uint64_t> internal_labels;
    std::vector<std::uint64_t> called_targets;
    bool has_indirect_calls = false;
    bool is_entry_point = false;

    std::string emit_cpp() const;
};

struct AotBatchConfig {
    std::size_t max_functions = 0; // 0 = unlimited
    std::size_t functions_per_batch = 100;
    std::string output_prefix = "aot_batch_";
    std::uint64_t entry_point = 0;
    bool verbose = false;
};

struct AotBatchResult {
    bool success = false;
    std::size_t total_functions = 0;
    std::size_t total_instructions = 0;
    std::size_t batch_files_count = 0;
    std::vector<std::string> generated_files;
    std::string header_file;
    std::string registry_file;
    std::string manifest_file;
    std::string error_message;
};

// ============================================================================
// 3. Decoder, Lifter & Batch Generation Engine
// ============================================================================
DecodedInstruction decode_instruction(const std::uint8_t* code, std::size_t max_len, std::uint64_t vaddr);
LiftedFunction lift_function(const std::uint8_t* code, std::size_t max_len, std::uint64_t vaddr, const std::string& name = "");
std::vector<LiftedFunction> lift_code_section(const std::uint8_t* code, std::size_t size, std::uint64_t base_vaddr, std::uint64_t entry_point = 0, std::size_t max_functions = 0);
std::vector<LiftedFunction> lift_elf(const std::uint8_t* elf_data, std::size_t size, std::size_t max_functions = 0);

std::string emit_function_cpp(const LiftedFunction& fn);
std::string generate_batch_cpp(const std::vector<LiftedFunction>& funcs, const std::string& batch_name);
std::string generate_batch_header(const std::vector<LiftedFunction>& funcs);
std::string generate_batch_registry(const std::vector<LiftedFunction>& funcs);
std::string generate_batch_manifest(const std::vector<LiftedFunction>& funcs, const std::string& source_id = "eboot.elf");

AotBatchResult batch_translate_elf(const std::string& elf_path, const std::string& output_dir, const AotBatchConfig& config = {});

} // namespace bb
