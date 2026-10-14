#include "hle.h"
#include "kernel_threads.h"
#include <stdexcept>
#include <chrono>
#include <cstring>
#include <algorithm>

namespace bb {

void HleManager::register_handler(std::uint64_t nid, const std::string& name, HleHandler handler) {
    if (!handler) throw std::invalid_argument("Cannot register null HLE handler");
    handlers_[nid] = HleSymbol{name, handler};
}

bool HleManager::has_symbol(std::uint64_t nid) const {
    return handlers_.find(nid) != handlers_.end();
}

std::int64_t HleManager::invoke(std::uint64_t nid, GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t arg2) const {
    auto it = handlers_.find(nid);
    if (it == handlers_.end()) {
        throw std::runtime_error("Unresolved HLE NID: " + std::to_string(nid));
    }
    return it->second.handler(memory, arg0, arg1, arg2);
}

std::size_t HleManager::count() const {
    return handlers_.size();
}

HleManager HleManager::create_standard() {
    HleManager manager;

    // sceKernelGettimeofday (NID 0x2001)
    // Args: arg0 = guest timeval pointer (uint64_t sec, uint64_t usec)
    manager.register_handler(0x2001, "sceKernelGettimeofday", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t, std::uint64_t) -> std::int64_t {
        if (!arg0) return -1;
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        const auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(now).count() % 1000000;
        struct { std::uint64_t s, us; } tv{static_cast<std::uint64_t>(secs), static_cast<std::uint64_t>(usecs)};
        if (memory.store(arg0, &tv, sizeof(tv)) != MemoryError::ok) return -1;
        return 0;
    });

    // sceKernelGetCurrentCpu (NID 0x2002)
    manager.register_handler(0x2002, "sceKernelGetCurrentCpu", [](GuestMemory&, std::uint64_t, std::uint64_t, std::uint64_t) -> std::int64_t {
        return 0; // Return logical CPU 0
    });

    // sceKernelAllocateDirectMemory (NID 0x2003)
    // Args: arg0 = target address, arg1 = size, arg2 = pointer to write allocated address
    manager.register_handler(0x2003, "sceKernelAllocateDirectMemory", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t arg2) -> std::int64_t {
        if (memory.map(arg0, arg1, read | write) != MemoryError::ok) return -1;
        if (arg2 && memory.store(arg2, &arg0, sizeof(arg0)) != MemoryError::ok) {
            memory.unmap(arg0); // Do not retain a mapping when the guest output is invalid.
            return -1;
        }
        return 0; // Success
    });

    // ========================================================================
    // Orbis OS Semaphores HLE
    // ========================================================================

    // sceKernelCreateSema (NID 0x2010)
    // Args: arg0 = out_id pointer, arg1 = name pointer, arg2 = (initial | (maximum << 16))
    manager.register_handler(0x2010, "sceKernelCreateSema", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t arg2) -> std::int64_t {
        // Validate guest outputs before allocating a host-side semaphore.
        std::uint32_t id = 0;
        if (memory.store(arg0, &id, sizeof(id)) != MemoryError::ok) return ORBIS_KERNEL_ERROR_EINVAL;
        char name_buf[32] = "sema";
        if (arg1) {
            if (memory.load(arg1, name_buf, sizeof(name_buf) - 1) != MemoryError::ok) return ORBIS_KERNEL_ERROR_EINVAL;
            name_buf[sizeof(name_buf) - 1] = '\0';
        }
        std::int32_t initial = static_cast<std::int32_t>(arg2 & 0xFFFF);
        std::int32_t maximum = static_cast<std::int32_t>((arg2 >> 16) & 0xFFFF);
        if (maximum == 0) maximum = std::max(1, initial);

        std::int32_t res = KernelSyncManager::instance().create_sema(&id, name_buf, SCE_KERNEL_SEMA_ATTR_TH_FIFO, initial, maximum);
        if (res == ORBIS_OK && memory.store(arg0, &id, sizeof(id)) != MemoryError::ok) {
            KernelSyncManager::instance().delete_sema(id);
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        return res;
    });

    // sceKernelWaitSema (NID 0x2011)
    // Args: arg0 = id, arg1 = need, arg2 = timeout_usec pointer
    manager.register_handler(0x2011, "sceKernelWaitSema", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t arg2) -> std::int64_t {
        std::uint32_t id = static_cast<std::uint32_t>(arg0);
        std::int32_t need = arg1 ? static_cast<std::int32_t>(arg1) : 1;
        std::uint32_t timeout_val = 0;
        std::uint32_t *p_timeout = nullptr;
        if (arg2) {
            if (memory.load(arg2, &timeout_val, sizeof(timeout_val)) != MemoryError::ok ||
                memory.store(arg2, &timeout_val, sizeof(timeout_val)) != MemoryError::ok)
                return ORBIS_KERNEL_ERROR_EINVAL;
            p_timeout = &timeout_val;
        }
        std::int32_t res = KernelSyncManager::instance().wait_sema(id, need, p_timeout);
        if (p_timeout && arg2) {
            memory.store(arg2, &timeout_val, sizeof(timeout_val));
        }
        return res;
    });

    // sceKernelPollSema (NID 0x2012)
    // Args: arg0 = id, arg1 = need
    manager.register_handler(0x2012, "sceKernelPollSema", [](GuestMemory&, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t) -> std::int64_t {
        std::uint32_t id = static_cast<std::uint32_t>(arg0);
        std::int32_t need = arg1 ? static_cast<std::int32_t>(arg1) : 1;
        return KernelSyncManager::instance().poll_sema(id, need);
    });

    // sceKernelSignalSema (NID 0x2013)
    // Args: arg0 = id, arg1 = count
    manager.register_handler(0x2013, "sceKernelSignalSema", [](GuestMemory&, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t) -> std::int64_t {
        std::uint32_t id = static_cast<std::uint32_t>(arg0);
        std::int32_t count = arg1 ? static_cast<std::int32_t>(arg1) : 1;
        return KernelSyncManager::instance().signal_sema(id, count);
    });

    // sceKernelDeleteSema (NID 0x2015)
    // Args: arg0 = id
    manager.register_handler(0x2015, "sceKernelDeleteSema", [](GuestMemory&, std::uint64_t arg0, std::uint64_t, std::uint64_t) -> std::int64_t {
        std::uint32_t id = static_cast<std::uint32_t>(arg0);
        return KernelSyncManager::instance().delete_sema(id);
    });

    // ========================================================================
    // Orbis OS Light-weight Mutexes HLE
    // ========================================================================

    // sceKernelCreateLwMutex (NID 0x2020)
    // Args: arg0 = work_addr in guest memory, arg1 = name pointer, arg2 = attr
    manager.register_handler(0x2020, "sceKernelCreateLwMutex", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t arg2) -> std::int64_t {
        char name_buf[32] = "lwmutex";
        if (arg1) {
            if (memory.load(arg1, name_buf, sizeof(name_buf) - 1) != MemoryError::ok) return ORBIS_KERNEL_ERROR_EINVAL;
            name_buf[sizeof(name_buf) - 1] = '\0';
        }
        return KernelSyncManager::instance().create_lw_mutex(memory, arg0, name_buf, static_cast<std::uint32_t>(arg2), 0);
    });

    // sceKernelLockLwMutex (NID 0x2021)
    // Args: arg0 = work_addr, arg1 = count, arg2 = timeout_usec pointer
    manager.register_handler(0x2021, "sceKernelLockLwMutex", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t arg2) -> std::int64_t {
        std::int32_t count = arg1 ? static_cast<std::int32_t>(arg1) : 1;
        std::uint32_t timeout_val = 0;
        std::uint32_t *p_timeout = nullptr;
        if (arg2) {
            if (memory.load(arg2, &timeout_val, sizeof(timeout_val)) != MemoryError::ok ||
                memory.store(arg2, &timeout_val, sizeof(timeout_val)) != MemoryError::ok)
                return ORBIS_KERNEL_ERROR_EINVAL;
            p_timeout = &timeout_val;
        }
        std::int32_t res = KernelSyncManager::instance().lock_lw_mutex(memory, arg0, count, p_timeout);
        if (p_timeout && arg2) {
            memory.store(arg2, &timeout_val, sizeof(timeout_val));
        }
        return res;
    });

    // sceKernelTryLockLwMutex (NID 0x2022)
    // Args: arg0 = work_addr, arg1 = count
    manager.register_handler(0x2022, "sceKernelTryLockLwMutex", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t) -> std::int64_t {
        std::int32_t count = arg1 ? static_cast<std::int32_t>(arg1) : 1;
        return KernelSyncManager::instance().trylock_lw_mutex(memory, arg0, count);
    });

    // sceKernelUnlockLwMutex (NID 0x2023)
    // Args: arg0 = work_addr, arg1 = count
    manager.register_handler(0x2023, "sceKernelUnlockLwMutex", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t) -> std::int64_t {
        std::int32_t count = arg1 ? static_cast<std::int32_t>(arg1) : 1;
        return KernelSyncManager::instance().unlock_lw_mutex(memory, arg0, count);
    });

    // sceKernelDeleteLwMutex (NID 0x2024)
    // Args: arg0 = work_addr
    manager.register_handler(0x2024, "sceKernelDeleteLwMutex", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t, std::uint64_t) -> std::int64_t {
        return KernelSyncManager::instance().delete_lw_mutex(memory, arg0);
    });

    // ========================================================================
    // Orbis OS Pthread Mutex & Cond HLE
    // ========================================================================

    // scePthreadMutexInit (NID 0x2030)
    // Args: arg0 = out_id pointer, arg1 = attr pointer
    manager.register_handler(0x2030, "scePthreadMutexInit", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t arg1, std::uint64_t) -> std::int64_t {
        std::uint32_t attr_val = SCE_PTHREAD_MUTEX_NORMAL;
        std::uint32_t id = 0;
        if (memory.store(arg0, &id, sizeof(id)) != MemoryError::ok ||
            (arg1 && memory.load(arg1, &attr_val, sizeof(attr_val)) != MemoryError::ok))
            return ORBIS_KERNEL_ERROR_EINVAL;
        std::int32_t res = KernelSyncManager::instance().pthread_mutex_init(&id, &attr_val);
        if (res == ORBIS_OK && memory.store(arg0, &id, sizeof(id)) != MemoryError::ok) {
            KernelSyncManager::instance().pthread_mutex_destroy(id);
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        return res;
    });

    // scePthreadMutexLock (NID 0x2031)
    manager.register_handler(0x2031, "scePthreadMutexLock", [](GuestMemory&, std::uint64_t arg0, std::uint64_t, std::uint64_t) -> std::int64_t {
        return KernelSyncManager::instance().pthread_mutex_lock(static_cast<std::uint32_t>(arg0));
    });

    // scePthreadMutexUnlock (NID 0x2032)
    manager.register_handler(0x2032, "scePthreadMutexUnlock", [](GuestMemory&, std::uint64_t arg0, std::uint64_t, std::uint64_t) -> std::int64_t {
        return KernelSyncManager::instance().pthread_mutex_unlock(static_cast<std::uint32_t>(arg0));
    });

    // scePthreadMutexDestroy (NID 0x2034)
    manager.register_handler(0x2034, "scePthreadMutexDestroy", [](GuestMemory&, std::uint64_t arg0, std::uint64_t, std::uint64_t) -> std::int64_t {
        return KernelSyncManager::instance().pthread_mutex_destroy(static_cast<std::uint32_t>(arg0));
    });

    // scePthreadCondInit (NID 0x2040)
    manager.register_handler(0x2040, "scePthreadCondInit", [](GuestMemory& memory, std::uint64_t arg0, std::uint64_t, std::uint64_t) -> std::int64_t {
        std::uint32_t id = 0;
        if (memory.store(arg0, &id, sizeof(id)) != MemoryError::ok) return ORBIS_KERNEL_ERROR_EINVAL;
        std::int32_t res = KernelSyncManager::instance().pthread_cond_init(&id, nullptr);
        if (res == ORBIS_OK && memory.store(arg0, &id, sizeof(id)) != MemoryError::ok) {
            KernelSyncManager::instance().pthread_cond_destroy(id);
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        return res;
    });

    // scePthreadCondSignal (NID 0x2042)
    manager.register_handler(0x2042, "scePthreadCondSignal", [](GuestMemory&, std::uint64_t arg0, std::uint64_t, std::uint64_t) -> std::int64_t {
        return KernelSyncManager::instance().pthread_cond_signal(static_cast<std::uint32_t>(arg0));
    });

    // scePthreadCondDestroy (NID 0x2044)
    manager.register_handler(0x2044, "scePthreadCondDestroy", [](GuestMemory&, std::uint64_t arg0, std::uint64_t, std::uint64_t) -> std::int64_t {
        return KernelSyncManager::instance().pthread_cond_destroy(static_cast<std::uint32_t>(arg0));
    });

    return manager;
}

} // namespace bb
