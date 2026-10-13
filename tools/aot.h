#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace bb {
// A deliberately closed fixture subset: MOV RAX,RDI; ADD/SUB/XOR RAX,RSI; RET.
// Reject everything else. There is no fallback, executable allocation, or JIT.
std::vector<std::uint8_t> parse_hex(const std::string& input);
std::string translate(const std::vector<std::uint8_t>& bytes, const std::string& symbol);
}
