#pragma once
#include <cstdint>

namespace bb {
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

inline Result addx(std::uint64_t a, std::uint64_t b, unsigned bits, bool cin = false) {
    if (bits == 64) {
        const auto r = a + b + (cin ? 1 : 0);
        const bool carry = (r < a) || (cin && r == a);
        return {r, szp(r) | (carry ? CF : 0) | ((a ^ b ^ r) & AF) |
            ((~(a ^ b) & (a ^ r)) >> 63 ? OF : 0)};
    }
    const std::uint64_t m = (1ULL << bits) - 1;
    a &= m; b &= m;
    const std::uint64_t sum = a + b + (cin ? 1 : 0);
    const std::uint64_t r = sum & m;
    unsigned ones = 0;
    for (unsigned i = 0; i < 8; ++i) ones += unsigned((r >> i) & 1);
    std::uint64_t flags = (r == 0 ? ZF : 0) | (((r >> (bits - 1)) & 1) ? SF : 0) | (ones % 2 == 0 ? PF : 0);
    if (sum > m) flags |= CF;
    if (((a & 0xF) + (b & 0xF) + (cin ? 1 : 0)) > 0xF) flags |= AF;
    if ((~(a ^ b) & (a ^ r)) & (1ULL << (bits - 1))) flags |= OF;
    return {r, flags};
}

inline Result subx(std::uint64_t a, std::uint64_t b, unsigned bits, bool bin = false) {
    if (bits == 64) {
        const auto r = a - b - (bin ? 1 : 0);
        const bool borrow = (a < b) || (bin && a == b);
        return {r, szp(r) | (borrow ? CF : 0) | ((a ^ b ^ r) & AF) |
            (((a ^ b) & (a ^ r)) >> 63 ? OF : 0)};
    }
    const std::uint64_t m = (1ULL << bits) - 1;
    a &= m; b &= m;
    const bool borrow = a < (b + (bin ? 1 : 0));
    const std::uint64_t diff = a - b - (bin ? 1 : 0);
    const std::uint64_t r = diff & m;
    unsigned ones = 0;
    for (unsigned i = 0; i < 8; ++i) ones += unsigned((r >> i) & 1);
    std::uint64_t flags = (r == 0 ? ZF : 0) | (((r >> (bits - 1)) & 1) ? SF : 0) | (ones % 2 == 0 ? PF : 0);
    if (borrow) flags |= CF;
    if ((a & 0xF) < ((b & 0xF) + (bin ? 1 : 0))) flags |= AF;
    if (((a ^ b) & (a ^ r)) & (1ULL << (bits - 1))) flags |= OF;
    return {r, flags};
}

inline Result andx(std::uint64_t a, std::uint64_t b, unsigned bits) {
    const std::uint64_t m = (bits == 64 ? ~0ULL : ((1ULL << bits) - 1));
    const std::uint64_t r = (a & b) & m;
    unsigned ones = 0;
    for (unsigned i = 0; i < 8; ++i) ones += unsigned((r >> i) & 1);
    std::uint64_t flags = (r == 0 ? ZF : 0) | (((r >> (bits - 1)) & 1) ? SF : 0) | (ones % 2 == 0 ? PF : 0);
    return {r, flags};
}

inline Result orx(std::uint64_t a, std::uint64_t b, unsigned bits) {
    const std::uint64_t m = (bits == 64 ? ~0ULL : ((1ULL << bits) - 1));
    const std::uint64_t r = (a | b) & m;
    unsigned ones = 0;
    for (unsigned i = 0; i < 8; ++i) ones += unsigned((r >> i) & 1);
    std::uint64_t flags = (r == 0 ? ZF : 0) | (((r >> (bits - 1)) & 1) ? SF : 0) | (ones % 2 == 0 ? PF : 0);
    return {r, flags};
}

inline Result xorr(std::uint64_t a, std::uint64_t b, unsigned bits) {
    const std::uint64_t m = (bits == 64 ? ~0ULL : ((1ULL << bits) - 1));
    const std::uint64_t r = (a ^ b) & m;
    unsigned ones = 0;
    for (unsigned i = 0; i < 8; ++i) ones += unsigned((r >> i) & 1);
    std::uint64_t flags = (r == 0 ? ZF : 0) | (((r >> (bits - 1)) & 1) ? SF : 0) | (ones % 2 == 0 ? PF : 0);
    return {r, flags};
}

inline Result shlx(std::uint64_t a, unsigned count, unsigned bits) {
    count &= (bits == 64 ? 63 : 31);
    const std::uint64_t m = (bits == 64 ? ~0ULL : ((1ULL << bits) - 1));
    a &= m;
    const std::uint64_t r = (count >= bits ? 0 : (a << count)) & m;
    unsigned ones = 0;
    for (unsigned i = 0; i < 8; ++i) ones += unsigned((r >> i) & 1);
    std::uint64_t flags = (r == 0 ? ZF : 0) | (((r >> (bits - 1)) & 1) ? SF : 0) | (ones % 2 == 0 ? PF : 0);
    if (count > 0 && count <= bits) {
        if ((a >> (bits - count)) & 1) flags |= CF;
        if (count == 1 && (((a >> (bits - 1)) & 1) != ((r >> (bits - 1)) & 1))) flags |= OF;
    }
    return {r, flags};
}

inline Result shrx(std::uint64_t a, unsigned count, unsigned bits) {
    count &= (bits == 64 ? 63 : 31);
    const std::uint64_t m = (bits == 64 ? ~0ULL : ((1ULL << bits) - 1));
    a &= m;
    const std::uint64_t r = (count >= bits ? 0 : (a >> count)) & m;
    unsigned ones = 0;
    for (unsigned i = 0; i < 8; ++i) ones += unsigned((r >> i) & 1);
    std::uint64_t flags = (r == 0 ? ZF : 0) | (((r >> (bits - 1)) & 1) ? SF : 0) | (ones % 2 == 0 ? PF : 0);
    if (count > 0 && count <= bits) {
        if ((a >> (count - 1)) & 1) flags |= CF;
        if (count == 1 && ((a >> (bits - 1)) & 1)) flags |= OF;
    }
    return {r, flags};
}

inline Result cmpx(std::uint64_t a, std::uint64_t b, unsigned bits) {
    return subx(a, b, bits, false);
}

inline Result testx(std::uint64_t a, std::uint64_t b, unsigned bits) {
    return andx(a, b, bits);
}
}
