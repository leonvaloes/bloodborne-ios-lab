#include "smoke.h"
#include <iostream>
#ifndef BB_BUILD_ID
#define BB_BUILD_ID "local-unversioned"
#endif
int main() {
    const auto r = bb::smoke();
    std::cout << "{\"schema\":1,\"build\":\"" BB_BUILD_ID "\",\"environment\":\"host\","
        "\"checks\":" << r.checks << ",\"failures\":" << r.failures << ",\"game_execution\":false}\n";
    return r.failures ? 1 : 0;
}
