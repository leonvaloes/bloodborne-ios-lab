#ifndef BB_KERNEL_THREADS_H
#define BB_KERNEL_THREADS_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <thread>
#include <chrono>
#include "guest_memory.h"

namespace bb {

// Orbis OS Kernel Error Codes
constexpr std::int32_t ORBIS_OK = 0;
constexpr std::int32_t ORBIS_KERNEL_ERROR_EPERM = static_cast<std::int32_t>(0x80020001);
constexpr std::int32_t ORBIS_KERNEL_ERROR_ESRCH = static_cast<std::int32_t>(0x80020003);
constexpr std::int32_t ORBIS_KERNEL_ERROR_EDEADLK = static_cast<std::int32_t>(0x8002000b);
constexpr std::int32_t ORBIS_KERNEL_ERROR_ENOMEM = static_cast<std::int32_t>(0x8002000c);
constexpr std::int32_t ORBIS_KERNEL_ERROR_EACCES = static_cast<std::int32_t>(0x8002000d);
constexpr std::int32_t ORBIS_KERNEL_ERROR_EBUSY = static_cast<std::int32_t>(0x80020010);
constexpr std::int32_t ORBIS_KERNEL_ERROR_EINVAL = static_cast<std::int32_t>(0x80020016);
constexpr std::int32_t ORBIS_KERNEL_ERROR_ENOSPC = static_cast<std::int32_t>(0x8002001c);
constexpr std::int32_t ORBIS_KERNEL_ERROR_ETIMEDOUT = static_cast<std::int32_t>(0x8002003c);
constexpr std::int32_t ORBIS_KERNEL_ERROR_ECANCELED = static_cast<std::int32_t>(0x80020055);

// Orbis Semaphore Attributes
constexpr std::uint32_t SCE_KERNEL_SEMA_ATTR_TH_FIFO = 0x00000000;
constexpr std::uint32_t SCE_KERNEL_SEMA_ATTR_TH_PRIO = 0x00000002;

// Orbis Light-weight Mutex Attributes
constexpr std::uint32_t SCE_KERNEL_LW_MUTEX_ATTR_TH_FIFO = 0x00000000;
constexpr std::uint32_t SCE_KERNEL_LW_MUTEX_ATTR_RECURSIVE = 0x00000002;
constexpr std::uint32_t SCE_KERNEL_LW_MUTEX_ATTR_TH_PRIO = 0x00000004;

// Pthread Mutex Types
constexpr std::uint32_t SCE_PTHREAD_MUTEX_NORMAL = 1;
constexpr std::uint32_t SCE_PTHREAD_MUTEX_RECURSIVE = 2;
constexpr std::uint32_t SCE_PTHREAD_MUTEX_ERRORCHECK = 3;

#pragma pack(push, 1)
struct SceKernelLwMutexWork {
    std::uint64_t lock_var;
    std::uint32_t attr;
    std::int32_t recursive_count;
    std::uint32_t wait_count;
    std::uint32_t id;
    char name[32];
};
#pragma pack(pop)

// Structure representing an Orbis Semaphore
class OrbisSemaphore {
public:
    OrbisSemaphore(std::uint32_t id, const std::string &name, std::uint32_t attr,
                   std::int32_t initial_count, std::int32_t max_count);

    std::uint32_t id() const { return id_; }
    const std::string &name() const { return name_; }
    std::uint32_t attr() const { return attr_; }
    std::int32_t count() const;
    std::int32_t maximum() const { return maximum_; }
    std::size_t active_waiters() const;

    std::int32_t wait(std::int32_t need, std::uint32_t *timeout_usec);
    std::int32_t poll(std::int32_t need);
    std::int32_t signal(std::int32_t count);
    std::int32_t cancel(std::int32_t count, std::int32_t *out_waiters);
    void close_and_wake_all();

private:
    struct Waiter {
        std::int32_t need{0};
        std::int32_t result{0};
        bool done{false};
        std::condition_variable cond;
    };

    std::uint32_t id_{0};
    std::string name_;
    std::uint32_t attr_{0};
    std::int32_t count_{0};
    std::int32_t initial_{0};
    std::int32_t maximum_{0};
    bool deleted_{false};
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Waiter>> waiters_;
};

// Structure representing an Orbis Light-weight Mutex
class OrbisLwMutex {
public:
    OrbisLwMutex(std::uint32_t id, const std::string &name, std::uint32_t attr, std::int32_t initial_count);

    std::uint32_t id() const { return id_; }
    const std::string &name() const { return name_; }
    std::uint32_t attr() const { return attr_; }
    bool is_locked() const;
    std::int32_t recursive_count() const;
    std::size_t wait_count() const;

    std::int32_t lock(std::int32_t count, std::uint32_t *timeout_usec);
    std::int32_t trylock(std::int32_t count);
    std::int32_t unlock(std::int32_t count);
    void close_and_wake_all();

private:
    std::uint32_t id_{0};
    std::string name_;
    std::uint32_t attr_{0};
    std::int32_t lock_count_{0};
    std::int32_t recursive_count_{0};
    std::thread::id owner_{};
    bool has_owner_{false};
    bool deleted_{false};
    std::size_t wait_count_{0};
    mutable std::mutex mutex_;
    std::condition_variable cond_;
};

// Structure representing an Orbis Pthread Mutex
class OrbisPthreadMutex {
public:
    explicit OrbisPthreadMutex(std::uint32_t id, std::uint32_t type = SCE_PTHREAD_MUTEX_NORMAL);

    std::uint32_t id() const { return id_; }
    std::uint32_t type() const { return type_; }

    std::int32_t lock();
    std::int32_t trylock();
    std::int32_t timedlock(std::uint32_t timeout_usec);
    std::int32_t unlock();
    void close_and_wake_all();

    std::unique_lock<std::mutex> acquire_internal_lock() {
        return std::unique_lock<std::mutex>(mutex_);
    }

private:
    std::uint32_t id_{0};
    std::uint32_t type_{SCE_PTHREAD_MUTEX_NORMAL};
    bool locked_{false};
    std::int32_t recursion_{0};
    std::thread::id owner_{};
    bool deleted_{false};
    std::mutex mutex_;
    std::condition_variable cond_;
};

// Structure representing an Orbis Pthread Condition Variable
class OrbisPthreadCond {
public:
    explicit OrbisPthreadCond(std::uint32_t id);

    std::uint32_t id() const { return id_; }

    std::int32_t wait(OrbisPthreadMutex &mutex);
    std::int32_t timedwait(OrbisPthreadMutex &mutex, std::uint32_t timeout_usec);
    std::int32_t signal();
    std::int32_t broadcast();
    void close_and_wake_all();

private:
    std::uint32_t id_{0};
    bool deleted_{false};
    std::mutex mutex_;
    std::condition_variable cond_;
};

// Central Manager for Orbis OS synchronization primitives
class KernelSyncManager {
public:
    KernelSyncManager();
    ~KernelSyncManager();

    static KernelSyncManager &instance();

    void reset();

    // Semaphore API
    std::int32_t create_sema(std::uint32_t *out_id, const char *name, std::uint32_t attr,
                             std::int32_t initial_count, std::int32_t max_count, const void *opt = nullptr);
    std::int32_t wait_sema(std::uint32_t id, std::int32_t need, std::uint32_t *timeout_usec = nullptr);
    std::int32_t poll_sema(std::uint32_t id, std::int32_t need);
    std::int32_t signal_sema(std::uint32_t id, std::int32_t count);
    std::int32_t cancel_sema(std::uint32_t id, std::int32_t count, std::int32_t *out_waiters = nullptr);
    std::int32_t delete_sema(std::uint32_t id);
    std::size_t sema_waiters(std::uint32_t id) const;

    // Light-weight Mutex API (Handle-based)
    std::int32_t create_lw_mutex(std::uint32_t *out_id, const char *name, std::uint32_t attr,
                                 std::int32_t initial_count, const void *opt = nullptr);
    std::int32_t lock_lw_mutex(std::uint32_t id, std::int32_t count = 1, std::uint32_t *timeout_usec = nullptr);
    std::int32_t trylock_lw_mutex(std::uint32_t id, std::int32_t count = 1);
    std::int32_t unlock_lw_mutex(std::uint32_t id, std::int32_t count = 1);
    std::int32_t delete_lw_mutex(std::uint32_t id);

    // Light-weight Mutex API (GuestMemory struct-based)
    std::int32_t create_lw_mutex(GuestMemory &mem, std::uint64_t work_addr, const char *name,
                                 std::uint32_t attr, std::int32_t initial_count);
    std::int32_t lock_lw_mutex(GuestMemory &mem, std::uint64_t work_addr, std::int32_t count, std::uint32_t *timeout_usec);
    std::int32_t trylock_lw_mutex(GuestMemory &mem, std::uint64_t work_addr, std::int32_t count);
    std::int32_t unlock_lw_mutex(GuestMemory &mem, std::uint64_t work_addr, std::int32_t count);
    std::int32_t delete_lw_mutex(GuestMemory &mem, std::uint64_t work_addr);

    // Pthread Mutex API
    std::int32_t pthread_mutex_init(std::uint32_t *out_id, const std::uint32_t *attr = nullptr);
    std::int32_t pthread_mutex_lock(std::uint32_t id);
    std::int32_t pthread_mutex_trylock(std::uint32_t id);
    std::int32_t pthread_mutex_timedlock(std::uint32_t id, std::uint32_t timeout_usec);
    std::int32_t pthread_mutex_unlock(std::uint32_t id);
    std::int32_t pthread_mutex_destroy(std::uint32_t id);

    // Pthread Condition Variable API
    std::int32_t pthread_cond_init(std::uint32_t *out_id, const void *attr = nullptr);
    std::int32_t pthread_cond_wait(std::uint32_t cond_id, std::uint32_t mutex_id);
    std::int32_t pthread_cond_timedwait(std::uint32_t cond_id, std::uint32_t mutex_id, std::uint32_t timeout_usec);
    std::int32_t pthread_cond_signal(std::uint32_t cond_id);
    std::int32_t pthread_cond_broadcast(std::uint32_t cond_id);
    std::int32_t pthread_cond_destroy(std::uint32_t cond_id);

    // Statistics
    std::size_t total_semas() const;
    std::size_t total_lw_mutexes() const;
    std::size_t total_pthread_mutexes() const;
    std::size_t total_pthread_conds() const;

private:
    mutable std::mutex registry_mutex_;
    std::uint32_t next_sema_id_{1};
    std::uint32_t next_lw_mutex_id_{1};
    std::uint32_t next_pthread_mutex_id_{1};
    std::uint32_t next_pthread_cond_id_{1};

    std::unordered_map<std::uint32_t, std::shared_ptr<OrbisSemaphore>> semaphores_;
    std::unordered_map<std::uint32_t, std::shared_ptr<OrbisLwMutex>> lw_mutexes_;
    std::unordered_map<std::uint32_t, std::shared_ptr<OrbisPthreadMutex>> pthread_mutexes_;
    std::unordered_map<std::uint32_t, std::shared_ptr<OrbisPthreadCond>> pthread_conds_;
};

} // namespace bb

#endif // BB_KERNEL_THREADS_H
