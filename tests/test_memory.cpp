#include "check.h"
#include "guest_memory.h"
#include <array>
#include <atomic>
#include <limits>
#include <random>
#include <thread>

int main() { return run([] {
    using namespace bb;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    GuestMemory m(64);
    CHECK(m.map(10, 0, read) == MemoryError::invalid);
    CHECK(m.map(max, 1, read) == MemoryError::invalid);
    CHECK(m.map(max - 2, 4, read) == MemoryError::invalid);
    CHECK(m.map(0, 4, 4) == MemoryError::invalid);
    CHECK(m.map(16, 16, read | write) == MemoryError::ok);
    CHECK(m.map(15, 2, read) == MemoryError::overlap);
    CHECK(m.map(17, 1, read) == MemoryError::overlap);
    CHECK(m.map(16, 16, read) == MemoryError::overlap);
    CHECK(m.map(0, 32, read) == MemoryError::overlap);
    CHECK(m.map(0, 16, read | write) == MemoryError::ok);
    CHECK(m.map(32, 32, read | write) == MemoryError::ok);
    CHECK(m.map(100, 1, read) == MemoryError::budget);
    CHECK(m.used() == 64);
    std::array<std::uint8_t, 32> result{};
    result.fill(0xAA);
    CHECK(m.load(32, result.data(), result.size()) == MemoryError::ok);
    for (auto byte : result) CHECK(byte == 0);
    std::uint8_t input = 0xBB, output = 0;
    CHECK(m.store(63, &input, 1) == MemoryError::ok);
    CHECK(m.load(63, &output, 1) == MemoryError::ok && output == input);
    CHECK(m.load(64, &output, 1) == MemoryError::unmapped);
    CHECK(m.store(63, result.data(), 2) == MemoryError::unmapped);
    CHECK(m.load(63, &output, 1) == MemoryError::ok && output == input);
    CHECK(m.load(15, result.data(), 2) == MemoryError::unmapped);
    CHECK(m.load(0, nullptr, 1) == MemoryError::invalid);
    CHECK(m.store(0, nullptr, 1) == MemoryError::invalid);
    CHECK(m.load(0, &output, 0) == MemoryError::invalid);
    CHECK(m.store(max, &input, 1) == MemoryError::invalid);
    CHECK(m.protect(32, read) == MemoryError::ok);
    CHECK(m.store(63, &input, 1) == MemoryError::denied);
    CHECK(m.protect(32, write) == MemoryError::ok);
    output = 0xFF;
    CHECK(m.load(63, &output, 1) == MemoryError::denied && output == 0xFF);
    CHECK(m.protect(32, 0) == MemoryError::ok);
    CHECK(m.store(63, &input, 1) == MemoryError::denied);
    CHECK(m.protect(32, 8) == MemoryError::invalid);
    CHECK(m.protect(33, read) == MemoryError::unmapped);
    CHECK(m.unmap(33) == MemoryError::unmapped);
    CHECK(m.unmap(32) == MemoryError::ok && m.used() == 32);
    CHECK(m.load(63, &output, 1) == MemoryError::unmapped);
    CHECK(m.unmap(32) == MemoryError::unmapped);
    CHECK(m.map(max - 32, 32, read | write) == MemoryError::ok);
    CHECK(m.store(max - 1, &input, 1) == MemoryError::ok);
    CHECK(m.load(max - 1, &output, 1) == MemoryError::ok && output == input);

    // Random transfers against a flat reference model, including crossing bounds.
    GuestMemory random_memory(256);
    CHECK(random_memory.map(0x1000, 256, read | write) == MemoryError::ok);
    std::array<std::uint8_t, 256> reference{};
    std::mt19937 rng(0xBB15);
    for (unsigned trial = 0; trial < 20000; ++trial) {
        const unsigned offset = rng() % 300, length = rng() % 32 + 1;
        std::array<std::uint8_t, 32> data{}, readback{};
        for (auto& byte : data) byte = static_cast<std::uint8_t>(rng());
        const bool valid = offset < 256 && length <= 256 - offset;
        const auto expected = valid ? MemoryError::ok : MemoryError::unmapped;
        CHECK(random_memory.store(0x1000 + offset, data.data(), length) == expected);
        if (valid) for (unsigned i = 0; i < length; ++i) reference[offset + i] = data[i];
        CHECK(random_memory.load(0x1000 + offset, readback.data(), length) == expected);
        if (valid) for (unsigned i = 0; i < length; ++i) CHECK(readback[i] == reference[offset + i]);
    }
    std::array<std::uint8_t, 256> final{};
    CHECK(random_memory.load(0x1000, final.data(), final.size()) == MemoryError::ok);
    CHECK(final == reference);
    std::cout << "memory: boundaries, permissions, budget, 20000 random transfers passed\n";
}); }
