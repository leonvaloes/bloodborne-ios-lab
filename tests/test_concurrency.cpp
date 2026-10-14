#include "check.h"
#include "guest_memory.h"
#include <array>
#include <thread>
#include <atomic>

int main() { return run([] {
    using namespace bb;
    GuestMemory mem(128);
    CHECK(mem.map(0x1000, 64, read | write) == MemoryError::ok);

    constexpr unsigned num_threads = 8;
    constexpr unsigned iters_per_thread = 5000;

    std::array<std::thread, num_threads> workers;
    std::atomic<unsigned> errors{0};

    // Concurrent atomic fetch-and-add (equivalent to LOCK XADD across multiple cores)
    for (unsigned i = 0; i < num_threads; ++i) {
        workers[i] = std::thread([&] {
            for (unsigned j = 0; j < iters_per_thread; ++j) {
                std::uint64_t old_v = 0;
                if (mem.atomic_fetch_add_u64(0x1000, 1, old_v) != MemoryError::ok) {
                    ++errors;
                }
            }
        });
    }

    for (auto& w : workers) w.join();
    CHECK(errors == 0);

    std::uint64_t final_val = 0;
    CHECK(mem.load(0x1000, &final_val, sizeof(final_val)) == MemoryError::ok);
    CHECK(final_val == (num_threads * iters_per_thread));

    // Concurrent atomic compare-and-swap
    std::atomic<unsigned> cas_wins{0};
    for (unsigned i = 0; i < num_threads; ++i) {
        workers[i] = std::thread([&, i] {
            std::uint64_t expected = final_val;
            bool success = false;
            if (mem.atomic_compare_exchange_u64(0x1000, expected, 0xCAFE0000ULL | i, success) == MemoryError::ok) {
                if (success) ++cas_wins;
            }
        });
    }
    for (auto& w : workers) w.join();
    // Exactly one thread must have succeeded the CAS
    CHECK(cas_wins == 1);

    std::cout << "concurrency: 8 threads, 40000 concurrent atomic LOCK operations passed\n";
}); }
