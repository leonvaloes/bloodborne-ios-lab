#include "check.h"
#include "fixtures.h"
#include <array>
#include <random>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
static bb::Result native(unsigned op, std::uint64_t a, std::uint64_t b) {
    std::uint64_t captured;
    // Inline code is compiled into the executable, never generated at runtime.
    // Capture LAHF via RAX, never MOV AH to a memory operand: ASan may put
    // that operand in R8-R15, whose REX prefix cannot encode AH. No push/pop
    // is used, so compiler red-zone locals remain untouched.
    unsigned char overflow;
    if (op == 0) {
        asm volatile("addq %[b], %[a]; seto %[o]; lahf"
            : [a] "+r"(a), "=a"(captured), [o] "=m"(overflow)
            : [b] "r"(b) : "cc");
    } else if (op == 1) {
        asm volatile("subq %[b], %[a]; seto %[o]; lahf"
            : [a] "+r"(a), "=a"(captured), [o] "=m"(overflow)
            : [b] "r"(b) : "cc");
    } else {
        asm volatile("xorq %[b], %[a]; seto %[o]; lahf"
            : [a] "+r"(a), "=a"(captured), [o] "=m"(overflow)
            : [b] "r"(b) : "cc");
    }
    const auto mask = bb::CF | bb::PF | bb::ZF | bb::SF | (op != 2 ? bb::AF : 0);
    const auto flags = ((captured >> 8) & mask) | (overflow ? bb::OF : 0);
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
