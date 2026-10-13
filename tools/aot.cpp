#include "aot.h"
#include <sstream>
#include <stdexcept>

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
    if (symbol != "fixture_add" && symbol != "fixture_sub" && symbol != "fixture_xor")
        throw std::runtime_error("Unknown fixture symbol");
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
    return "// Generated from authored x86-64 fixture bytes. Do not edit.\n#include \"fixtures.h\"\n"
        "bb::Result " + symbol + "(std::uint64_t a, std::uint64_t b) { return bb::" + operation + "(a, b); }\n";
}
}
