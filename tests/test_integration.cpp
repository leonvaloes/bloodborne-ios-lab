#include "check.h"
#include "smoke.h"

int main() { return run([] {
    const auto result = bb::smoke();
    CHECK(result.checks >= 40);
    CHECK(result.failures == 0);
    std::cout << "integration: generated fixtures + guest memory + smoke runner passed (" << result.checks << " checks)\n";
}); }
