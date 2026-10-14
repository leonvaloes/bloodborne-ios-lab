#include "check.h"
#include "hle.h"

int main() { return run([] {
    using namespace bb;
    GuestMemory mem(256);
    CHECK(mem.map(0x1000, 128, read | write) == MemoryError::ok);

    auto hle = HleManager::create_standard();
    CHECK(hle.count() >= 3);
    CHECK(hle.has_symbol(0x2001)); // sceKernelGettimeofday
    CHECK(hle.has_symbol(0x2002)); // sceKernelGetCurrentCpu
    CHECK(hle.has_symbol(0x2003)); // sceKernelAllocateDirectMemory
    CHECK(!hle.has_symbol(0xFFFF));

    // Test sceKernelGetCurrentCpu
    CHECK(hle.invoke(0x2002, mem, 0, 0, 0) == 0);

    // Test sceKernelGettimeofday
    CHECK(hle.invoke(0x2001, mem, 0x1000, 0, 0) == 0);
    struct { std::uint64_t s, us; } tv{};
    CHECK(mem.load(0x1000, &tv, sizeof(tv)) == MemoryError::ok);
    CHECK(tv.s > 0);

    // Test custom HLE registration
    hle.register_handler(0x7777, "custom_add", [](GuestMemory&, std::uint64_t a, std::uint64_t b, std::uint64_t) -> std::int64_t {
        return static_cast<std::int64_t>(a + b);
    });
    CHECK(hle.invoke(0x7777, mem, 40, 60, 0) == 100);

    bool threw = false;
    try {
        hle.invoke(0x9999, mem, 0, 0, 0);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    std::cout << "hle: kernel imports, argument marshaling and custom thunks passed\n";
}); }
