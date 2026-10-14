#include "check.h"
#include "smoke.h"

int main() { return run([] {
    const auto result = bb::smoke();
    CHECK(result.checks >= 70);
    CHECK(result.failures == 0);
    std::cout << "integration: all S0-S7 fixtures + guest memory + SIMD + atomics + HLE + parser passed (" << result.checks << " checks)\n";
}); }
