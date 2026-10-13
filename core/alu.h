#pragma once
#include <cstdint>

namespace bb {
// Only the arithmetic flags defined by these instructions. Other RFLAGS are
// deliberately outside this fixture's contract. XOR leaves AF undefined.
constexpr std::uint64_t CF = 1, PF = 4, AF = 16, ZF = 64, SF = 128, OF = 2048;
struct Result { std::uint64_t value, flags; };
inline std::uint64_t szp(std::uint64_t value) {
    unsigned ones = 0;
    for (unsigned i = 0; i < 8; ++i) ones += unsigned((value >> i) & 1);
    return (value == 0 ? ZF : 0) | (value >> 63 ? SF : 0) | (ones % 2 == 0 ? PF : 0);
}
inline Result add64(std::uint64_t a, std::uint64_t b) {
    const auto r = a + b;
    return {r, szp(r) | (r < a ? CF : 0) | ((a ^ b ^ r) & AF) |
        ((~(a ^ b) & (a ^ r)) >> 63 ? OF : 0)};
}
inline Result sub64(std::uint64_t a, std::uint64_t b) {
    const auto r = a - b;
    return {r, szp(r) | (a < b ? CF : 0) | ((a ^ b ^ r) & AF) |
        (((a ^ b) & (a ^ r)) >> 63 ? OF : 0)};
}
inline Result xor64(std::uint64_t a, std::uint64_t b) {
    return {a ^ b, szp(a ^ b)};
}
}
