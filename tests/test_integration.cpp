#include "check.h"
#include "smoke.h"

int main() { return run([] {
    const auto result = bb::smoke();
    CHECK(result.checks >= 75);
    CHECK(result.failures == 0);
    std::cout << "integration: all S0-S8 fixtures + guest memory + SIMD + atomics + HLE + parser + boot loader passed (" << result.checks << " checks)\n";
}); }
