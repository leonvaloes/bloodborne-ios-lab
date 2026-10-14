#include "check.h"
#include "guest_memory.h"
#include "kernel_threads.h"
#include <array>
#include <atomic>
#include <thread>
#include <chrono>

int main() { return run([] {
    using namespace bb;

    // ========================================================================
    // 1. Memory Concurrency Baseline
    // ========================================================================
    {
        GuestMemory memory(8 * 64);
        std::atomic<unsigned> failures{0};
        std::array<std::thread, 8> workers;
        for (unsigned i = 0; i < workers.size(); ++i) workers[i] = std::thread([&, i] {
            const std::uint64_t address = i * 64;
            for (unsigned round = 0; round < 1000; ++round) {
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
    }

    auto &mgr = KernelSyncManager::instance();
    mgr.reset();

    // ========================================================================
    // 2. Orbis OS Semaphores (SceKernelSema)
    // ========================================================================
    {
        std::uint32_t sem_id = 0;
        // Validation of parameters
        CHECK(mgr.create_sema(nullptr, "test_sema", SCE_KERNEL_SEMA_ATTR_TH_FIFO, 1, 2) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(mgr.create_sema(&sem_id, nullptr, SCE_KERNEL_SEMA_ATTR_TH_FIFO, 1, 2) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(mgr.create_sema(&sem_id, "test_sema", SCE_KERNEL_SEMA_ATTR_TH_FIFO, 3, 2) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(mgr.create_sema(&sem_id, "test_sema", SCE_KERNEL_SEMA_ATTR_TH_FIFO, -1, 2) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(mgr.create_sema(&sem_id, "test_sema", 10, 1, 2) == ORBIS_KERNEL_ERROR_EINVAL); // attr > 2

        // Valid creation
        CHECK(mgr.create_sema(&sem_id, "game_sync_sema", SCE_KERNEL_SEMA_ATTR_TH_FIFO, 2, 4) == ORBIS_OK);
        CHECK(sem_id > 0);

        // Immediate wait and poll
        CHECK(mgr.poll_sema(sem_id, 1) == ORBIS_OK); // count becomes 1
        CHECK(mgr.poll_sema(sem_id, 1) == ORBIS_OK); // count becomes 0
        CHECK(mgr.poll_sema(sem_id, 1) == ORBIS_KERNEL_ERROR_EBUSY); // empty!

        // Signal
        CHECK(mgr.signal_sema(sem_id, 3) == ORBIS_OK); // count becomes 3
        CHECK(mgr.signal_sema(sem_id, 2) == ORBIS_KERNEL_ERROR_EINVAL); // 3 + 2 > 4 (max)
        CHECK(mgr.poll_sema(sem_id, 3) == ORBIS_OK); // count becomes 0

        // Timeout on wait_sema
        uint32_t timeout = 5000; // 5 ms
        auto t0 = std::chrono::steady_clock::now();
        CHECK(mgr.wait_sema(sem_id, 1, &timeout) == ORBIS_KERNEL_ERROR_ETIMEDOUT);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(elapsed_ms >= 4);
        CHECK(timeout == 0);

        // Multithreaded producer-consumer test with Semaphore
        std::atomic<int> shared_counter{0};
        std::atomic<bool> producer_done{false};

        std::thread consumer([&] {
            while (!producer_done || shared_counter.load() < 50) {
                uint32_t wait_timeout = 20000; // 20 ms
                if (mgr.wait_sema(sem_id, 1, &wait_timeout) == ORBIS_OK) {
                    shared_counter.fetch_add(1);
                }
            }
        });

        std::thread producer([&] {
            for (int i = 0; i < 50; ++i) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                CHECK(mgr.signal_sema(sem_id, 1) == ORBIS_OK);
            }
            producer_done = true;
        });

        producer.join();
        consumer.join();
        CHECK(shared_counter.load() == 50);

        // Cancel test: wake all waiters with ECANCELED
        std::atomic<int> canceled_result{0};
        std::thread waiter_cancel([&] {
            canceled_result = mgr.wait_sema(sem_id, 1, nullptr);
        });

        // Ensure waiter is enrolled
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(mgr.sema_waiters(sem_id) == 1);

        int32_t num_awoken = 0;
        CHECK(mgr.cancel_sema(sem_id, 1, &num_awoken) == ORBIS_OK);
        CHECK(num_awoken == 1);
        waiter_cancel.join();
        CHECK(canceled_result.load() == ORBIS_KERNEL_ERROR_ECANCELED);

        // Delete test
        CHECK(mgr.delete_sema(sem_id) == ORBIS_OK);
        CHECK(mgr.poll_sema(sem_id, 1) == ORBIS_KERNEL_ERROR_ESRCH);
        CHECK(mgr.delete_sema(sem_id) == ORBIS_KERNEL_ERROR_ESRCH);
    }

    // ========================================================================
    // 3. Orbis OS Light-weight Mutexes (SceKernelLwMutex)
    // ========================================================================
    {
        std::uint32_t lw_id = 0;
        CHECK(mgr.create_lw_mutex(nullptr, "test_lw", 0, 0) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(mgr.create_lw_mutex(&lw_id, nullptr, 0, 0) == ORBIS_KERNEL_ERROR_EINVAL);
        CHECK(mgr.create_lw_mutex(&lw_id, "test_lw", 0, 5) == ORBIS_KERNEL_ERROR_EINVAL);

        CHECK(mgr.create_lw_mutex(&lw_id, "game_lw_mutex", SCE_KERNEL_LW_MUTEX_ATTR_RECURSIVE, 0) == ORBIS_OK);
        CHECK(lw_id > 0);

        // Trylock and unlock
        CHECK(mgr.trylock_lw_mutex(lw_id) == ORBIS_OK);
        // Recursive lock (since ATTR_RECURSIVE is enabled)
        CHECK(mgr.trylock_lw_mutex(lw_id) == ORBIS_OK);
        CHECK(mgr.lock_lw_mutex(lw_id, 1) == ORBIS_OK);
        // Need 3 unlocks
        CHECK(mgr.unlock_lw_mutex(lw_id) == ORBIS_OK);
        CHECK(mgr.unlock_lw_mutex(lw_id) == ORBIS_OK);
        CHECK(mgr.unlock_lw_mutex(lw_id) == ORBIS_OK);
        // Extra unlock should fail with EPERM
        CHECK(mgr.unlock_lw_mutex(lw_id) == ORBIS_KERNEL_ERROR_EPERM);

        // Multi-threaded contention on LwMutex
        constexpr unsigned num_workers = 8;
        constexpr unsigned iters_per_worker = 500;
        int protected_counter = 0;

        std::array<std::thread, num_workers> threads_arr;
        for (unsigned i = 0; i < num_workers; ++i) {
            threads_arr[i] = std::thread([&] {
                for (unsigned j = 0; j < iters_per_worker; ++j) {
                    CHECK(mgr.lock_lw_mutex(lw_id, 1) == ORBIS_OK);
                    ++protected_counter;
                    CHECK(mgr.unlock_lw_mutex(lw_id, 1) == ORBIS_OK);
                }
            });
        }
        for (auto &t : threads_arr) t.join();
        CHECK(protected_counter == static_cast<int>(num_workers * iters_per_worker));

        // Timedlock timeout test
        std::atomic<bool> lock_held{false};
        std::atomic<int> timeout_test_result{0};

        std::thread holder([&] {
            CHECK(mgr.lock_lw_mutex(lw_id, 1) == ORBIS_OK);
            lock_held = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CHECK(mgr.unlock_lw_mutex(lw_id, 1) == ORBIS_OK);
        });

        while (!lock_held) std::this_thread::yield();

        std::thread attempter([&] {
            uint32_t to = 5000; // 5 ms, holder sleeps 50 ms
            timeout_test_result = mgr.lock_lw_mutex(lw_id, 1, &to);
        });

        attempter.join();
        holder.join();
        CHECK(timeout_test_result.load() == ORBIS_KERNEL_ERROR_ETIMEDOUT);

        // GuestMemory SceKernelLwMutexWork test
        GuestMemory mem(64);
        CHECK(mem.map(0x4000, 64, read | write) == MemoryError::ok);

        CHECK(mgr.create_lw_mutex(mem, 0x4000, "guest_lw", SCE_KERNEL_LW_MUTEX_ATTR_RECURSIVE, 0) == ORBIS_OK);
        CHECK(mgr.trylock_lw_mutex(mem, 0x4000, 1) == ORBIS_OK);
        CHECK(mgr.unlock_lw_mutex(mem, 0x4000, 1) == ORBIS_OK);
        CHECK(mgr.delete_lw_mutex(mem, 0x4000) == ORBIS_OK);

        CHECK(mgr.delete_lw_mutex(lw_id) == ORBIS_OK);
        CHECK(mgr.lock_lw_mutex(lw_id, 1) == ORBIS_KERNEL_ERROR_ESRCH);
    }

    // ========================================================================
    // 4. Orbis OS Pthread Mutex & Condition Variable
    // ========================================================================
    {
        std::uint32_t p_mutex = 0;
        std::uint32_t p_cond = 0;
        CHECK(mgr.pthread_mutex_init(&p_mutex) == ORBIS_OK);
        CHECK(mgr.pthread_cond_init(&p_cond) == ORBIS_OK);

        bool signaled = false;
        std::thread waiter([&] {
            CHECK(mgr.pthread_mutex_lock(p_mutex) == ORBIS_OK);
            while (!signaled) {
                CHECK(mgr.pthread_cond_wait(p_cond, p_mutex) == ORBIS_OK);
            }
            CHECK(mgr.pthread_mutex_unlock(p_mutex) == ORBIS_OK);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(mgr.pthread_mutex_lock(p_mutex) == ORBIS_OK);
        signaled = true;
        CHECK(mgr.pthread_cond_signal(p_cond) == ORBIS_OK);
        CHECK(mgr.pthread_mutex_unlock(p_mutex) == ORBIS_OK);

        waiter.join();
        CHECK(signaled);

        CHECK(mgr.pthread_cond_destroy(p_cond) == ORBIS_OK);
        CHECK(mgr.pthread_mutex_destroy(p_mutex) == ORBIS_OK);
    }

    std::cout << "threads: Semaphores, LwMutexes, Pthreads and Memory cycles passed\n";
}); }
