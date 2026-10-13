#include "smoke.h"
#include "fixtures.h"
#include "guest_memory.h"
#include <limits>

namespace bb {
SmokeReport smoke() {
    SmokeReport report{0, 0};
    const auto check = [&report](bool passed) { ++report.checks; if (!passed) ++report.failures; };
    check(fixture_add(1, 2).value == 3);
    const auto carry = fixture_add(std::numeric_limits<std::uint64_t>::max(), 1);
    check(carry.value == 0 && (carry.flags & (CF | ZF)) == (CF | ZF));
    const auto overflow = fixture_add(0x7fffffffffffffffULL, 1);
    check(overflow.value == 0x8000000000000000ULL && (overflow.flags & OF));
    check(fixture_sub(0, 1).value == std::numeric_limits<std::uint64_t>::max());
    check((fixture_sub(0, 1).flags & CF) != 0);
    check(fixture_xor(0xff, 0xff).value == 0);
    GuestMemory memory(64);
    check(memory.map(0x400000, 64, read | write) == MemoryError::ok);
    const auto expected = fixture_add(0xBB00, 0x15).value;
    check(memory.store(0x400038, &expected, sizeof expected) == MemoryError::ok);
    std::uint64_t observed = 0;
    check(memory.load(0x400038, &observed, sizeof observed) == MemoryError::ok);
    check(observed == 0xBB15);
    check(memory.store(0x400039, &expected, sizeof expected) == MemoryError::unmapped);
    check(memory.protect(0x400000, read) == MemoryError::ok);
    check(memory.store(0x400000, &expected, sizeof expected) == MemoryError::denied);
    check(memory.unmap(0x400000) == MemoryError::ok && memory.used() == 0);
    return report;
}
}
