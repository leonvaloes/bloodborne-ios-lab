#include "check.h"
#include "hle.h"
#include "kernel_threads.h"

int main() { return run([] {
    using namespace bb;
    GuestMemory mem(528);
    CHECK(mem.map(0x1000, 512, read | write) == MemoryError::ok);

    auto hle = HleManager::create_standard();
    CHECK(hle.count() >= 15);
    CHECK(hle.has_symbol(0x2001)); // sceKernelGettimeofday
    CHECK(hle.has_symbol(0x2002)); // sceKernelGetCurrentCpu
    CHECK(hle.has_symbol(0x2003)); // sceKernelAllocateDirectMemory
    CHECK(hle.has_symbol(0x2010)); // sceKernelCreateSema
    CHECK(hle.has_symbol(0x2011)); // sceKernelWaitSema
    CHECK(hle.has_symbol(0x2012)); // sceKernelPollSema
    CHECK(hle.has_symbol(0x2013)); // sceKernelSignalSema
    CHECK(hle.has_symbol(0x2015)); // sceKernelDeleteSema
    CHECK(hle.has_symbol(0x2020)); // sceKernelCreateLwMutex
    CHECK(hle.has_symbol(0x2021)); // sceKernelLockLwMutex
    CHECK(hle.has_symbol(0x2022)); // sceKernelTryLockLwMutex
    CHECK(hle.has_symbol(0x2023)); // sceKernelUnlockLwMutex
    CHECK(hle.has_symbol(0x2024)); // sceKernelDeleteLwMutex
    CHECK(hle.has_symbol(0x2030)); // scePthreadMutexInit
    CHECK(hle.has_symbol(0x2031)); // scePthreadMutexLock
    CHECK(hle.has_symbol(0x2032)); // scePthreadMutexUnlock
    CHECK(hle.has_symbol(0x2034)); // scePthreadMutexDestroy
    CHECK(hle.has_symbol(0x2040)); // scePthreadCondInit
    CHECK(hle.has_symbol(0x2042)); // scePthreadCondSignal
    CHECK(hle.has_symbol(0x2044)); // scePthreadCondDestroy
    CHECK(!hle.has_symbol(0xFFFF));

    // Test sceKernelGetCurrentCpu
    CHECK(hle.invoke(0x2002, mem, 0, 0, 0) == 0);

    // Test sceKernelGettimeofday
    CHECK(hle.invoke(0x2001, mem, 0x1000, 0, 0) == 0);
    struct { std::uint64_t s, us; } tv{};
    CHECK(mem.load(0x1000, &tv, sizeof(tv)) == MemoryError::ok);
    CHECK(tv.s > 0);

    // Test Semaphores via HLE
    // Create sema with initial = 2, max = 2
    std::uint64_t sem_id_addr = 0x1020;
    std::uint64_t packed_counts = 2 | (2 << 16);
    CHECK(hle.invoke(0x2010, mem, sem_id_addr, 0, packed_counts) == ORBIS_OK);

    std::uint32_t sem_id = 0;
    CHECK(mem.load(sem_id_addr, &sem_id, sizeof(sem_id)) == MemoryError::ok);
    CHECK(sem_id > 0);

    // Poll sema
    CHECK(hle.invoke(0x2012, mem, sem_id, 1, 0) == ORBIS_OK);
    CHECK(hle.invoke(0x2012, mem, sem_id, 1, 0) == ORBIS_OK);
    CHECK(hle.invoke(0x2012, mem, sem_id, 1, 0) == ORBIS_KERNEL_ERROR_EBUSY);

    // Signal sema
    CHECK(hle.invoke(0x2013, mem, sem_id, 2, 0) == ORBIS_OK);
    // Wait sema
    CHECK(hle.invoke(0x2011, mem, sem_id, 2, 0) == ORBIS_OK);

    // Delete sema
    CHECK(hle.invoke(0x2015, mem, sem_id, 0, 0) == ORBIS_OK);
    CHECK(hle.invoke(0x2012, mem, sem_id, 1, 0) == ORBIS_KERNEL_ERROR_ESRCH);

    // Test Light-weight Mutex via HLE
    std::uint64_t lw_work_addr = 0x1040;
    CHECK(hle.invoke(0x2020, mem, lw_work_addr, 0, SCE_KERNEL_LW_MUTEX_ATTR_RECURSIVE) == ORBIS_OK);
    CHECK(hle.invoke(0x2022, mem, lw_work_addr, 1, 0) == ORBIS_OK); // trylock
    CHECK(hle.invoke(0x2021, mem, lw_work_addr, 1, 0) == ORBIS_OK); // recursive lock
    CHECK(hle.invoke(0x2023, mem, lw_work_addr, 1, 0) == ORBIS_OK); // unlock 1
    CHECK(hle.invoke(0x2023, mem, lw_work_addr, 1, 0) == ORBIS_OK); // unlock 2
    CHECK(hle.invoke(0x2024, mem, lw_work_addr, 0, 0) == ORBIS_OK); // delete

    // Test Pthread Mutex & Cond via HLE
    std::uint64_t p_mutex_addr = 0x1080;
    CHECK(hle.invoke(0x2030, mem, p_mutex_addr, 0, 0) == ORBIS_OK);
    std::uint32_t p_mutex_id = 0;
    CHECK(mem.load(p_mutex_addr, &p_mutex_id, sizeof(p_mutex_id)) == MemoryError::ok);
    CHECK(hle.invoke(0x2031, mem, p_mutex_id, 0, 0) == ORBIS_OK); // lock
    CHECK(hle.invoke(0x2032, mem, p_mutex_id, 0, 0) == ORBIS_OK); // unlock
    CHECK(hle.invoke(0x2034, mem, p_mutex_id, 0, 0) == ORBIS_OK); // destroy

    std::uint64_t p_cond_addr = 0x1090;
    CHECK(hle.invoke(0x2040, mem, p_cond_addr, 0, 0) == ORBIS_OK);
    std::uint32_t p_cond_id = 0;
    CHECK(mem.load(p_cond_addr, &p_cond_id, sizeof(p_cond_id)) == MemoryError::ok);
    CHECK(hle.invoke(0x2042, mem, p_cond_id, 0, 0) == ORBIS_OK); // signal
    CHECK(hle.invoke(0x2044, mem, p_cond_id, 0, 0) == ORBIS_OK); // destroy

    // Synthetic guest pointers: reject invalid/readonly outputs without returning orphan IDs.
    CHECK(hle.invoke(0x2010, mem, 0, 0, packed_counts) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(hle.invoke(0x2030, mem, 0, 0, 0) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(hle.invoke(0x2040, mem, 0, 0, 0) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(hle.invoke(0x2010, mem, 0x1080, 0xDEAD, packed_counts) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(hle.invoke(0x2030, mem, 0x1080, 0xDEAD, 0) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(mem.unmap(0x1000) == MemoryError::ok);
    CHECK(mem.map(0x1000, 512, read) == MemoryError::ok);
    CHECK(hle.invoke(0x2010, mem, 0x1080, 0, packed_counts) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(hle.invoke(0x2030, mem, 0x1080, 0, 0) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(hle.invoke(0x2040, mem, 0x1080, 0, 0) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(mem.unmap(0x1000) == MemoryError::ok);
    CHECK(mem.map(0x1000, 512, read | write) == MemoryError::ok);
    CHECK(mem.map(0x3000, 16, read) == MemoryError::ok);
    CHECK(hle.invoke(0x2011, mem, 0xFFFFFFFF, 1, 0x3000) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(mem.unmap(0x3000) == MemoryError::ok);
    CHECK(hle.invoke(0x2011, mem, 0xFFFFFFFF, 1, 0xDEAD) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(hle.invoke(0x2021, mem, 0x1040, 1, 0xDEAD) == ORBIS_KERNEL_ERROR_EINVAL);
    CHECK(mem.used() == 512);
    // An invalid output must roll back a successfully mapped direct-memory request.
    CHECK(hle.invoke(0x2003, mem, 0x2000, 16, 0xDEAD) == -1);
    CHECK(mem.used() == 512);
    CHECK(hle.invoke(0x2003, mem, 0x2000, 16, 0) == 0);
    CHECK(mem.used() == 528);

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

    std::cout << "hle: kernel imports, argument marshaling and multithread sync passed\n";
}); }
