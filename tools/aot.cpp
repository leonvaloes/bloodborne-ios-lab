#include "aot.h"
#include <sstream>
#include <stdexcept>
#include <vector>

namespace bb {

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

std::string translate(const std::vector<std::uint8_t>& bytes, const std::string& symbol) {
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

    throw std::runtime_error("Unknown fixture symbol: " + symbol);
}

} // namespace bb
