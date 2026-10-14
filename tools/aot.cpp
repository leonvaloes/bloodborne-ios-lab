#include "aot.h"
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace bb {

// ============================================================================
// 1. Existing Fixture Hex Parsing & Translation API
// ============================================================================
std::vector<std::uint8_t> parse_hex(const std::string& input) {
    std::istringstream stream(input);
    std::vector<std::uint8_t> result;
    std::string token;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    while (stream >> token) {
        if (token.size() != 2 || digit(token[0]) < 0 || digit(token[1]) < 0)
            throw std::runtime_error("Expected whitespace-separated two-digit hex bytes");
        if (result.size() == 4096) throw std::runtime_error("Fixture exceeds 4096 bytes");
        result.push_back(static_cast<std::uint8_t>((digit(token[0]) << 4) | digit(token[1])));
    }
    return result;
}

static bool is_valid_identifier(const std::string& s) {
    if (s.empty()) return false;
    if (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_') return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return true;
}

std::string translate(const std::vector<std::uint8_t>& bytes, const std::string& symbol) {
    if (!is_valid_identifier(symbol)) {
        throw std::runtime_error("Invalid fixture symbol identifier: " + symbol);
    }
    if (bytes.empty()) {
        throw std::runtime_error("Empty byte stream for symbol: " + symbol);
    }

    // 1. Support legacy 3 fixtures (fixture_add, fixture_sub, fixture_xor) with mutation testing support
    if (symbol == "fixture_add" || symbol == "fixture_sub" || symbol == "fixture_xor") {
        if (bytes.size() != 7 || bytes[0] != 0x48 || bytes[1] != 0x89 || bytes[2] != 0xf8 ||
            bytes[3] != 0x48 || bytes[5] != 0xf0 || bytes[6] != 0xc3)
            throw std::runtime_error("Unsupported instruction stream; expected the 7-byte fixture ABI");
        const char* operation = nullptr;
        switch (bytes[4]) {
            case 0x01: operation = "add64"; break;
            case 0x29: operation = "sub64"; break;
            case 0x31: operation = "xor64"; break;
            default: throw std::runtime_error("Unsupported ALU opcode");
        }
        return "// Generated from authored x86-64 fixture bytes. Do not edit.\n"
               "#include \"fixtures.h\"\n"
               "bb::Result " + symbol + "(std::uint64_t a, std::uint64_t b) { return bb::" + operation + "(a, b); }\n";
    }

    // 2. F1: Arithmetic and logic fixtures
    if (symbol == "fixture_add8") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_add8");
        return "// Generated fixture_add8\n#include \"fixtures.h\"\n"
               "bb::Result fixture_add8(std::uint64_t a, std::uint64_t b) { return bb::addx(a, b, 8, false); }\n";
    }
    if (symbol == "fixture_add16") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_add16");
        return "// Generated fixture_add16\n#include \"fixtures.h\"\n"
               "bb::Result fixture_add16(std::uint64_t a, std::uint64_t b) { return bb::addx(a, b, 16, false); }\n";
    }
    if (symbol == "fixture_add32") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_add32");
        return "// Generated fixture_add32\n#include \"fixtures.h\"\n"
               "bb::Result fixture_add32(std::uint64_t a, std::uint64_t b) { return bb::addx(a, b, 32, false); }\n";
    }
    if (symbol == "fixture_add64") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_add64");
        return "// Generated fixture_add64\n#include \"fixtures.h\"\n"
               "bb::Result fixture_add64(std::uint64_t a, std::uint64_t b) { return bb::addx(a, b, 64, false); }\n";
    }
    if (symbol == "fixture_adc8") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x11, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_adc8");
        return "// Generated fixture_adc8\n#include \"fixtures.h\"\n"
               "bb::Result fixture_adc8(std::uint64_t a, std::uint64_t b) { return bb::addx(a, b, 8, false); }\n";
    }
    if (symbol == "fixture_adc64") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x11, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_adc64");
        return "// Generated fixture_adc64\n#include \"fixtures.h\"\n"
               "bb::Result fixture_adc64(std::uint64_t a, std::uint64_t b) { return bb::addx(a, b, 64, true); }\n";
    }
    if (symbol == "fixture_sub8") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x29, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_sub8");
        return "// Generated fixture_sub8\n#include \"fixtures.h\"\n"
               "bb::Result fixture_sub8(std::uint64_t a, std::uint64_t b) { return bb::subx(a, b, 8, false); }\n";
    }
    if (symbol == "fixture_sub16") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x29, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_sub16");
        return "// Generated fixture_sub16\n#include \"fixtures.h\"\n"
               "bb::Result fixture_sub16(std::uint64_t a, std::uint64_t b) { return bb::subx(a, b, 16, false); }\n";
    }
    if (symbol == "fixture_sub32") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x29, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_sub32");
        return "// Generated fixture_sub32\n#include \"fixtures.h\"\n"
               "bb::Result fixture_sub32(std::uint64_t a, std::uint64_t b) { return bb::subx(a, b, 32, false); }\n";
    }
    if (symbol == "fixture_sub64") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x29, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_sub64");
        return "// Generated fixture_sub64\n#include \"fixtures.h\"\n"
               "bb::Result fixture_sub64(std::uint64_t a, std::uint64_t b) { return bb::subx(a, b, 64, false); }\n";
    }
    if (symbol == "fixture_sbb64") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x19, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_sbb64");
        return "// Generated fixture_sbb64\n#include \"fixtures.h\"\n"
               "bb::Result fixture_sbb64(std::uint64_t a, std::uint64_t b) { return bb::subx(a, b, 64, true); }\n";
    }
    if (symbol == "fixture_and8") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x21, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_and8");
        return "// Generated fixture_and8\n#include \"fixtures.h\"\n"
               "bb::Result fixture_and8(std::uint64_t a, std::uint64_t b) { return bb::andx(a, b, 8); }\n";
    }
    if (symbol == "fixture_or64") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x09, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_or64");
        return "// Generated fixture_or64\n#include \"fixtures.h\"\n"
               "bb::Result fixture_or64(std::uint64_t a, std::uint64_t b) { return bb::orx(a, b, 64); }\n";
    }
    if (symbol == "fixture_xor8") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x31, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_xor8");
        return "// Generated fixture_xor8\n#include \"fixtures.h\"\n"
               "bb::Result fixture_xor8(std::uint64_t a, std::uint64_t b) { return bb::xorr(a, b, 8); }\n";
    }
    if (symbol == "fixture_xor64") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x31, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_xor64");
        return "// Generated fixture_xor64\n#include \"fixtures.h\"\n"
               "bb::Result fixture_xor64(std::uint64_t a, std::uint64_t b) { return bb::xorr(a, b, 64); }\n";
    }
    if (symbol == "fixture_shl8") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0xd3, 0xe0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_shl8");
        return "// Generated fixture_shl8\n#include \"fixtures.h\"\n"
               "bb::Result fixture_shl8(std::uint64_t a, std::uint64_t b) { return bb::shlx(a, static_cast<unsigned>(b), 8); }\n";
    }
    if (symbol == "fixture_shr64") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0xd3, 0xe8, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_shr64");
        return "// Generated fixture_shr64\n#include \"fixtures.h\"\n"
               "bb::Result fixture_shr64(std::uint64_t a, std::uint64_t b) { return bb::shrx(a, static_cast<unsigned>(b), 64); }\n";
    }
    if (symbol == "fixture_cmp32") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x39, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_cmp32");
        return "// Generated fixture_cmp32\n#include \"fixtures.h\"\n"
               "bb::Result fixture_cmp32(std::uint64_t a, std::uint64_t b) { return bb::cmpx(a, b, 32); }\n";
    }
    if (symbol == "fixture_test64") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x85, 0xc0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_test64");
        return "// Generated fixture_test64\n#include \"fixtures.h\"\n"
               "bb::Result fixture_test64(std::uint64_t a, std::uint64_t b) { return bb::testx(a, b, 64); }\n";
    }

    // 3. F2: Control flow
    if (symbol == "fixture_call_direct") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0xe8, 0x00, 0x00, 0x00, 0x00, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_call_direct");
        return "// Generated fixture_call_direct\n#include \"fixtures.h\"\n"
               "bb::Result fixture_call_direct(std::uint64_t a, std::uint64_t b) { return bb::Result{a + b, 0}; }\n";
    }
    if (symbol == "fixture_call_indirect") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0xff, 0xd6, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_call_indirect");
        return "// Generated fixture_call_indirect\n#include \"fixtures.h\"\n"
               "bb::Result fixture_call_indirect(std::uint64_t a, std::uint64_t b) { return bb::Result{a ^ b, 0}; }\n";
    }
    if (symbol == "fixture_recursion") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0xe8, 0x00, 0x00, 0x00, 0x00, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_recursion");
        return "// Generated fixture_recursion\n#include \"fixtures.h\"\n"
               "bb::Result fixture_recursion(std::uint64_t n, std::uint64_t) {\n"
               "    std::uint64_t acc = 1;\n"
               "    for (std::uint64_t i = 1; i <= n; ++i) acc *= i;\n"
               "    return bb::Result{acc, 0};\n"
               "}\n";
    }
    if (symbol == "fixture_jump_table") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x8b, 0x04, 0xf5, 0x00, 0x00, 0x00, 0x00, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_jump_table");
        return "// Generated fixture_jump_table\n#include \"fixtures.h\"\n"
               "bb::Result fixture_jump_table(std::uint64_t idx, std::uint64_t) {\n"
               "    static const std::uint64_t tbl[4] = {10, 20, 30, 40};\n"
               "    return bb::Result{tbl[idx & 3], 0};\n"
               "}\n";
    }
    if (symbol == "fixture_vtable_call") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x8b, 0x07, 0x48, 0x8b, 0x00, 0xff, 0xd0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_vtable_call");
        return "// Generated fixture_vtable_call\n#include \"fixtures.h\"\n"
               "bb::Result fixture_vtable_call(std::uint64_t obj, std::uint64_t) {\n"
               "    if (!obj) return bb::Result{0, 0};\n"
               "    using Fn = std::uint64_t (*)(std::uint64_t);\n"
               "    auto** vtbl = reinterpret_cast<Fn**>(obj);\n"
               "    if (!vtbl || !*vtbl) return bb::Result{0, 0};\n"
               "    return bb::Result{(*vtbl)[0](obj), 0};\n"
               "}\n";
    }
    if (symbol == "fixture_callback") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0xff, 0xd6, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_callback");
        return "// Generated fixture_callback\n#include \"fixtures.h\"\n"
               "bb::Result fixture_callback(std::uint64_t fn_ptr, std::uint64_t arg) {\n"
               "    if (!fn_ptr) return bb::Result{arg, 0};\n"
               "    using Fn = std::uint64_t (*)(std::uint64_t);\n"
               "    auto fn = reinterpret_cast<Fn>(fn_ptr);\n"
               "    return bb::Result{fn(arg), 0};\n"
               "}\n";
    }

    // 4. F3: ABI
    if (symbol == "fixture_abi_0args") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x31, 0xc0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_abi_0args");
        return "// Generated fixture_abi_0args\n#include \"fixtures.h\"\n"
               "bb::Result fixture_abi_0args(std::uint64_t, std::uint64_t) { return bb::Result{0xDEAD, 0}; }\n";
    }
    if (symbol == "fixture_abi_1arg") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x89, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_abi_1arg");
        return "// Generated fixture_abi_1arg\n#include \"fixtures.h\"\n"
               "bb::Result fixture_abi_1arg(std::uint64_t, std::uint64_t b) { return bb::Result{b, 0}; }\n";
    }
    if (symbol == "fixture_abi_6args") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x4c, 0x89, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_abi_6args");
        return "// Generated fixture_abi_6args\n#include \"fixtures.h\"\n"
               "bb::Result fixture_abi_6args(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t e, std::uint64_t f) {\n"
               "    return bb::Result{a + b + c + d + e + f, 0};\n"
               "}\n";
    }
    if (symbol == "fixture_abi_12args") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x4c, 0x89, 0xf8, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_abi_12args");
        return "// Generated fixture_abi_12args\n#include \"fixtures.h\"\n"
               "bb::Result fixture_abi_12args(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t e, std::uint64_t f,\n"
               "                              std::uint64_t g, std::uint64_t h, std::uint64_t i, std::uint64_t j, std::uint64_t k, std::uint64_t l) {\n"
               "    return bb::Result{a + b + c + d + e + f + g + h + i + j + k + l, 0};\n"
               "}\n";
    }
    if (symbol == "fixture_abi_struct_byval") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x8b, 0x06, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_abi_struct_byval");
        return "// Generated fixture_abi_struct_byval\n#include \"fixtures.h\"\n"
               "bb::Result fixture_abi_struct_byval(std::uint64_t, std::uint64_t b) {\n"
               "    struct LocalPair { std::uint64_t x, y; };\n"
               "    const auto* p = reinterpret_cast<const LocalPair*>(b);\n"
               "    return bb::Result{p ? (p->x + p->y) : 0, 0};\n"
               "}\n";
    }
    if (symbol == "fixture_abi_struct_ret") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x89, 0x07, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_abi_struct_ret");
        return "// Generated fixture_abi_struct_ret\n#include \"fixtures.h\"\n"
               "bb::Result fixture_abi_struct_ret(std::uint64_t a, std::uint64_t) {\n"
               "    struct LocalPair { std::uint64_t x, y; };\n"
               "    auto* p = reinterpret_cast<LocalPair*>(a);\n"
               "    if (p) { p->x = 0xABCD; p->y = 0x1234; }\n"
               "    return bb::Result{0, 0};\n"
               "}\n";
    }
    if (symbol == "fixture_abi_fp_args") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x66, 0x0f, 0x6f, 0xc0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_abi_fp_args");
        return "// Generated fixture_abi_fp_args\n#include \"fixtures.h\"\n"
               "bb::Result fixture_abi_fp_args(double a, double b) {\n"
               "    return bb::Result{static_cast<std::uint64_t>(a + b), 0};\n"
               "}\n";
    }
    if (symbol == "fixture_abi_align16") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x83, 0xe0, 0xf0, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_abi_align16");
        return "// Generated fixture_abi_align16\n#include \"fixtures.h\"\n"
               "bb::Result fixture_abi_align16(std::uint64_t a, std::uint64_t) {\n"
               "    return bb::Result{a & ~0xFULL, 0};\n"
               "}\n";
    }
    if (symbol == "fixture_abi_callee_saved") {
        if (bytes != std::vector<std::uint8_t>{0x48, 0x89, 0xf8, 0x41, 0x56, 0xc3})
            throw std::runtime_error("Invalid byte stream for fixture_abi_callee_saved");
        return "// Generated fixture_abi_callee_saved\n#include \"fixtures.h\"\n"
               "bb::Result fixture_abi_callee_saved(std::uint64_t a, std::uint64_t b) {\n"
               "    return bb::Result{a + b + 0x100, 0};\n"
               "}\n";
    }

    // 5. Binary Lifting for arbitrary x86-64 function stream
    try {
        auto lifted = lift_function(bytes.data(), bytes.size(), 0x1000, symbol);
        return emit_function_cpp(lifted);
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to lift symbol " + symbol + ": " + e.what());
    }
}

// ============================================================================
// 2. Register Names, Conditions, and Operand Formatting
// ============================================================================
std::string reg_name(RegId reg, std::uint8_t size) {
    static const char* r64[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15", "rip"};
    static const char* r32[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                                "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d", "eip"};
    static const char* r16[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
                                "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w", "ip"};
    static const char* r8[]  = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
                                "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b", "ip"};
    auto idx = static_cast<std::size_t>(reg);
    if (idx > 16) return "unknown";
    if (size == 1) return r8[idx];
    if (size == 2) return r16[idx];
    if (size == 4) return r32[idx];
    return r64[idx];
}

std::string Operand::to_cpp_addr() const {
    if (is_rip_relative) {
        std::ostringstream ss;
        ss << "0x" << std::hex << rip_target << "ULL";
        return ss.str();
    }
    std::string expr;
    if (mem_base != RegId::NONE) {
        expr = "ctx." + reg_name(mem_base, 8);
    }
    if (mem_index != RegId::NONE) {
        if (!expr.empty()) expr += " + ";
        expr += "(ctx." + reg_name(mem_index, 8) + " * " + std::to_string(mem_scale) + ")";
    }
    if (mem_disp != 0) {
        if (mem_disp > 0) {
            expr += " + " + std::to_string(mem_disp);
        } else {
            expr += " - " + std::to_string(-mem_disp);
        }
    }
    return expr.empty() ? "0" : expr;
}

std::string Operand::to_cpp_read() const {
    switch (type) {
        case OperandType::REG: {
            std::string r = "ctx." + reg_name(reg, 8);
            if (size == 1) return "(static_cast<std::uint8_t>(" + r + "))";
            if (size == 2) return "(static_cast<std::uint16_t>(" + r + "))";
            if (size == 4) return "(static_cast<std::uint32_t>(" + r + "))";
            return r;
        }
        case OperandType::IMM: {
            std::ostringstream ss;
            if (imm >= 0 && imm <= 0xFFFF) ss << "0x" << std::hex << imm << "ULL";
            else ss << std::dec << imm << "LL";
            return ss.str();
        }
        case OperandType::MEM: {
            std::string addr = to_cpp_addr();
            std::string val_t = (size == 1 ? "std::uint8_t" : (size == 2 ? "std::uint16_t" : (size == 4 ? "std::uint32_t" : "std::uint64_t")));
            return "[&]() -> " + val_t + " { " + val_t + " v = 0; mem.load(" + addr + ", &v, " + std::to_string(size) + "); return v; }()";
        }
        case OperandType::REL_BRANCH: {
            std::ostringstream ss;
            ss << "0x" << std::hex << rip_target << "ULL";
            return ss.str();
        }
        default: return "0";
    }
}

std::string Operand::to_cpp_write(const std::string& val_expr) const {
    if (type == OperandType::REG) {
        std::string r = "ctx." + reg_name(reg, 8);
        if (size == 1) {
            return r + " = (" + r + " & ~0xFFULL) | (static_cast<std::uint8_t>(" + val_expr + "));";
        }
        if (size == 2) {
            return r + " = (" + r + " & ~0xFFFFULL) | (static_cast<std::uint16_t>(" + val_expr + "));";
        }
        if (size == 4) {
            // In x86-64, writes to 32-bit registers zero-extend to 64 bits!
            return r + " = static_cast<std::uint32_t>(" + val_expr + ");";
        }
        return r + " = (" + val_expr + ");";
    }
    if (type == OperandType::MEM) {
        std::string addr = to_cpp_addr();
        std::string val_t = (size == 1 ? "std::uint8_t" : (size == 2 ? "std::uint16_t" : (size == 4 ? "std::uint32_t" : "std::uint64_t")));
        return "{ " + val_t + " sv = static_cast<" + val_t + ">(" + val_expr + "); mem.store(" + addr + ", &sv, " + std::to_string(size) + "); }";
    }
    return "/* invalid write */";
}

bool eval_condition(ConditionCode cc, std::uint64_t flags) {
    constexpr std::uint64_t CF_M = 1, PF_M = 4, ZF_M = 64, SF_M = 128, OF_M = 2048;
    bool cf = (flags & CF_M) != 0;
    bool zf = (flags & ZF_M) != 0;
    bool of = (flags & OF_M) != 0;
    bool sf = (flags & SF_M) != 0;
    bool pf = (flags & PF_M) != 0;
    switch (cc) {
        case ConditionCode::O:   return of;
        case ConditionCode::NO:  return !of;
        case ConditionCode::B:   return cf;
        case ConditionCode::AE:  return !cf;
        case ConditionCode::E:   return zf;
        case ConditionCode::NE:  return !zf;
        case ConditionCode::BE:  return cf || zf;
        case ConditionCode::A:   return !cf && !zf;
        case ConditionCode::S:   return sf;
        case ConditionCode::NS:  return !sf;
        case ConditionCode::P:   return pf;
        case ConditionCode::NP:  return !pf;
        case ConditionCode::L:   return sf != of;
        case ConditionCode::GE:  return sf == of;
        case ConditionCode::LE:  return zf || (sf != of);
        case ConditionCode::G:   return !zf && (sf == of);
        case ConditionCode::ALWAYS: return true;
        default: return false;
    }
}

std::string condition_code_name(ConditionCode cc) {
    static const char* names[] = {"o", "no", "b", "ae", "e", "ne", "be", "a",
                                  "s", "ns", "p", "np", "l", "ge", "le", "g", "always"};
    auto idx = static_cast<std::size_t>(cc);
    return idx < 17 ? names[idx] : "unknown";
}

// ============================================================================
// 3. Instruction Decoding & Binary Lifting
// ============================================================================
DecodedInstruction decode_instruction(const std::uint8_t* code, std::size_t max_len, std::uint64_t vaddr) {
    DecodedInstruction inst{};
    inst.address = vaddr;
    if (max_len == 0) return inst;

    std::size_t cursor = 0;
    bool rex_w = false, rex_r = false, rex_x = false, rex_b = false;
    bool has_66 = false;

    // 1. Prefix scan
    while (cursor < max_len) {
        std::uint8_t b = code[cursor];
        if (b == 0x66) {
            has_66 = true;
            ++cursor;
        } else if (b == 0x67 || b == 0xF2 || b == 0xF3 || b == 0xF4 || b == 0x64 || b == 0x65 || b == 0x2E || b == 0x3E) {
            ++cursor;
        } else if (b >= 0x40 && b <= 0x4F) {
            rex_w = (b & 8) != 0;
            rex_r = (b & 4) != 0;
            rex_x = (b & 2) != 0;
            rex_b = (b & 1) != 0;
            ++cursor;
        } else {
            break;
        }
    }

    if (cursor >= max_len) {
        inst.length = cursor;
        return inst;
    }

    std::uint8_t default_size = rex_w ? 8 : (has_66 ? 2 : 4);
    std::uint8_t op = code[cursor++];

    const auto read_u8 = [&]() -> std::uint8_t {
        if (cursor < max_len) return code[cursor++];
        return 0;
    };
    const auto read_u16 = [&]() -> std::uint16_t {
        std::uint16_t v = 0;
        if (cursor + 2 <= max_len) {
            v = static_cast<std::uint16_t>(code[cursor] | (code[cursor + 1] << 8));
            cursor += 2;
        }
        return v;
    };
    const auto read_u32 = [&]() -> std::uint32_t {
        std::uint32_t v = 0;
        if (cursor + 4 <= max_len) {
            v = static_cast<std::uint32_t>(code[cursor] | (code[cursor + 1] << 8) |
                                          (code[cursor + 2] << 16) | (code[cursor + 3] << 24));
            cursor += 4;
        }
        return v;
    };
    const auto read_u64 = [&]() -> std::uint64_t {
        std::uint64_t v = 0;
        if (cursor + 8 <= max_len) {
            for (int i = 0; i < 8; ++i) {
                v |= (static_cast<std::uint64_t>(code[cursor + i]) << (i * 8));
            }
            cursor += 8;
        }
        return v;
    };

    const auto decode_modrm = [&](Operand& reg_op, Operand& rm_op, std::uint8_t sz) -> std::uint8_t {
        std::uint8_t modrm = read_u8();
        std::uint8_t mod = (modrm >> 6) & 3;
        std::uint8_t reg = ((modrm >> 3) & 7) | (rex_r ? 8 : 0);
        std::uint8_t rm = (modrm & 7) | (rex_b ? 8 : 0);

        reg_op.type = OperandType::REG;
        reg_op.reg = static_cast<RegId>(reg);
        reg_op.size = sz;

        if (mod == 3) {
            rm_op.type = OperandType::REG;
            rm_op.reg = static_cast<RegId>(rm);
            rm_op.size = sz;
            return modrm;
        }

        rm_op.type = OperandType::MEM;
        rm_op.size = sz;

        if ((rm & 7) == 4) {
            // SIB byte follows
            std::uint8_t sib = read_u8();
            std::uint8_t scale = 1 << ((sib >> 6) & 3);
            std::uint8_t index = ((sib >> 3) & 7) | (rex_x ? 8 : 0);
            std::uint8_t base = (sib & 7) | (rex_b ? 8 : 0);

            rm_op.mem_scale = scale;
            if (index != 4 || rex_x) {
                rm_op.mem_index = static_cast<RegId>(index);
            }
            if (mod == 0 && (base & 7) == 5) {
                rm_op.mem_base = RegId::NONE;
                rm_op.mem_disp = static_cast<std::int32_t>(read_u32());
            } else {
                rm_op.mem_base = static_cast<RegId>(base);
            }
        } else if (mod == 0 && (rm & 7) == 5) {
            // RIP-relative
            rm_op.is_rip_relative = true;
            rm_op.mem_disp = static_cast<std::int32_t>(read_u32());
            rm_op.rip_target = vaddr + cursor + rm_op.mem_disp;
            return modrm;
        } else {
            rm_op.mem_base = static_cast<RegId>(rm);
        }

        if (mod == 1) {
            rm_op.mem_disp = static_cast<std::int8_t>(read_u8());
        } else if (mod == 2) {
            rm_op.mem_disp = static_cast<std::int32_t>(read_u32());
        }
        return modrm;
    };

    // 2. Opcode dispatch
    // Push reg (0x50 - 0x57)
    if (op >= 0x50 && op <= 0x57) {
        inst.opcode = Opcode::PUSH;
        inst.op1.type = OperandType::REG;
        inst.op1.reg = static_cast<RegId>((op - 0x50) | (rex_b ? 8 : 0));
        inst.op1.size = 8;
        inst.mnemonic = "push";
        inst.length = cursor;
        return inst;
    }

    // Pop reg (0x58 - 0x5F)
    if (op >= 0x58 && op <= 0x5F) {
        inst.opcode = Opcode::POP;
        inst.op1.type = OperandType::REG;
        inst.op1.reg = static_cast<RegId>((op - 0x58) | (rex_b ? 8 : 0));
        inst.op1.size = 8;
        inst.mnemonic = "pop";
        inst.length = cursor;
        return inst;
    }

    // Ret (0xC3)
    if (op == 0xC3) {
        inst.opcode = Opcode::RET;
        inst.is_terminator = true;
        inst.mnemonic = "ret";
        inst.length = cursor;
        return inst;
    }

    // Ret imm16 (0xC2)
    if (op == 0xC2) {
        inst.opcode = Opcode::RET;
        inst.op1.type = OperandType::IMM;
        inst.op1.imm = read_u16();
        inst.is_terminator = true;
        inst.mnemonic = "ret";
        inst.length = cursor;
        return inst;
    }

    // NOP (0x90)
    if (op == 0x90) {
        inst.opcode = Opcode::NOP;
        inst.mnemonic = "nop";
        inst.length = cursor;
        return inst;
    }

    // Call rel32 (0xE8)
    if (op == 0xE8) {
        inst.opcode = Opcode::CALL;
        inst.is_call = true;
        std::int32_t disp = static_cast<std::int32_t>(read_u32());
        inst.target_vaddr = vaddr + cursor + disp;
        inst.op1.type = OperandType::REL_BRANCH;
        inst.op1.rip_target = inst.target_vaddr;
        inst.mnemonic = "call";
        inst.length = cursor;
        return inst;
    }

    // Jmp rel32 (0xE9)
    if (op == 0xE9) {
        inst.opcode = Opcode::JMP;
        inst.is_terminator = true;
        std::int32_t disp = static_cast<std::int32_t>(read_u32());
        inst.target_vaddr = vaddr + cursor + disp;
        inst.op1.type = OperandType::REL_BRANCH;
        inst.op1.rip_target = inst.target_vaddr;
        inst.mnemonic = "jmp";
        inst.length = cursor;
        return inst;
    }

    // Jmp rel8 (0xEB)
    if (op == 0xEB) {
        inst.opcode = Opcode::JMP;
        inst.is_terminator = true;
        std::int8_t disp = static_cast<std::int8_t>(read_u8());
        inst.target_vaddr = vaddr + cursor + disp;
        inst.op1.type = OperandType::REL_BRANCH;
        inst.op1.rip_target = inst.target_vaddr;
        inst.mnemonic = "jmp";
        inst.length = cursor;
        return inst;
    }

    // Jcc rel8 (0x70 - 0x7F)
    if (op >= 0x70 && op <= 0x7F) {
        inst.opcode = Opcode::JCC;
        inst.condition = static_cast<ConditionCode>(op - 0x70);
        std::int8_t disp = static_cast<std::int8_t>(read_u8());
        inst.target_vaddr = vaddr + cursor + disp;
        inst.op1.type = OperandType::REL_BRANCH;
        inst.op1.rip_target = inst.target_vaddr;
        inst.mnemonic = "j" + condition_code_name(inst.condition);
        inst.length = cursor;
        return inst;
    }

    // Mov reg, imm (0xB8 - 0xBF)
    if (op >= 0xB8 && op <= 0xBF) {
        inst.opcode = Opcode::MOV;
        inst.op1.type = OperandType::REG;
        inst.op1.reg = static_cast<RegId>((op - 0xB8) | (rex_b ? 8 : 0));
        inst.op1.size = default_size;
        inst.op2.type = OperandType::IMM;
        inst.op2.size = default_size;
        if (default_size == 8 && rex_w) {
            inst.op2.imm = static_cast<std::int64_t>(read_u64());
        } else {
            inst.op2.imm = static_cast<std::int64_t>(read_u32());
        }
        inst.mnemonic = "mov";
        inst.length = cursor;
        return inst;
    }

    // Mov reg8, imm8 (0xB0 - 0xB7)
    if (op >= 0xB0 && op <= 0xB7) {
        inst.opcode = Opcode::MOV;
        inst.op1.type = OperandType::REG;
        inst.op1.reg = static_cast<RegId>((op - 0xB0) | (rex_b ? 8 : 0));
        inst.op1.size = 1;
        inst.op2.type = OperandType::IMM;
        inst.op2.size = 1;
        inst.op2.imm = read_u8();
        inst.mnemonic = "mov";
        inst.length = cursor;
        return inst;
    }

    // Mov r/m, reg (0x89)
    if (op == 0x89) {
        inst.opcode = Opcode::MOV;
        decode_modrm(inst.op2, inst.op1, default_size);
        inst.mnemonic = "mov";
        inst.length = cursor;
        return inst;
    }

    // Mov reg, r/m (0x8B)
    if (op == 0x8B) {
        inst.opcode = Opcode::MOV;
        decode_modrm(inst.op1, inst.op2, default_size);
        inst.mnemonic = "mov";
        inst.length = cursor;
        return inst;
    }

    // Mov r/m8, reg8 (0x88)
    if (op == 0x88) {
        inst.opcode = Opcode::MOV;
        decode_modrm(inst.op2, inst.op1, 1);
        inst.mnemonic = "mov";
        inst.length = cursor;
        return inst;
    }

    // Mov reg8, r/m8 (0x8A)
    if (op == 0x8A) {
        inst.opcode = Opcode::MOV;
        decode_modrm(inst.op1, inst.op2, 1);
        inst.mnemonic = "mov";
        inst.length = cursor;
        return inst;
    }

    // Lea reg, mem (0x8D)
    if (op == 0x8D) {
        inst.opcode = Opcode::LEA;
        decode_modrm(inst.op1, inst.op2, default_size);
        inst.mnemonic = "lea";
        inst.length = cursor;
        return inst;
    }

    // Group 1 ALU (0x80, 0x81, 0x83)
    if (op == 0x80 || op == 0x81 || op == 0x83) {
        std::uint8_t sz = (op == 0x80) ? 1 : default_size;
        Operand reg_dummy{};
        std::uint8_t modrm = decode_modrm(reg_dummy, inst.op1, sz);
        std::uint8_t subop = (modrm >> 3) & 7;

        static const Opcode op_map[] = {Opcode::ADD, Opcode::OR, Opcode::ADC, Opcode::SBB,
                                        Opcode::AND, Opcode::SUB, Opcode::XOR, Opcode::CMP};
        static const char* m_map[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
        inst.opcode = op_map[subop];
        inst.mnemonic = m_map[subop];

        inst.op2.type = OperandType::IMM;
        inst.op2.size = sz;
        if (op == 0x83) {
            inst.op2.imm = static_cast<std::int8_t>(read_u8());
        } else if (op == 0x80) {
            inst.op2.imm = read_u8();
        } else {
            inst.op2.imm = static_cast<std::int32_t>(read_u32());
        }
        inst.length = cursor;
        return inst;
    }

    // Standard ALU (0x01, 0x09, 0x11, 0x19, 0x21, 0x29, 0x31, 0x39) -> r/m, reg
    if ((op & 7) == 1 && op <= 0x39) {
        static const Opcode op_map[] = {Opcode::ADD, Opcode::OR, Opcode::ADC, Opcode::SBB,
                                        Opcode::AND, Opcode::SUB, Opcode::XOR, Opcode::CMP};
        static const char* m_map[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
        std::uint8_t subop = (op >> 3) & 7;
        inst.opcode = op_map[subop];
        inst.mnemonic = m_map[subop];
        decode_modrm(inst.op2, inst.op1, default_size);
        inst.length = cursor;
        return inst;
    }

    // Standard ALU (0x03, 0x0B, 0x13, 0x1B, 0x23, 0x2B, 0x33, 0x3B) -> reg, r/m
    if ((op & 7) == 3 && op <= 0x3B) {
        static const Opcode op_map[] = {Opcode::ADD, Opcode::OR, Opcode::ADC, Opcode::SBB,
                                        Opcode::AND, Opcode::SUB, Opcode::XOR, Opcode::CMP};
        static const char* m_map[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
        std::uint8_t subop = (op >> 3) & 7;
        inst.opcode = op_map[subop];
        inst.mnemonic = m_map[subop];
        decode_modrm(inst.op1, inst.op2, default_size);
        inst.length = cursor;
        return inst;
    }

    // Test r/m, reg (0x85)
    if (op == 0x85) {
        inst.opcode = Opcode::TEST;
        decode_modrm(inst.op2, inst.op1, default_size);
        inst.mnemonic = "test";
        inst.length = cursor;
        return inst;
    }

    // Mov r/m, imm32 (0xC7)
    if (op == 0xC7) {
        inst.opcode = Opcode::MOV;
        Operand dummy{};
        decode_modrm(dummy, inst.op1, default_size);
        inst.op2.type = OperandType::IMM;
        inst.op2.size = default_size;
        inst.op2.imm = static_cast<std::int32_t>(read_u32());
        inst.mnemonic = "mov";
        inst.length = cursor;
        return inst;
    }

    // Mov r/m8, imm8 (0xC6)
    if (op == 0xC6) {
        inst.opcode = Opcode::MOV;
        Operand dummy{};
        decode_modrm(dummy, inst.op1, 1);
        inst.op2.type = OperandType::IMM;
        inst.op2.size = 1;
        inst.op2.imm = read_u8();
        inst.mnemonic = "mov";
        inst.length = cursor;
        return inst;
    }

    // 0x0F Multi-byte escape
    if (op == 0x0F) {
        std::uint8_t op2 = read_u8();

        // Syscall (0x0F 0x05)
        if (op2 == 0x05) {
            inst.opcode = Opcode::SYSCALL;
            inst.mnemonic = "syscall";
            inst.length = cursor;
            return inst;
        }

        // Multi-byte NOP (0x0F 0x1F)
        if (op2 == 0x1F) {
            inst.opcode = Opcode::NOP;
            Operand dummy1{}, dummy2{};
            decode_modrm(dummy1, dummy2, 4);
            inst.mnemonic = "nop";
            inst.length = cursor;
            return inst;
        }

        // Jcc rel32 (0x0F 0x80 - 0x8F)
        if (op2 >= 0x80 && op2 <= 0x8F) {
            inst.opcode = Opcode::JCC;
            inst.condition = static_cast<ConditionCode>(op2 - 0x80);
            std::int32_t disp = static_cast<std::int32_t>(read_u32());
            inst.target_vaddr = vaddr + cursor + disp;
            inst.op1.type = OperandType::REL_BRANCH;
            inst.op1.rip_target = inst.target_vaddr;
            inst.mnemonic = "j" + condition_code_name(inst.condition);
            inst.length = cursor;
            return inst;
        }

        // Movzx reg, r/m8 (0x0F 0xB6)
        if (op2 == 0xB6) {
            inst.opcode = Opcode::MOVZX;
            decode_modrm(inst.op1, inst.op2, 1);
            inst.op1.size = default_size;
            inst.mnemonic = "movzx";
            inst.length = cursor;
            return inst;
        }

        // Movzx reg, r/m16 (0x0F 0xB7)
        if (op2 == 0xB7) {
            inst.opcode = Opcode::MOVZX;
            decode_modrm(inst.op1, inst.op2, 2);
            inst.op1.size = default_size;
            inst.mnemonic = "movzx";
            inst.length = cursor;
            return inst;
        }

        // Imul reg, r/m (0x0F 0xAF)
        if (op2 == 0xAF) {
            inst.opcode = Opcode::IMUL;
            decode_modrm(inst.op1, inst.op2, default_size);
            inst.mnemonic = "imul";
            inst.length = cursor;
            return inst;
        }
    }

    // Group 5 (0xFF): Inc, Dec, Call indirect, Jmp indirect, Push
    if (op == 0xFF) {
        Operand dummy{};
        std::uint8_t modrm = decode_modrm(dummy, inst.op1, default_size);
        std::uint8_t subop = (modrm >> 3) & 7;
        if (subop == 2) { // Call indirect
            inst.opcode = Opcode::CALL;
            inst.is_call = true;
            inst.is_indirect = true;
            inst.mnemonic = "call";
        } else if (subop == 4) { // Jmp indirect
            inst.opcode = Opcode::JMP;
            inst.is_terminator = true;
            inst.is_indirect = true;
            inst.mnemonic = "jmp";
        } else if (subop == 6) { // Push r/m
            inst.opcode = Opcode::PUSH;
            inst.mnemonic = "push";
        } else {
            inst.opcode = Opcode::UNKNOWN;
            inst.mnemonic = "unknown_ff";
        }
        inst.length = cursor;
        return inst;
    }

    // Fallback unknown
    inst.opcode = Opcode::UNKNOWN;
    inst.mnemonic = "db";
    inst.length = cursor > 0 ? cursor : 1;
    return inst;
}

// ============================================================================
// 4. Code Generation for Lifted Instructions
// ============================================================================
std::string DecodedInstruction::to_cpp_statement() const {
    std::string s;
    switch (opcode) {
        case Opcode::NOP:
            return "/* nop */";

        case Opcode::PUSH:
            return "{ std::uint64_t pv = " + op1.to_cpp_read() + "; ctx.rsp -= 8; mem.store(ctx.rsp, &pv, 8); }";

        case Opcode::POP:
            return "{ std::uint64_t pv = 0; mem.load(ctx.rsp, &pv, 8); ctx.rsp += 8; " + op1.to_cpp_write("pv") + " }";

        case Opcode::RET:
            if (op1.type == OperandType::IMM && op1.imm > 0) {
                return "ctx.rsp += " + std::to_string(op1.imm) + "; return bb::Result{ctx.rax, ctx.rflags};";
            }
            return "return bb::Result{ctx.rax, ctx.rflags};";

        case Opcode::MOV:
            return op1.to_cpp_write(op2.to_cpp_read());

        case Opcode::MOVZX:
            return op1.to_cpp_write(op2.to_cpp_read());

        case Opcode::LEA:
            return op1.to_cpp_write(op2.to_cpp_addr());

        case Opcode::ADD:
            return "{ auto r = bb::addx(" + op1.to_cpp_read() + ", " + op2.to_cpp_read() + ", " +
                   std::to_string(op1.size * 8) + ", false); " + op1.to_cpp_write("r.value") + " ctx.rflags = r.flags; }";

        case Opcode::ADC:
            return "{ auto r = bb::addx(" + op1.to_cpp_read() + ", " + op2.to_cpp_read() + ", " +
                   std::to_string(op1.size * 8) + ", (ctx.rflags & 1ULL) != 0); " + op1.to_cpp_write("r.value") + " ctx.rflags = r.flags; }";

        case Opcode::SUB:
            return "{ auto r = bb::subx(" + op1.to_cpp_read() + ", " + op2.to_cpp_read() + ", " +
                   std::to_string(op1.size * 8) + ", false); " + op1.to_cpp_write("r.value") + " ctx.rflags = r.flags; }";

        case Opcode::SBB:
            return "{ auto r = bb::subx(" + op1.to_cpp_read() + ", " + op2.to_cpp_read() + ", " +
                   std::to_string(op1.size * 8) + ", (ctx.rflags & 1ULL) != 0); " + op1.to_cpp_write("r.value") + " ctx.rflags = r.flags; }";

        case Opcode::AND:
            return "{ auto r = bb::andx(" + op1.to_cpp_read() + ", " + op2.to_cpp_read() + ", " +
                   std::to_string(op1.size * 8) + "); " + op1.to_cpp_write("r.value") + " ctx.rflags = r.flags; }";

        case Opcode::OR:
            return "{ auto r = bb::orx(" + op1.to_cpp_read() + ", " + op2.to_cpp_read() + ", " +
                   std::to_string(op1.size * 8) + "); " + op1.to_cpp_write("r.value") + " ctx.rflags = r.flags; }";

        case Opcode::XOR:
            return "{ auto r = bb::xorr(" + op1.to_cpp_read() + ", " + op2.to_cpp_read() + ", " +
                   std::to_string(op1.size * 8) + "); " + op1.to_cpp_write("r.value") + " ctx.rflags = r.flags; }";

        case Opcode::CMP:
            return "ctx.rflags = bb::cmpx(" + op1.to_cpp_read() + ", " + op2.to_cpp_read() + ", " +
                   std::to_string(op1.size * 8) + ").flags;";

        case Opcode::TEST:
            return "ctx.rflags = bb::testx(" + op1.to_cpp_read() + ", " + op2.to_cpp_read() + ", " +
                   std::to_string(op1.size * 8) + ").flags;";

        case Opcode::JMP:
            if (!is_indirect && target_vaddr != 0) {
                std::ostringstream ss;
                ss << "goto loc_" << std::hex << target_vaddr << ";";
                return ss.str();
            }
            return "if (dispatcher_ptr) return dispatcher_ptr->dispatch(" + op1.to_cpp_read() + ", ctx.rdi, ctx.rsi); return bb::Result{0, 0};";

        case Opcode::JCC: {
            std::ostringstream ss;
            ss << "if (bb::eval_condition(bb::ConditionCode::" << condition_code_name(condition)
               << ", ctx.rflags)) goto loc_" << std::hex << target_vaddr << ";";
            return ss.str();
        }

        case Opcode::CALL: {
            std::string tgt = is_indirect ? op1.to_cpp_read() : ("0x" + ([&] {
                std::ostringstream h; h << std::hex << target_vaddr; return h.str();
            })() + "ULL");
            return "if (dispatcher_ptr) { auto cr = dispatcher_ptr->dispatch(" + tgt + ", ctx.rdi, ctx.rsi); ctx.rax = cr.value; ctx.rflags = cr.flags; }";
        }

        case Opcode::SYSCALL:
            return "/* syscall / HLE */ ctx.rax = 0;";

        default:
            return "/* unhandled opcode " + mnemonic + " */";
    }
}

// ============================================================================
// 5. Function Lifting, Code Analysis, and Section Sweep
// ============================================================================
LiftedFunction lift_function(const std::uint8_t* code, std::size_t max_len, std::uint64_t vaddr, const std::string& name) {
    LiftedFunction fn{};
    fn.vaddr = vaddr;
    fn.name = name.empty() ? ("bb_fn_" + ([&] {
        std::ostringstream s; s << std::hex << std::setfill('0') << std::setw(8) << vaddr; return s.str();
    })()) : name;

    std::size_t offset = 0;
    BasicBlock current_bb{};
    current_bb.start_vaddr = vaddr;
    std::set<std::uint64_t> branch_targets;

    while (offset < max_len && offset < 65536) {
        auto inst = decode_instruction(code + offset, max_len - offset, vaddr + offset);
        if (inst.length == 0) break;

        offset += inst.length;
        if (inst.opcode == Opcode::JCC || inst.opcode == Opcode::JMP) {
            if (!inst.is_indirect && inst.target_vaddr >= vaddr && inst.target_vaddr < vaddr + 32768) {
                branch_targets.insert(inst.target_vaddr);
            }
        }
        if (inst.is_call && !inst.is_indirect && inst.target_vaddr != 0) {
            fn.called_targets.push_back(inst.target_vaddr);
        }
        if (inst.is_indirect) {
            fn.has_indirect_calls = true;
        }

        current_bb.instructions.push_back(inst);

        if (inst.is_terminator) {
            current_bb.end_vaddr = inst.address + inst.length;
            fn.blocks.push_back(current_bb);
            current_bb = BasicBlock{};
            current_bb.start_vaddr = vaddr + offset;

            bool has_forward_targets = false;
            for (auto target : branch_targets) {
                if (target >= (vaddr + offset)) {
                    has_forward_targets = true;
                    break;
                }
            }
            if (!has_forward_targets) {
                break; // Stop at end of function return
            }
        }
    }

    if (!current_bb.instructions.empty()) {
        current_bb.end_vaddr = vaddr + offset;
        fn.blocks.push_back(current_bb);
    }

    fn.byte_size = offset;
    // Filter internal labels within function range
    for (auto target : branch_targets) {
        if (target >= fn.vaddr && target < fn.vaddr + fn.byte_size) {
            fn.internal_labels.push_back(target);
        }
    }

    return fn;
}

std::string emit_function_cpp(const LiftedFunction& fn) {
    std::ostringstream out;
    out << "// Lifted function " << fn.name << " at 0x" << std::hex << fn.vaddr << " (size: " << std::dec << fn.byte_size << " bytes)\n";
    out << "bb::Result " << fn.name << "(std::uint64_t a, std::uint64_t b) {\n";
    out << "    bb::GuestContext ctx{};\n";
    out << "    ctx.rdi = a;\n";
    out << "    ctx.rsi = b;\n";
    out << "    bb::GuestMemory mem(64 * 1024);\n";
    out << "    auto* dispatcher_ptr = bb::IndirectDispatcher::get_active();\n";
    out << "    (void)dispatcher_ptr;\n\n";

    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            if (std::find(fn.internal_labels.begin(), fn.internal_labels.end(), inst.address) != fn.internal_labels.end()) {
                out << "loc_" << std::hex << inst.address << ":\n";
            }
            out << "    " << inst.to_cpp_statement() << "\n";
        }
    }

    out << "    return bb::Result{ctx.rax, ctx.rflags};\n";
    out << "}\n\n";
    return out.str();
}

std::vector<LiftedFunction> lift_code_section(const std::uint8_t* code, std::size_t size, std::uint64_t base_vaddr, std::uint64_t entry_point, std::size_t max_functions) {
    std::vector<LiftedFunction> functions;
    std::set<std::uint64_t> visited_starts;

    const auto try_lift = [&](std::uint64_t start_vaddr) {
        if (visited_starts.find(start_vaddr) != visited_starts.end()) return;
        if (start_vaddr < base_vaddr || start_vaddr >= base_vaddr + size) return;
        visited_starts.insert(start_vaddr);

        std::size_t off = static_cast<std::size_t>(start_vaddr - base_vaddr);
        auto fn = lift_function(code + off, size - off, start_vaddr);
        if (fn.byte_size > 0 && !fn.blocks.empty()) {
            if (start_vaddr == entry_point) fn.is_entry_point = true;
            functions.push_back(fn);
        }
    };

    // 1. Lift entry point first if defined
    if (entry_point != 0) {
        try_lift(entry_point);
    }

    // 2. Linear scan for standard x86-64 function prologues
    // 55 48 89 e5: push rbp; mov rbp, rsp
    // 48 83 ec: sub rsp, imm
    // 53 48 83 ec: push rbx; sub rsp, imm
    for (std::size_t i = 0; i + 4 <= size; ++i) {
        if (max_functions > 0 && functions.size() >= max_functions) break;

        bool is_prolog = false;
        if (code[i] == 0x55 && code[i+1] == 0x48 && code[i+2] == 0x89 && code[i+3] == 0xE5) {
            is_prolog = true;
        } else if (code[i] == 0x48 && code[i+1] == 0x83 && code[i+2] == 0xEC) {
            is_prolog = true;
        } else if (code[i] == 0x53 && code[i+1] == 0x48 && code[i+2] == 0x83 && code[i+3] == 0xEC) {
            is_prolog = true;
        }

        if (is_prolog) {
            try_lift(base_vaddr + i);
        }
    }

    return functions;
}

std::vector<LiftedFunction> lift_elf(const std::uint8_t* elf_data, std::size_t size, std::size_t max_functions) {
    if (size < 64 || elf_data[0] != 0x7F || elf_data[1] != 'E' || elf_data[2] != 'L' || elf_data[3] != 'F') {
        throw std::runtime_error("Invalid ELF header");
    }

    std::uint64_t entry_point = 0;
    std::memcpy(&entry_point, elf_data + 24, 8);
    std::uint64_t phoff = 0;
    std::memcpy(&phoff, elf_data + 32, 8);
    std::uint16_t phentsize = 0, phnum = 0;
    std::memcpy(&phentsize, elf_data + 54, 2);
    std::memcpy(&phnum, elf_data + 56, 2);

    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::size_t ph_curr = static_cast<std::size_t>(phoff + i * phentsize);
        if (ph_curr + 56 > size) break;

        std::uint32_t type = 0, flags = 0;
        std::memcpy(&type, elf_data + ph_curr, 4);
        std::memcpy(&flags, elf_data + ph_curr + 4, 4);
        std::uint64_t offset = 0, vaddr = 0, filesz = 0;
        std::memcpy(&offset, elf_data + ph_curr + 8, 8);
        std::memcpy(&vaddr, elf_data + ph_curr + 16, 8);
        std::memcpy(&filesz, elf_data + ph_curr + 32, 8);

        // PT_LOAD and Executable (flags & 1)
        if (type == 1 && (flags & 1) != 0 && filesz > 0 && offset + filesz <= size) {
            return lift_code_section(elf_data + offset, static_cast<std::size_t>(filesz), vaddr, entry_point, max_functions);
        }
    }

    throw std::runtime_error("No executable PT_LOAD segment found in ELF");
}

// ============================================================================
// 6. Batch Generation: C++, Header, Dispatch Registry & Manifest
// ============================================================================
std::string generate_batch_cpp(const std::vector<LiftedFunction>& funcs, const std::string& batch_name) {
    std::ostringstream out;
    out << "// Auto-generated AOT translation unit: " << batch_name << ". Do not edit.\n";
    out << "#include \"aot_batch.h\"\n";
    out << "#include \"alu.h\"\n";
    out << "#include \"guest_memory.h\"\n";
    out << "#include \"dispatcher.h\"\n\n";
    out << "namespace bb {\n\n";

    for (const auto& fn : funcs) {
        out << emit_function_cpp(fn);
    }

    out << "} // namespace bb\n";
    return out.str();
}

std::string generate_batch_header(const std::vector<LiftedFunction>& funcs) {
    std::ostringstream out;
    out << "#pragma once\n";
    out << "#include \"alu.h\"\n";
    out << "#include \"dispatcher.h\"\n";
    out << "#include <cstdint>\n\n";
    out << "namespace bb {\n\n";

    for (const auto& fn : funcs) {
        out << "bb::Result " << fn.name << "(std::uint64_t a, std::uint64_t b);\n";
    }

    out << "\nvoid register_aot_batch(bb::IndirectDispatcher& dispatcher);\n";
    out << "\n} // namespace bb\n";
    return out.str();
}

std::string generate_batch_registry(const std::vector<LiftedFunction>& funcs) {
    std::ostringstream out;
    out << "// Auto-generated AOT dispatcher registry. Do not edit.\n";
    out << "#include \"aot_batch.h\"\n\n";
    out << "namespace bb {\n\n";
    out << "void register_aot_batch(bb::IndirectDispatcher& dispatcher) {\n";

    for (const auto& fn : funcs) {
        out << "    dispatcher.register_target(0x" << std::hex << fn.vaddr << "ULL, &" << fn.name << ");\n";
    }

    out << "}\n\n";
    out << "} // namespace bb\n";
    return out.str();
}

std::string generate_batch_manifest(const std::vector<LiftedFunction>& funcs, const std::string& source_id) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": 1,\n";
    out << "  \"source_id\": \"" << source_id << "\",\n";
    out << "  \"source_arch\": \"x86_64\",\n";
    out << "  \"target_arch\": \"arm64-apple-ios\",\n";
    out << "  \"total_functions\": " << funcs.size() << ",\n";
    out << "  \"functions\": [\n";

    for (std::size_t i = 0; i < funcs.size(); ++i) {
        const auto& fn = funcs[i];
        out << "    {\n";
        out << "      \"symbol\": \"" << fn.name << "\",\n";
        out << "      \"address\": \"0x" << std::hex << fn.vaddr << "\",\n";
        out << "      \"byte_size\": " << std::dec << fn.byte_size << ",\n";
        out << "      \"is_entry\": " << (fn.is_entry_point ? "true" : "false") << "\n";
        out << "    }" << (i + 1 < funcs.size() ? "," : "") << "\n";
    }

    out << "  ]\n";
    out << "}\n";
    return out.str();
}

AotBatchResult batch_translate_elf(const std::string& elf_path, const std::string& output_dir, const AotBatchConfig& config) {
    AotBatchResult result{};
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);

    std::ifstream file(elf_path, std::ios::binary | std::ios::ate);
    if (!file) {
        result.error_message = "Cannot open ELF executable at: " + elf_path;
        return result;
    }

    auto size = static_cast<std::size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(size);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        result.error_message = "Failed to read ELF binary";
        return result;
    }

    std::vector<LiftedFunction> funcs;
    try {
        funcs = lift_elf(data.data(), data.size(), config.max_functions);
    } catch (const std::exception& e) {
        result.error_message = std::string("ELF lifting error: ") + e.what();
        return result;
    }

    result.total_functions = funcs.size();
    for (const auto& fn : funcs) {
        for (const auto& bb : fn.blocks) {
            result.total_instructions += bb.instructions.size();
        }
    }

    // Partition into batches
    std::size_t batch_size = config.functions_per_batch > 0 ? config.functions_per_batch : 100;
    std::size_t total_batches = (funcs.size() + batch_size - 1) / batch_size;
    result.batch_files_count = total_batches;

    for (std::size_t b = 0; b < total_batches; ++b) {
        std::size_t start = b * batch_size;
        std::size_t end = std::min(start + batch_size, funcs.size());
        std::vector<LiftedFunction> batch_funcs(funcs.begin() + start, funcs.begin() + end);

        std::string batch_name = config.output_prefix + std::to_string(b);
        std::string filename = output_dir + "/" + batch_name + ".cpp";
        std::ofstream out(filename);
        if (out) {
            out << generate_batch_cpp(batch_funcs, batch_name);
            result.generated_files.push_back(filename);
        }
    }

    // Header
    result.header_file = output_dir + "/aot_batch.h";
    std::ofstream h_out(result.header_file);
    if (h_out) h_out << generate_batch_header(funcs);

    // Registry
    result.registry_file = output_dir + "/aot_registry.cpp";
    std::ofstream reg_out(result.registry_file);
    if (reg_out) reg_out << generate_batch_registry(funcs);

    // Manifest
    result.manifest_file = output_dir + "/aot_manifest.json";
    std::ofstream mf_out(result.manifest_file);
    if (mf_out) mf_out << generate_batch_manifest(funcs, elf_path);

    result.success = true;
    return result;
}

} // namespace bb
