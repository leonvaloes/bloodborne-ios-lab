#include "kernel_threads.h"
#include <algorithm>
#include <chrono>

namespace bb {

// ============================================================================
// OrbisSemaphore Implementation
// ============================================================================

OrbisSemaphore::OrbisSemaphore(std::uint32_t id, const std::string &name, std::uint32_t attr,
                               std::int32_t initial_count, std::int32_t max_count)
    : id_(id), name_(name), attr_(attr), count_(initial_count),
      initial_(initial_count), maximum_(max_count) {}

std::int32_t OrbisSemaphore::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

std::size_t OrbisSemaphore::active_waiters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_.size();
}

std::int32_t OrbisSemaphore::wait(std::int32_t need, std::uint32_t *timeout_usec) {
    if (need <= 0 || need > maximum_) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (deleted_) {
        return ORBIS_KERNEL_ERROR_EACCES;
    }

    // Fast path: if enough tokens are available immediately and no other waiters are queued
    if (waiters_.empty() && count_ >= need) {
        count_ -= need;
        return ORBIS_OK;
    }

    // If zero timeout requested and cannot acquire immediately
    if (timeout_usec && *timeout_usec == 0) {
        return ORBIS_KERNEL_ERROR_ETIMEDOUT;
    }

    auto waiter = std::make_shared<Waiter>();
    waiter->need = need;
    waiters_.push_back(waiter);

    bool timed_out = false;
    const auto start_time = std::chrono::steady_clock::now();

    if (timeout_usec) {
        const auto timeout_dur = std::chrono::microseconds(*timeout_usec);
        while (!waiter->done && !deleted_) {
            if (waiter->cond.wait_for(lock, timeout_dur) == std::cv_status::timeout) {
                if (!waiter->done) {
                    timed_out = true;
                    break;
                }
            }
        }
    } else {
        while (!waiter->done && !deleted_) {
            waiter->cond.wait(lock);
        }
    }

    // Remove from waiters list
    auto it = std::find(waiters_.begin(), waiters_.end(), waiter);
    if (it != waiters_.end()) {
        waiters_.erase(it);
    }

    if (deleted_) {
        return ORBIS_KERNEL_ERROR_EACCES;
    }

    if (timed_out) {
        if (timeout_usec) *timeout_usec = 0;
        return ORBIS_KERNEL_ERROR_ETIMEDOUT;
    }

    if (timeout_usec) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();
        if (static_cast<std::uint64_t>(elapsed) >= *timeout_usec) {
            *timeout_usec = 0;
        } else {
            *timeout_usec -= static_cast<std::uint32_t>(elapsed);
        }
    }

    return waiter->result;
}

std::int32_t OrbisSemaphore::poll(std::int32_t need) {
    if (need <= 0 || need > maximum_) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (deleted_) {
        return ORBIS_KERNEL_ERROR_EACCES;
    }

    if (count_ >= need) {
        count_ -= need;
        return ORBIS_OK;
    }

    return ORBIS_KERNEL_ERROR_EBUSY;
}

std::int32_t OrbisSemaphore::signal(std::int32_t count) {
    if (count <= 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (deleted_) {
        return ORBIS_KERNEL_ERROR_EACCES;
    }

    if (count > maximum_ - count_) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    count_ += count;

    // Wake waiters in FIFO order
    for (auto &waiter : waiters_) {
        if (!waiter->done && waiter->need <= count_) {
            count_ -= waiter->need;
            waiter->done = true;
            waiter->result = ORBIS_OK;
            waiter->cond.notify_one();
        }
    }

    return ORBIS_OK;
}

std::int32_t OrbisSemaphore::cancel(std::int32_t count, std::int32_t *out_waiters) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deleted_) {
        return ORBIS_KERNEL_ERROR_EACCES;
    }

    if (count > maximum_) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::int32_t count_awoken = 0;
    for (auto &waiter : waiters_) {
        if (!waiter->done) {
            waiter->done = true;
            waiter->result = ORBIS_KERNEL_ERROR_ECANCELED;
            waiter->cond.notify_one();
            ++count_awoken;
        }
    }
    waiters_.clear();

    if (out_waiters) {
        *out_waiters = count_awoken;
    }

    count_ = (count < 0) ? initial_ : count;
    return ORBIS_OK;
}

void OrbisSemaphore::close_and_wake_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    deleted_ = true;
    for (auto &waiter : waiters_) {
        if (!waiter->done) {
            waiter->done = true;
            waiter->result = ORBIS_KERNEL_ERROR_EACCES;
            waiter->cond.notify_one();
        }
    }
    waiters_.clear();
}

// ============================================================================
// OrbisLwMutex Implementation
// ============================================================================

OrbisLwMutex::OrbisLwMutex(std::uint32_t id, const std::string &name, std::uint32_t attr, std::int32_t initial_count)
    : id_(id), name_(name), attr_(attr) {
    if (initial_count > 0) {
        lock_count_ = 1;
        recursive_count_ = initial_count;
        owner_ = std::this_thread::get_id();
        has_owner_ = true;
    }
}

bool OrbisLwMutex::is_locked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lock_count_ > 0;
}

std::int32_t OrbisLwMutex::recursive_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recursive_count_;
}

std::size_t OrbisLwMutex::wait_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wait_count_;
}

std::int32_t OrbisLwMutex::lock(std::int32_t count, std::uint32_t *timeout_usec) {
    if (count <= 0) return ORBIS_KERNEL_ERROR_EINVAL;

    const auto current_tid = std::this_thread::get_id();
    std::unique_lock<std::mutex> lock(mutex_);
    if (deleted_) return ORBIS_KERNEL_ERROR_EACCES;

    // Recursive lock check
    if (has_owner_ && owner_ == current_tid) {
        if (attr_ & SCE_KERNEL_LW_MUTEX_ATTR_RECURSIVE) {
            recursive_count_ += count;
            return ORBIS_OK;
        }
        return ORBIS_KERNEL_ERROR_EDEADLK;
    }

    // Try immediate acquire
    if (!has_owner_) {
        has_owner_ = true;
        owner_ = current_tid;
        lock_count_ = 1;
        recursive_count_ = count;
        return ORBIS_OK;
    }

    if (timeout_usec && *timeout_usec == 0) {
        return ORBIS_KERNEL_ERROR_EBUSY;
    }

    ++wait_count_;
    const auto start_time = std::chrono::steady_clock::now();
    bool timed_out = false;

    if (timeout_usec) {
        const auto timeout_dur = std::chrono::microseconds(*timeout_usec);
        while (has_owner_ && !deleted_) {
            if (cond_.wait_for(lock, timeout_dur) == std::cv_status::timeout) {
                if (has_owner_) {
                    timed_out = true;
                    break;
                }
            }
        }
    } else {
        while (has_owner_ && !deleted_) {
            cond_.wait(lock);
        }
    }

    --wait_count_;

    if (deleted_) return ORBIS_KERNEL_ERROR_EACCES;
    if (timed_out) {
        if (timeout_usec) *timeout_usec = 0;
        return ORBIS_KERNEL_ERROR_ETIMEDOUT;
    }

    has_owner_ = true;
    owner_ = current_tid;
    lock_count_ = 1;
    recursive_count_ = count;

    if (timeout_usec) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();
        if (static_cast<std::uint64_t>(elapsed) >= *timeout_usec) {
            *timeout_usec = 0;
        } else {
            *timeout_usec -= static_cast<std::uint32_t>(elapsed);
        }
    }

    return ORBIS_OK;
}

std::int32_t OrbisLwMutex::trylock(std::int32_t count) {
    if (count <= 0) return ORBIS_KERNEL_ERROR_EINVAL;

    const auto current_tid = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(mutex_);
    if (deleted_) return ORBIS_KERNEL_ERROR_EACCES;

    if (has_owner_) {
        if (owner_ == current_tid && (attr_ & SCE_KERNEL_LW_MUTEX_ATTR_RECURSIVE)) {
            recursive_count_ += count;
            return ORBIS_OK;
        }
        return ORBIS_KERNEL_ERROR_EBUSY;
    }

    has_owner_ = true;
    owner_ = current_tid;
    lock_count_ = 1;
    recursive_count_ = count;
    return ORBIS_OK;
}

std::int32_t OrbisLwMutex::unlock(std::int32_t count) {
    if (count <= 0) return ORBIS_KERNEL_ERROR_EINVAL;

    const auto current_tid = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(mutex_);
    if (deleted_) return ORBIS_KERNEL_ERROR_EACCES;

    if (!has_owner_ || owner_ != current_tid) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }

    if (recursive_count_ > count) {
        recursive_count_ -= count;
        return ORBIS_OK;
    }

    has_owner_ = false;
    lock_count_ = 0;
    recursive_count_ = 0;
    cond_.notify_one();
    return ORBIS_OK;
}

void OrbisLwMutex::close_and_wake_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    deleted_ = true;
    has_owner_ = false;
    lock_count_ = 0;
    cond_.notify_all();
}

// ============================================================================
// OrbisPthreadMutex Implementation
// ============================================================================

OrbisPthreadMutex::OrbisPthreadMutex(std::uint32_t id, std::uint32_t type)
    : id_(id), type_(type) {}

std::int32_t OrbisPthreadMutex::lock() {
    const auto current_tid = std::this_thread::get_id();
    std::unique_lock<std::mutex> lock(mutex_);
    if (deleted_) return ORBIS_KERNEL_ERROR_EINVAL;

    if (locked_ && owner_ == current_tid) {
        if (type_ == SCE_PTHREAD_MUTEX_RECURSIVE) {
            ++recursion_;
            return ORBIS_OK;
        }
        return ORBIS_KERNEL_ERROR_EDEADLK;
    }

    while (locked_ && !deleted_) {
        cond_.wait(lock);
    }

    if (deleted_) return ORBIS_KERNEL_ERROR_EINVAL;

    locked_ = true;
    owner_ = current_tid;
    recursion_ = 1;
    return ORBIS_OK;
}

std::int32_t OrbisPthreadMutex::trylock() {
    const auto current_tid = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(mutex_);
    if (deleted_) return ORBIS_KERNEL_ERROR_EINVAL;

    if (locked_) {
        if (owner_ == current_tid && type_ == SCE_PTHREAD_MUTEX_RECURSIVE) {
            ++recursion_;
            return ORBIS_OK;
        }
        return ORBIS_KERNEL_ERROR_EBUSY;
    }

    locked_ = true;
    owner_ = current_tid;
    recursion_ = 1;
    return ORBIS_OK;
}

std::int32_t OrbisPthreadMutex::timedlock(std::uint32_t timeout_usec) {
    const auto current_tid = std::this_thread::get_id();
    std::unique_lock<std::mutex> lock(mutex_);
    if (deleted_) return ORBIS_KERNEL_ERROR_EINVAL;

    if (locked_ && owner_ == current_tid) {
        if (type_ == SCE_PTHREAD_MUTEX_RECURSIVE) {
            ++recursion_;
            return ORBIS_OK;
        }
        return ORBIS_KERNEL_ERROR_EDEADLK;
    }

    if (!locked_) {
        locked_ = true;
        owner_ = current_tid;
        recursion_ = 1;
        return ORBIS_OK;
    }

    if (timeout_usec == 0) return ORBIS_KERNEL_ERROR_ETIMEDOUT;

    const auto dur = std::chrono::microseconds(timeout_usec);
    if (!cond_.wait_for(lock, dur, [&] { return !locked_ || deleted_; })) {
        return ORBIS_KERNEL_ERROR_ETIMEDOUT;
    }

    if (deleted_) return ORBIS_KERNEL_ERROR_EINVAL;

    locked_ = true;
    owner_ = current_tid;
    recursion_ = 1;
    return ORBIS_OK;
}

std::int32_t OrbisPthreadMutex::unlock() {
    const auto current_tid = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(mutex_);
    if (deleted_) return ORBIS_KERNEL_ERROR_EINVAL;

    if (!locked_ || owner_ != current_tid) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }

    if (recursion_ > 1) {
        --recursion_;
        return ORBIS_OK;
    }

    locked_ = false;
    recursion_ = 0;
    cond_.notify_one();
    return ORBIS_OK;
}

void OrbisPthreadMutex::close_and_wake_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    deleted_ = true;
    locked_ = false;
    cond_.notify_all();
}

// ============================================================================
// OrbisPthreadCond Implementation
// ============================================================================

OrbisPthreadCond::OrbisPthreadCond(std::uint32_t id) : id_(id) {}

std::int32_t OrbisPthreadCond::wait(OrbisPthreadMutex &mutex) {
    std::unique_lock<std::mutex> local_lock(mutex_);
    if (deleted_) return ORBIS_KERNEL_ERROR_EINVAL;

    mutex.unlock();
    cond_.wait(local_lock);
    mutex.lock();
    return ORBIS_OK;
}

std::int32_t OrbisPthreadCond::timedwait(OrbisPthreadMutex &mutex, std::uint32_t timeout_usec) {
    std::unique_lock<std::mutex> local_lock(mutex_);
    if (deleted_) return ORBIS_KERNEL_ERROR_EINVAL;

    mutex.unlock();
    const auto dur = std::chrono::microseconds(timeout_usec);
    const auto res = cond_.wait_for(local_lock, dur);
    mutex.lock();

    if (res == std::cv_status::timeout) {
        return ORBIS_KERNEL_ERROR_ETIMEDOUT;
    }
    return ORBIS_OK;
}

std::int32_t OrbisPthreadCond::signal() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deleted_) return ORBIS_KERNEL_ERROR_EINVAL;
    cond_.notify_one();
    return ORBIS_OK;
}

std::int32_t OrbisPthreadCond::broadcast() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deleted_) return ORBIS_KERNEL_ERROR_EINVAL;
    cond_.notify_all();
    return ORBIS_OK;
}

void OrbisPthreadCond::close_and_wake_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    deleted_ = true;
    cond_.notify_all();
}

// ============================================================================
// KernelSyncManager Implementation
// ============================================================================

KernelSyncManager::KernelSyncManager() = default;
KernelSyncManager::~KernelSyncManager() {
    reset();
}

KernelSyncManager &KernelSyncManager::instance() {
    static KernelSyncManager s_instance;
    return s_instance;
}

void KernelSyncManager::reset() {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    for (auto &kv : semaphores_) {
        kv.second->close_and_wake_all();
    }
    semaphores_.clear();

    for (auto &kv : lw_mutexes_) {
        kv.second->close_and_wake_all();
    }
    lw_mutexes_.clear();

    for (auto &kv : pthread_mutexes_) {
        kv.second->close_and_wake_all();
    }
    pthread_mutexes_.clear();

    for (auto &kv : pthread_conds_) {
        kv.second->close_and_wake_all();
    }
    pthread_conds_.clear();

    next_sema_id_ = 1;
    next_lw_mutex_id_ = 1;
    next_pthread_mutex_id_ = 1;
    next_pthread_cond_id_ = 1;
}

std::int32_t KernelSyncManager::create_sema(std::uint32_t *out_id, const char *name, std::uint32_t attr,
                                           std::int32_t initial_count, std::int32_t max_count, const void *) {
    if (!out_id || !name || attr > 2 || initial_count < 0 || max_count <= 0 || initial_count > max_count) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard<std::mutex> lock(registry_mutex_);
    std::uint32_t id = next_sema_id_++;
    auto sema = std::make_shared<OrbisSemaphore>(id, name, attr, initial_count, max_count);
    semaphores_[id] = sema;
    *out_id = id;
    return ORBIS_OK;
}

std::int32_t KernelSyncManager::wait_sema(std::uint32_t id, std::int32_t need, std::uint32_t *timeout_usec) {
    std::shared_ptr<OrbisSemaphore> sema;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = semaphores_.find(id);
        if (it == semaphores_.end()) {
            return ORBIS_KERNEL_ERROR_ESRCH;
        }
        sema = it->second;
    }
    return sema->wait(need, timeout_usec);
}

std::int32_t KernelSyncManager::poll_sema(std::uint32_t id, std::int32_t need) {
    std::shared_ptr<OrbisSemaphore> sema;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = semaphores_.find(id);
        if (it == semaphores_.end()) {
            return ORBIS_KERNEL_ERROR_ESRCH;
        }
        sema = it->second;
    }
    return sema->poll(need);
}

std::int32_t KernelSyncManager::signal_sema(std::uint32_t id, std::int32_t count) {
    std::shared_ptr<OrbisSemaphore> sema;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = semaphores_.find(id);
        if (it == semaphores_.end()) {
            return ORBIS_KERNEL_ERROR_ESRCH;
        }
        sema = it->second;
    }
    return sema->signal(count);
}

std::int32_t KernelSyncManager::cancel_sema(std::uint32_t id, std::int32_t count, std::int32_t *out_waiters) {
    std::shared_ptr<OrbisSemaphore> sema;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = semaphores_.find(id);
        if (it == semaphores_.end()) {
            return ORBIS_KERNEL_ERROR_ESRCH;
        }
        sema = it->second;
    }
    return sema->cancel(count, out_waiters);
}

std::int32_t KernelSyncManager::delete_sema(std::uint32_t id) {
    std::shared_ptr<OrbisSemaphore> sema;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = semaphores_.find(id);
        if (it == semaphores_.end()) {
            return ORBIS_KERNEL_ERROR_ESRCH;
        }
        sema = it->second;
        semaphores_.erase(it);
    }
    sema->close_and_wake_all();
    return ORBIS_OK;
}

std::size_t KernelSyncManager::sema_waiters(std::uint32_t id) const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto it = semaphores_.find(id);
    if (it == semaphores_.end()) return 0;
    return it->second->active_waiters();
}

std::int32_t KernelSyncManager::create_lw_mutex(std::uint32_t *out_id, const char *name, std::uint32_t attr,
                                                std::int32_t initial_count, const void *) {
    if (!out_id || !name || initial_count < 0 || initial_count > 1) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard<std::mutex> lock(registry_mutex_);
    std::uint32_t id = next_lw_mutex_id_++;
    auto mutex = std::make_shared<OrbisLwMutex>(id, name, attr, initial_count);
    lw_mutexes_[id] = mutex;
    *out_id = id;
    return ORBIS_OK;
}

std::int32_t KernelSyncManager::lock_lw_mutex(std::uint32_t id, std::int32_t count, std::uint32_t *timeout_usec) {
    std::shared_ptr<OrbisLwMutex> mutex;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = lw_mutexes_.find(id);
        if (it == lw_mutexes_.end()) {
            return ORBIS_KERNEL_ERROR_ESRCH;
        }
        mutex = it->second;
    }
    return mutex->lock(count, timeout_usec);
}

std::int32_t KernelSyncManager::trylock_lw_mutex(std::uint32_t id, std::int32_t count) {
    std::shared_ptr<OrbisLwMutex> mutex;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = lw_mutexes_.find(id);
        if (it == lw_mutexes_.end()) {
            return ORBIS_KERNEL_ERROR_ESRCH;
        }
        mutex = it->second;
    }
    return mutex->trylock(count);
}

std::int32_t KernelSyncManager::unlock_lw_mutex(std::uint32_t id, std::int32_t count) {
    std::shared_ptr<OrbisLwMutex> mutex;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = lw_mutexes_.find(id);
        if (it == lw_mutexes_.end()) {
            return ORBIS_KERNEL_ERROR_ESRCH;
        }
        mutex = it->second;
    }
    return mutex->unlock(count);
}

std::int32_t KernelSyncManager::delete_lw_mutex(std::uint32_t id) {
    std::shared_ptr<OrbisLwMutex> mutex;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = lw_mutexes_.find(id);
        if (it == lw_mutexes_.end()) {
            return ORBIS_KERNEL_ERROR_ESRCH;
        }
        mutex = it->second;
        lw_mutexes_.erase(it);
    }
    mutex->close_and_wake_all();
    return ORBIS_OK;
}

// GuestMemory Struct-based Lightweight Mutex methods
std::int32_t KernelSyncManager::create_lw_mutex(GuestMemory &mem, std::uint64_t work_addr, const char *name,
                                                std::uint32_t attr, std::int32_t initial_count) {
    if (!work_addr || !name || initial_count < 0 || initial_count > 1) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::uint32_t id = 0;
    std::int32_t err = create_lw_mutex(&id, name, attr, initial_count, nullptr);
    if (err != ORBIS_OK) return err;

    SceKernelLwMutexWork work{};
    work.id = id;
    work.attr = attr;
    work.lock_var = (initial_count > 0) ? 1 : 0;
    work.recursive_count = initial_count;
    work.wait_count = 0;
    std::snprintf(work.name, sizeof(work.name), "%s", name);

    if (mem.store(work_addr, &work, sizeof(work)) != MemoryError::ok) {
        delete_lw_mutex(id);
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    return ORBIS_OK;
}

std::int32_t KernelSyncManager::lock_lw_mutex(GuestMemory &mem, std::uint64_t work_addr, std::int32_t count, std::uint32_t *timeout_usec) {
    SceKernelLwMutexWork work{};
    if (mem.load(work_addr, &work, sizeof(work)) != MemoryError::ok) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::int32_t res = lock_lw_mutex(work.id, count, timeout_usec);
    if (res == ORBIS_OK) {
        work.lock_var = 1;
        work.recursive_count += count;
        mem.store(work_addr, &work, sizeof(work));
    }
    return res;
}

std::int32_t KernelSyncManager::trylock_lw_mutex(GuestMemory &mem, std::uint64_t work_addr, std::int32_t count) {
    SceKernelLwMutexWork work{};
    if (mem.load(work_addr, &work, sizeof(work)) != MemoryError::ok) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::int32_t res = trylock_lw_mutex(work.id, count);
    if (res == ORBIS_OK) {
        work.lock_var = 1;
        work.recursive_count += count;
        mem.store(work_addr, &work, sizeof(work));
    }
    return res;
}

std::int32_t KernelSyncManager::unlock_lw_mutex(GuestMemory &mem, std::uint64_t work_addr, std::int32_t count) {
    SceKernelLwMutexWork work{};
    if (mem.load(work_addr, &work, sizeof(work)) != MemoryError::ok) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::int32_t res = unlock_lw_mutex(work.id, count);
    if (res == ORBIS_OK) {
        if (work.recursive_count > count) {
            work.recursive_count -= count;
        } else {
            work.lock_var = 0;
            work.recursive_count = 0;
        }
        mem.store(work_addr, &work, sizeof(work));
    }
    return res;
}

std::int32_t KernelSyncManager::delete_lw_mutex(GuestMemory &mem, std::uint64_t work_addr) {
    SceKernelLwMutexWork work{};
    if (mem.load(work_addr, &work, sizeof(work)) != MemoryError::ok) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    return delete_lw_mutex(work.id);
}

// Pthread Mutex Implementations
std::int32_t KernelSyncManager::pthread_mutex_init(std::uint32_t *out_id, const std::uint32_t *attr) {
    if (!out_id) return ORBIS_KERNEL_ERROR_EINVAL;
    std::uint32_t type = attr ? *attr : SCE_PTHREAD_MUTEX_NORMAL;

    std::lock_guard<std::mutex> lock(registry_mutex_);
    std::uint32_t id = next_pthread_mutex_id_++;
    pthread_mutexes_[id] = std::make_shared<OrbisPthreadMutex>(id, type);
    *out_id = id;
    return ORBIS_OK;
}

std::int32_t KernelSyncManager::pthread_mutex_lock(std::uint32_t id) {
    std::shared_ptr<OrbisPthreadMutex> m;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = pthread_mutexes_.find(id);
        if (it == pthread_mutexes_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
        m = it->second;
    }
    return m->lock();
}

std::int32_t KernelSyncManager::pthread_mutex_trylock(std::uint32_t id) {
    std::shared_ptr<OrbisPthreadMutex> m;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = pthread_mutexes_.find(id);
        if (it == pthread_mutexes_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
        m = it->second;
    }
    return m->trylock();
}

std::int32_t KernelSyncManager::pthread_mutex_timedlock(std::uint32_t id, std::uint32_t timeout_usec) {
    std::shared_ptr<OrbisPthreadMutex> m;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = pthread_mutexes_.find(id);
        if (it == pthread_mutexes_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
        m = it->second;
    }
    return m->timedlock(timeout_usec);
}

std::int32_t KernelSyncManager::pthread_mutex_unlock(std::uint32_t id) {
    std::shared_ptr<OrbisPthreadMutex> m;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = pthread_mutexes_.find(id);
        if (it == pthread_mutexes_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
        m = it->second;
    }
    return m->unlock();
}

std::int32_t KernelSyncManager::pthread_mutex_destroy(std::uint32_t id) {
    std::shared_ptr<OrbisPthreadMutex> m;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = pthread_mutexes_.find(id);
        if (it == pthread_mutexes_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
        m = it->second;
        pthread_mutexes_.erase(it);
    }
    m->close_and_wake_all();
    return ORBIS_OK;
}

// Pthread Condition Variable Implementations
std::int32_t KernelSyncManager::pthread_cond_init(std::uint32_t *out_id, const void *) {
    if (!out_id) return ORBIS_KERNEL_ERROR_EINVAL;

    std::lock_guard<std::mutex> lock(registry_mutex_);
    std::uint32_t id = next_pthread_cond_id_++;
    pthread_conds_[id] = std::make_shared<OrbisPthreadCond>(id);
    *out_id = id;
    return ORBIS_OK;
}

std::int32_t KernelSyncManager::pthread_cond_wait(std::uint32_t cond_id, std::uint32_t mutex_id) {
    std::shared_ptr<OrbisPthreadCond> c;
    std::shared_ptr<OrbisPthreadMutex> m;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it_c = pthread_conds_.find(cond_id);
        auto it_m = pthread_mutexes_.find(mutex_id);
        if (it_c == pthread_conds_.end() || it_m == pthread_mutexes_.end()) {
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        c = it_c->second;
        m = it_m->second;
    }
    return c->wait(*m);
}

std::int32_t KernelSyncManager::pthread_cond_timedwait(std::uint32_t cond_id, std::uint32_t mutex_id, std::uint32_t timeout_usec) {
    std::shared_ptr<OrbisPthreadCond> c;
    std::shared_ptr<OrbisPthreadMutex> m;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it_c = pthread_conds_.find(cond_id);
        auto it_m = pthread_mutexes_.find(mutex_id);
        if (it_c == pthread_conds_.end() || it_m == pthread_mutexes_.end()) {
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        c = it_c->second;
        m = it_m->second;
    }
    return c->timedwait(*m, timeout_usec);
}

std::int32_t KernelSyncManager::pthread_cond_signal(std::uint32_t cond_id) {
    std::shared_ptr<OrbisPthreadCond> c;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = pthread_conds_.find(cond_id);
        if (it == pthread_conds_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
        c = it->second;
    }
    return c->signal();
}

std::int32_t KernelSyncManager::pthread_cond_broadcast(std::uint32_t cond_id) {
    std::shared_ptr<OrbisPthreadCond> c;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = pthread_conds_.find(cond_id);
        if (it == pthread_conds_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
        c = it->second;
    }
    return c->broadcast();
}

std::int32_t KernelSyncManager::pthread_cond_destroy(std::uint32_t cond_id) {
    std::shared_ptr<OrbisPthreadCond> c;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = pthread_conds_.find(cond_id);
        if (it == pthread_conds_.end()) return ORBIS_KERNEL_ERROR_EINVAL;
        c = it->second;
        pthread_conds_.erase(it);
    }
    c->close_and_wake_all();
    return ORBIS_OK;
}

std::size_t KernelSyncManager::total_semas() const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    return semaphores_.size();
}

std::size_t KernelSyncManager::total_lw_mutexes() const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    return lw_mutexes_.size();
}

std::size_t KernelSyncManager::total_pthread_mutexes() const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    return pthread_mutexes_.size();
}

std::size_t KernelSyncManager::total_pthread_conds() const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    return pthread_conds_.size();
}

} // namespace bb
