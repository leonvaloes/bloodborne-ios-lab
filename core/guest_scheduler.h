#pragma once
#include "x86_interp.h"
#include "kernel_threads.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace bb {
// Single-host-thread, deterministic guest scheduler. All methods are called between
// interpreter runs, never from a guest callback. IDs are monotonic and never reused.
class GuestScheduler {
public:
    using Id = std::uint32_t;
    enum class State { created, ready, waiting, exited, faulted };
    enum class Stop { budget, idle, deadlock, fault };
    struct Result { Stop stop; std::uint64_t steps; Id thread; CpuRunResult cpu; };
    struct Callback { std::uint64_t function, argument; };
    using Trap = std::function<bool(GuestScheduler&, Id, CpuState&)>;
    using FaultHandler = std::function<bool(GuestScheduler&, Id, CpuState&, const CpuRunResult&)>;
    using PauseChecker = std::function<bool()>;
    explicit GuestScheduler(GuestMemory& memory, std::uint64_t quantum = 256);
    void add_trap_range(std::uint64_t base, std::uint64_t size);
    void set_trap(Trap trap) { trap_ = std::move(trap); }
    void set_fault_handler(FaultHandler handler) { fault_handler_ = std::move(handler); }
    void set_pause_checker(PauseChecker checker) { pause_checker_ = std::move(checker); }
    // Despacho de blocos de codigo compilados AOT (T051)
    void register_aot_block(std::uint64_t rip, AotBlockHandler handler) {
        interpreter_.register_aot_block(rip, std::move(handler));
    }
    void unregister_aot_block(std::uint64_t rip) {
        interpreter_.unregister_aot_block(rip);
    }
    bool has_aot_block(std::uint64_t rip) const {
        return interpreter_.has_aot_block(rip);
    }
    void clear_aot_blocks() {
        interpreter_.clear_aot_blocks();
    }
    std::size_t aot_block_count() const {
        return interpreter_.aot_block_count();
    }
    // Caller maps disjoint stacks; create() places the return trap at the
    // SysV entry rsp (rsp % 16 == 8), with [rsp, rsp+8) inside the stack.
    Id create(std::uint64_t entry, std::uint64_t stack_bottom, std::uint64_t stack_top,
              std::uint64_t return_trap, std::uint64_t argument = 0, std::uint64_t fs_base = 0);
    bool abort_created(Id id);
    // Adopts an existing thread CPU state (e.g. main thread set up by bootstrap) into the scheduler.
    Id adopt(const CpuState& state, std::uint64_t stack_bottom, std::uint64_t stack_top);
    bool has_thread(Id id) const { return threads_.find(id) != threads_.end(); }
    std::size_t thread_count() const { return threads_.size(); }
    std::int32_t start(Id id);
    std::int32_t exit(Id id, std::uint64_t value);
    std::int32_t join(Id caller, Id target, std::uint64_t* value = nullptr,
                      std::uint64_t timeout = 0); // zero means infinite; result on resume()
    std::int32_t resume(Id id) const;
    std::int32_t cancel(Id id);
    State state(Id id) const;
    const CpuState& cpu(Id id) const;
    CpuState& cpu(Id id);
    std::uint64_t value(Id id) const;
    enum class ClockMode { Steps, Decoupled };
    void set_clock_mode(ClockMode mode) { clock_mode_ = mode; }
    ClockMode clock_mode() const { return clock_mode_; }
    void set_clock_scale(double us_per_step) { us_per_step_ = us_per_step > 0.0 ? us_per_step : 1.0; }
    double clock_scale() const { return us_per_step_; }
    Result run(std::uint64_t max_steps);
    void advance(std::uint64_t microseconds);
    std::uint64_t now() const { return now_; }
    std::vector<X86Interpreter::TraceItem> recent_traces() const { return interpreter_.recent_traces(); }
    std::int32_t sleep(Id id, std::uint64_t microseconds);
    Id mutex_create(bool recursive = false);
    std::int32_t mutex_delete(Id mutex);
    std::int32_t lock(Id thread, Id mutex, std::uint64_t timeout = 0);
    std::int32_t unlock(Id thread, Id mutex);
    std::int32_t try_lock(Id thread, Id mutex);
    unsigned mutex_depth(Id mutex) const;
    Id cond_create();
    std::int32_t cond_delete(Id cond);
    std::int32_t cond_wait(Id thread, Id cond, Id mutex, std::uint64_t timeout = 0);
    std::int32_t cond_signal(Id cond, bool broadcast = false);
    Id event_create(std::uint64_t initial = 0);
    std::int32_t event_delete(Id event);
    std::int32_t event_wait(Id thread, Id event, std::uint64_t mask, bool all,
                            std::uint64_t timeout = 0);
    std::int32_t event_set(Id event, std::uint64_t bits);
    std::int32_t event_clear(Id event, std::uint64_t bits);
    std::uint64_t event_bits(Id event) const;
    std::size_t event_waiters_count(Id event) const;

    // Kernel Semaphores
    Id sema_create(std::int32_t initial_count, std::int32_t max_count);
    std::int32_t sema_wait(Id thread, Id sema, std::int32_t need, std::uint64_t timeout = 0);
    std::int32_t sema_poll(Id sema, std::int32_t need);
    std::int32_t sema_signal(Id sema, std::int32_t count);
    std::int32_t sema_delete(Id sema);
    std::int32_t sema_count(Id sema) const;
    std::size_t sema_waiters(Id sema) const;

    // TLS (Thread-Local Storage)
    Id tls_create(std::uint64_t destructor = 0);
    std::int32_t tls_delete(Id key);
    std::int32_t tls_set(Id thread, Id key, std::uint64_t value);
    std::uint64_t tls_get(Id thread, Id key) const;
    bool tls_valid(Id key) const;
    void set_tls_destructor_invoker(std::function<void(Id thread, std::uint64_t destructor, std::uint64_t value)> invoker);

    void set_resume(Id id, std::function<void(CpuState&, std::int32_t)> hook);
    // Trampolines: the driver must explicitly arrange a guest CALL to function,
    // then call complete_initializer after its return trap. Never invoke on host.
    std::int32_t begin_initializer(Id thread, std::uint64_t key, Callback callback);
    std::int32_t complete_initializer(Id thread, std::uint64_t key, bool success);
    void register_atexit(Callback callback);
    const std::vector<Callback>& atexit_callbacks() const { return atexit_; }
    Callback initializer(Id thread) const;
private:
    enum class Wait { none, join, mutex, cond, reacquire, event, sleep, init, sema };
    struct Thread {
        CpuState cpu;
        State state = State::created;
        Wait wait = Wait::none;
        Id object = 0, mutex = 0;
        std::uint64_t mask = 0, deadline = 0, result_value = 0;
        std::int32_t need = 0;
        bool all = false, timed = false;
        std::int32_t result = ORBIS_OK;
        Callback callback{};
        std::function<void(CpuState&, std::int32_t)> on_resume;
        std::uint64_t stack_bottom = 0, stack_top = 0;
        unsigned saved_mutex_depth = 0;
        // Thread-local storage
        std::unordered_map<std::uint32_t, std::uint64_t> tls_values;
    };
    struct Mutex { Id owner = 0; unsigned depth = 0; bool recursive = false; std::deque<Id> waiters; };
    struct Cond { std::deque<Id> waiters; };
    struct Event { std::uint64_t bits = 0; std::deque<Id> waiters; };
    struct Init { Id owner = 0; bool complete = false; std::deque<Id> waiters; };
    struct Sema {
        std::int32_t count = 0;
        std::int32_t max_count = 0;
        std::deque<Id> waiters; // each waiter has 'need' in Thread::need
    };
    Thread* find(Id id);
    const Thread* find(Id id) const;
    void ready(Id id, std::int32_t result);
    void block(Id id, Wait kind, Id object, std::uint64_t timeout, std::int32_t need = 0);
    void acquire_next(Id mutex);
    void reacquire(Id thread, Id mutex, std::int32_t result);
    GuestMemory& mem_;
    X86Interpreter interpreter_;
    std::uint64_t quantum_, now_ = 0;
    Id next_ = 1;
    std::map<Id, Thread> threads_;
    std::map<Id, Mutex> mutexes_;
    std::map<Id, Cond> conds_;
    std::map<Id, Event> events_;
    std::map<std::uint64_t, Init> inits_;
    std::map<Id, Sema> semas_;
    std::unordered_set<Id> tls_keys_;
    std::map<std::uint32_t, std::uint64_t> tls_destructors_; // key -> destructor function
    std::function<void(Id, std::uint64_t, std::uint64_t)> tls_destructor_invoker_{};
    std::vector<Callback> atexit_;
    std::deque<Id> ready_;
    Trap trap_;
    FaultHandler fault_handler_{};
    PauseChecker pause_checker_{};
    bool running_ = false;
    ClockMode clock_mode_{ClockMode::Steps};
    double us_per_step_{1.0};
    double sub_us_accumulator_{0.0};
};
} // namespace bb
