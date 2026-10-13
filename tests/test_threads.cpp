#include "check.h"
#include "guest_memory.h"
#include <array>
#include <atomic>
#include <thread>
int main() { return run([] {
    bb::GuestMemory memory(8 * 64);
    std::atomic<unsigned> failures{0};
    std::array<std::thread, 8> workers;
    for (unsigned i = 0; i < workers.size(); ++i) workers[i] = std::thread([&, i] {
        const std::uint64_t address = i * 64;
        for (unsigned round = 0; round < 2000; ++round) {
            const std::uint64_t value = (std::uint64_t(i) << 32) | round;
            std::uint64_t observed = 0;
            if (memory.map(address, 64, bb::read | bb::write) != bb::MemoryError::ok ||
                memory.store(address + 56, &value, sizeof value) != bb::MemoryError::ok ||
                memory.load(address + 56, &observed, sizeof observed) != bb::MemoryError::ok ||
                observed != value || memory.unmap(address) != bb::MemoryError::ok) ++failures;
        }
    });
    for (auto& worker : workers) worker.join();
    CHECK(failures == 0);
    CHECK(memory.used() == 0);
    std::cout << "threads: 8 workers, 16000 map/store/load/unmap cycles passed\n";
}); }
