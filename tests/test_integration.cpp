#include "check.h"
#include "smoke.h"

int main() { return run([] {
    const auto result = bb::smoke();
    CHECK(result.checks >= 60);
    CHECK(result.failures == 0);
    std::cout << "integration: all S0-S5 fixtures + guest memory + SIMD + atomics + HLE + dispatcher passed (" << result.checks << " checks)\n";
}); }
