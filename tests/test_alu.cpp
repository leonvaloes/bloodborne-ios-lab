#include "check.h"
#include "fixtures.h"
#include <array>
#include <random>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
static bb::Result native(unsigned op, std::uint64_t a, std::uint64_t b) {
    std::uint64_t flags;
    // Inline code is compiled into the executable, never generated at runtime.
    // Capture arithmetic flags with SETcc/LAHF: no push/pop that could clobber
    // compiler red-zone locals. LAHF uses AH, so explicitly clobber RAX.
    unsigned char carry, parity, auxiliary, zero, sign, overflow;
    if (op == 0) {
        asm volatile("addq %[b], %[a]; setc %[c]; setp %[p]; setz %[z]; sets %[s]; seto %[o]; lahf; movb %%ah, %[h]"
            : [a] "+r"(a), [c] "=m"(carry), [p] "=m"(parity), [z] "=m"(zero),
              [s] "=m"(sign), [o] "=m"(overflow), [h] "=m"(auxiliary)
            : [b] "r"(b) : "rax", "cc");
    } else if (op == 1) {
        asm volatile("subq %[b], %[a]; setc %[c]; setp %[p]; setz %[z]; sets %[s]; seto %[o]; lahf; movb %%ah, %[h]"
            : [a] "+r"(a), [c] "=m"(carry), [p] "=m"(parity), [z] "=m"(zero),
              [s] "=m"(sign), [o] "=m"(overflow), [h] "=m"(auxiliary)
            : [b] "r"(b) : "rax", "cc");
    } else {
        asm volatile("xorq %[b], %[a]; setc %[c]; setp %[p]; setz %[z]; sets %[s]; seto %[o]; lahf; movb %%ah, %[h]"
            : [a] "+r"(a), [c] "=m"(carry), [p] "=m"(parity), [z] "=m"(zero),
              [s] "=m"(sign), [o] "=m"(overflow), [h] "=m"(auxiliary)
            : [b] "r"(b) : "rax", "cc");
    }
    flags = (carry ? bb::CF : 0) | (parity ? bb::PF : 0) | (zero ? bb::ZF : 0) |
        (sign ? bb::SF : 0) | (overflow ? bb::OF : 0) | (op != 2 ? auxiliary & bb::AF : 0);
    return {a, flags};
}
#endif

int main() { return run([] {
    using namespace bb;
    CHECK(fixture_add(0, 0).flags == (PF | ZF));
    CHECK(fixture_add(UINT64_MAX, 1).flags == (CF | PF | AF | ZF));
    CHECK(fixture_add(0x7fffffffffffffffULL, 1).flags == (PF | AF | SF | OF));
    CHECK(fixture_sub(0, 1).flags == (CF | PF | AF | SF));
    CHECK(fixture_sub(0x8000000000000000ULL, 1).flags == (PF | AF | OF));
    CHECK(fixture_xor(0x80, 0).flags == 0);
    CHECK(fixture_xor(0xff, 0xff).flags == (PF | ZF));
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    using Function = Result (*)(std::uint64_t, std::uint64_t);
    const std::array<Function, 3> functions{fixture_add, fixture_sub, fixture_xor};
    const std::array<std::uint64_t, 12> edges{0, 1, 15, 16, 127, 128, 255, 256,
        0x7fffffffffffffffULL, 0x8000000000000000ULL, UINT64_MAX - 1, UINT64_MAX};
    const auto compare = [&](unsigned op, std::uint64_t a, std::uint64_t b) {
        const auto expected = native(op, a, b), actual = functions[op](a, b);
        CHECK(expected.value == actual.value);
        CHECK(expected.flags == actual.flags);
    };
    for (unsigned op = 0; op < 3; ++op) for (auto a : edges) for (auto b : edges) compare(op, a, b);
    std::mt19937_64 rng(0xBB15);
    for (unsigned i = 0; i < 100000; ++i) {
        const auto a = rng(), b = rng();
        for (unsigned op = 0; op < 3; ++op) compare(op, a, b);
    }
    std::cout << "ALU: 300432 comparisons against native x86-64 results and flags passed\n";
#else
    std::cout << "ALU: fixed golden vectors passed; native x86-64 oracle unavailable on this target\n";
#endif
}); }
