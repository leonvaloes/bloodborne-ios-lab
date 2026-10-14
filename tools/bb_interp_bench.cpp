// Microbenchmark sintético do loop quente, sem bytes do jogo.
#include "../core/x86_interp.h"
#include <chrono>
#include <cstdio>
int main() {
    bb::GuestMemory mem(4096);
    if (mem.map(0x1000, 4096, bb::Permission::read | bb::Permission::write) != bb::MemoryError::ok) return 2;
    const unsigned char loop[] = {0xEB, 0xFE};
    if (mem.store(0x1000, loop, sizeof(loop)) != bb::MemoryError::ok) return 2;
    bb::X86Interpreter cpu(mem);
    cpu.state.rip = 0x1000;
    constexpr std::uint64_t steps = 1000000;
    const auto start = std::chrono::steady_clock::now();
    const auto result = cpu.run(steps);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (result.stop != bb::CpuStop::step_limit || result.steps != steps) return 1;
    std::printf("%llu passos em %.3f s: %.2f M instr/s\n",
                static_cast<unsigned long long>(steps), seconds, steps / seconds / 1e6);
}
