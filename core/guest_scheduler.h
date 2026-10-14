#pragma once
#include "x86_interp.h"
#include "kernel_threads.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

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
    explicit GuestScheduler(GuestMemory& memory, std::uint64_t quantum = 256);
    void add_trap_range(std::uint64_t base, std::uint64_t size);
    void set_trap(Trap trap) { trap_ = std::move(trap); }
    // Caller maps disjoint stacks and places a return trap at [stack_top-8].
    Id create(std::uint64_t entry, std::uint64_t stack_bottom, std::uint64_t stack_top,
              std::uint64_t return_trap, std::uint64_t argument = 0);
    std::int32_t start(Id id);
    std::int32_t exit(Id id, std::uint64_t value);
    std::int32_t join(Id caller, Id target, std::uint64_t* value = nullptr,
                      std::uint64_t timeout = 0); // zero means infinite; result on resume()
    std::int32_t resume(Id id) const;
    std::int32_t cancel(Id id);
    State state(Id id) const;
    const CpuState& cpu(Id id) const;
    std::uint64_t value(Id id) const;
    Result run(std::uint64_t max_steps);
    void advance(std::uint64_t microseconds);
    std::uint64_t now() const { return now_; }
    std::int32_t sleep(Id id, std::uint64_t microseconds);
    Id mutex_create(bool recursive = false);
    std::int32_t lock(Id thread, Id mutex, std::uint64_t timeout = 0);
    std::int32_t unlock(Id thread, Id mutex);
    Id cond_create();
    std::int32_t cond_wait(Id thread, Id cond, Id mutex, std::uint64_t timeout = 0);
    std::int32_t cond_signal(Id cond, bool broadcast = false);
    Id event_create(std::uint64_t initial = 0);
    std::int32_t event_wait(Id thread, Id event, std::uint64_t mask, bool all,
                            std::uint64_t timeout = 0);
    std::int32_t event_set(Id event, std::uint64_t bits);
    std::int32_t event_clear(Id event, std::uint64_t bits);
    // Trampolines: the driver must explicitly arrange a guest CALL to function,
    // then call complete_initializer after its return trap. Never invoke on host.
    std::int32_t begin_initializer(Id thread, std::uint64_t key, Callback callback);
    std::int32_t complete_initializer(Id thread, std::uint64_t key, bool success);
    void register_atexit(Callback callback);
    const std::vector<Callback>& atexit_callbacks() const { return atexit_; }
    Callback initializer(Id thread) const;
private:
    enum class Wait { none, join, mutex, cond, reacquire, event, sleep, init };
    struct Thread {
        CpuState cpu;
        State state = State::created;
        Wait wait = Wait::none;
        Id object = 0, mutex = 0;
        std::uint64_t mask = 0, deadline = 0, result_value = 0;
        bool all = false, timed = false;
        std::int32_t result = ORBIS_OK;
        Callback callback{};
        std::uint64_t stack_bottom = 0, stack_top = 0;
    };
    struct Mutex { Id owner = 0; unsigned depth = 0; bool recursive = false; std::deque<Id> waiters; };
    struct Cond { std::deque<Id> waiters; };
    struct Event { std::uint64_t bits = 0; std::deque<Id> waiters; };
    struct Init { Id owner = 0; bool complete = false; std::deque<Id> waiters; };
    Thread* find(Id id);
    const Thread* find(Id id) const;
    void ready(Id id, std::int32_t result);
    void block(Id id, Wait kind, Id object, std::uint64_t timeout);
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
    std::vector<Callback> atexit_;
    std::deque<Id> ready_;
    Trap trap_;
    bool running_ = false;
};
} // namespace bb
