#include "orbis_runtime.h"
#include "kernel_threads.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

namespace bb {
namespace {
constexpr std::uint64_t page = 16384, pool = 5056ULL << 20, logical_limit = 3ULL << 30;
constexpr std::uint64_t invalid = 0x80020016, no_memory = 0x8002000c, no_space = 0x80020023;
std::uint64_t align(std::uint64_t value, std::uint64_t unit) { return (value + unit - 1) & ~(unit - 1); }
bool valid_alignment(std::uint64_t a) { return a >= page && a <= pool && !(a & (a - 1)); }
// SHA-1 de FIPS 180-4, somente 8 bytes do resultado são usados pelo NID.
std::array<std::uint8_t, 20> sha1(const std::string& text) {
    std::vector<std::uint8_t> data(text.begin(), text.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    std::uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    auto rol = [](std::uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); };
    for (std::size_t p = 0; p < data.size(); p += 64) {
        std::uint32_t w[80] = {};
        for (int i = 0; i < 16; ++i) w[i] = (std::uint32_t(data[p + i * 4]) << 24) | (std::uint32_t(data[p + i * 4 + 1]) << 16) | (std::uint32_t(data[p + i * 4 + 2]) << 8) | data[p + i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        auto a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
        for (int i=0; i<80; ++i) {
            std::uint32_t f, k;
            if (i<20) { f=(b&c)|(~b&d); k=0x5a827999; }
            else if (i<40) { f=b^c^d; k=0x6ed9eba1; }
            else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8f1bbcdc; }
            else { f=b^c^d; k=0xca62c1d6; }
            const auto t=rol(a,5)+f+e+k+w[i]; e=d; d=c; c=rol(b,30); b=a; a=t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    std::array<std::uint8_t,20> out{};
    for (int i=0;i<20;++i) out[i]=static_cast<std::uint8_t>(h[i/4] >> (24-8*(i%4)));
    return out;
}
template<typename T> T get(GuestMemory& m, std::uint64_t a) {
    T v{}; if (m.load(a,&v,sizeof v)!=MemoryError::ok) throw std::runtime_error("leitura guest inválida"); return v;
}
template<typename T> void put(GuestMemory& m, std::uint64_t a, const T& v) {
    if (m.store(a,&v,sizeof v)!=MemoryError::ok) throw std::runtime_error("escrita guest inválida");
}
}
std::string orbis_nid(const std::string& symbol) {
    static const char salt[] = "\x51\x8d\x64\xa6\x35\xde\xd8\xc1\xe6\xb0\x39\xb1\xc3\xe5\x52\x30";
    const auto digest=sha1(symbol+std::string(salt,16));
    static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string result;
    unsigned buffer=0, count=0;
    for (int i=7;i>=0;--i) {
        buffer=(buffer<<8)|digest[i]; count+=8;
        while (count>=6) { count-=6; result+=alphabet[(buffer>>count)&63]; }
    }
    if (count) result+=alphabet[(buffer<<(6-count))&63];
    return result;
}
OrbisRuntime::OrbisRuntime(GuestMemory& memory)
    : mem_(memory), user_mgr_(), pad_mgr_(nullptr, &user_mgr_) {
    static const char* const symbols[] = {
#include "orbis_import_names.h"
        "scePthreadMutexattrInit", "scePthreadMutexattrSettype", "scePthreadMutexattrDestroy",
        "scePthreadMutexInit", "scePthreadMutexLock", "scePthreadMutexUnlock", "scePthreadMutexDestroy",
        "scePthreadMutexTrylock", "sceKernelMmap", "sceKernelMunmap", "sceKernelVirtualQuery",
        "__cxa_guard_abort",
        // libScePad
        "scePadOpen", "scePadClose", "scePadReadState", "scePadSetVibration", "scePadSetLightBar", "scePadInit",
        // libSceUserService
        "sceUserServiceInitialize", "sceUserServiceGetInitialUser", "sceUserServiceGetLoginIdList",
        "sceUserServiceGetLoginUserIdList", "sceUserServiceGetUserName", "sceUserServiceTerminate",
        // libSceSaveData
        "sceSaveDataInitialize", "sceSaveDataInitialize2", "sceSaveDataInitialize3",
        "sceSaveDataMount", "sceSaveDataMount2", "sceSaveDataUmount", "sceSaveDataDelete",
        "sceSaveDataDirNameSearch", "sceSaveDataTerminate",
        // libSceAudioOut
        "sceAudioOutOpen", "sceAudioOutClose", "sceAudioOutOutput", "sceAudioOutSetVolume",
        "sceAudioOutGetPortInfo", "sceAudioOutGetPortState", "sceAudioOutInit",
        // Heap & runtime
        "_sceKernelRtldSetApplicationHeapAPI",
        // Math double
        "sin", "cos", "tan", "atan2", "pow", "exp", "log", "sqrt", "floor", "ceil", "fmod", "round", "fabs",
        // Math float
        "sinf", "cosf", "tanf", "atan2f", "powf", "expf", "logf", "sqrtf", "floorf", "ceilf", "fmodf", "roundf", "fabsf",
        // Conversions & stdlib
        "strtol", "strtoul", "strtoll", "strtoull", "strtod", "strtof", "qsort", "bsearch",
        // Formatting
        "snprintf", "vsnprintf"
    };
    for (const char* s : symbols) names_.emplace(orbis_nid(s), s);
}
std::string OrbisRuntime::resolve(const std::string& import) const {
    const auto key=import.substr(0,import.find('#'));
    auto it=names_.find(key);
    return it==names_.end() ? import : it->second;
}
std::uint64_t OrbisRuntime::arg(const CpuState& c, unsigned i) const {
    static constexpr unsigned regs[]={7,6,2,1,8,9};
    if (i<6) return c.gpr[regs[i]];
    if (i>16 || c.gpr[4]>UINT64_MAX-8*(i-5)) throw std::runtime_error("pilha SysV inválida");
    return get<std::uint64_t>(mem_,c.gpr[4]+8*(i-5));
}
std::uint64_t OrbisRuntime::invoke(const std::string& import, const CpuState& cpu) {
    CpuState copy = cpu;
    return invoke(import, copy);
}
std::uint64_t OrbisRuntime::invoke(const std::string& import, CpuState& cpu) {
    const auto name=resolve(import);
    if (scheduler_ && (name=="scePthreadMutexLock" || name=="pthread_mutex_lock" ||
        name=="scePthreadCondWait" || name=="pthread_cond_wait" ||
        name=="scePthreadCondTimedwait" || name=="pthread_cond_timedwait" ||
        name=="sceKernelWaitEventFlag" || name=="scePthreadOnce" ||
        name=="__cxa_guard_acquire" || name=="__cxa_finalize"))
        throw std::runtime_error("guest scheduler trap adapter unavailable for " + name);
    return handle(name,cpu);
}
OrbisRuntime::InvocationResult OrbisRuntime::invoke_trap(const std::string& import, CpuState& cpu,
                                                           GuestScheduler::Id tid, std::uint64_t return_trap) {
    if (!scheduler_) {
        return InvocationResult{InvocationResult::Action::returned, handle(resolve(import), cpu), 0, 0};
    }
    const auto n = resolve(import);
    auto a = [&](unsigned i) { return arg(cpu, i); };
    auto result = [&](std::uint64_t value) {
        return InvocationResult{InvocationResult::Action::returned, value, 0, 0};
    };

    if (n == "scePthreadCreate") {
        if (!a(0) || !a(2) || !return_trap || thread_stack_next_ > UINT64_MAX - 0x10000) return result(invalid);
        const auto bottom = thread_stack_next_;
        if (mem_.map(bottom, 0x10000, read | write) != MemoryError::ok) return result(no_memory);
        auto id = scheduler_->create(a(2), bottom, bottom + 0x10000, return_trap, a(3));
        if (!id) { mem_.unmap(bottom); return result(invalid); }
        if (mem_.store(a(0), &id, sizeof(id)) != MemoryError::ok) { mem_.unmap(bottom); return result(invalid); }
        thread_stack_next_ += 0x10000;
        return result(static_cast<std::uint32_t>(scheduler_->start(id)));
    }

    if (n == "scePthreadExit") {
        scheduler_->exit(tid, a(0));
        return {InvocationResult::Action::exited, 0, 0, 0};
    }

    if (n == "scePthreadJoin") {
        if (a(1)) {
            std::uint64_t probe = 0;
            if (mem_.load(a(1), &probe, 8) != MemoryError::ok || mem_.store(a(1), &probe, 8) != MemoryError::ok)
                return result(invalid);
        }
        const auto target = static_cast<GuestScheduler::Id>(a(0));
        auto status = scheduler_->join(tid, target);
        if (status) return result(static_cast<std::uint32_t>(status));
        if (scheduler_->state(tid) == GuestScheduler::State::waiting) {
            std::uint64_t ret = 0;
            if (mem_.load(cpu.gpr[4], &ret, 8) != MemoryError::ok) return result(invalid);
            cpu.gpr[4] += 8;
            cpu.rip = ret;
            cpu.gpr[0] = 0;
            auto out_val = a(1);
            scheduler_->set_resume(tid, [this, out_val, target](CpuState& state, std::int32_t res) {
                state.gpr[0] = static_cast<std::uint32_t>(res);
                if (res == ORBIS_OK && out_val) {
                    std::uint64_t val = scheduler_->value(target);
                    mem_.store(out_val, &val, 8);
                }
            });
            return {InvocationResult::Action::blocked, 0, 0, 0};
        }
        if (a(1)) {
            std::uint64_t val = scheduler_->value(target);
            put(mem_, a(1), val);
        }
        return result(0);
    }

    if (n == "scePthreadMutexInit" || n == "pthread_mutex_init") {
        if (!a(0)) return result(invalid);
        bool recursive = false;
        if (a(1)) {
            auto attr = get<std::uint64_t>(mem_, a(1));
            if (!attrs_.count(attr)) return result(invalid);
            recursive = (attrs_.at(attr) == 2);
        }
        const auto id = scheduler_->mutex_create(recursive);
        const auto handle = next_handle_++;
        guest_mutexes_[handle] = id;
        put(mem_, a(0), handle);
        return result(0);
    }

    if (n == "scePthreadMutexLock" || n == "pthread_mutex_lock" ||
        n == "scePthreadMutexUnlock" || n == "pthread_mutex_unlock" ||
        n == "scePthreadMutexTrylock" || n == "scePthreadMutexDestroy" || n == "pthread_mutex_destroy") {
        if (!a(0)) return result(invalid);
        auto handle = get<std::uint64_t>(mem_, a(0));
        if (!handle && n != "scePthreadMutexUnlock" && n != "pthread_mutex_unlock" &&
            n != "scePthreadMutexDestroy" && n != "pthread_mutex_destroy") {
            handle = next_handle_++;
            guest_mutexes_[handle] = scheduler_->mutex_create(false);
            put(mem_, a(0), handle);
        }
        if (!guest_mutexes_.count(handle)) return result(invalid);
        const auto id = guest_mutexes_.at(handle);
        if (n == "scePthreadMutexDestroy" || n == "pthread_mutex_destroy") {
            guest_mutexes_.erase(handle);
            put<std::uint64_t>(mem_, a(0), 0);
            return result(0);
        }
        if (n == "scePthreadMutexUnlock" || n == "pthread_mutex_unlock") {
            return result(static_cast<std::uint32_t>(scheduler_->unlock(tid, id)));
        }
        const auto status = (n == "scePthreadMutexTrylock") ? scheduler_->try_lock(tid, id) : scheduler_->lock(tid, id);
        if (status) return result(static_cast<std::uint32_t>(status));
        if (scheduler_->state(tid) == GuestScheduler::State::waiting) {
            std::uint64_t ret = 0;
            if (mem_.load(cpu.gpr[4], &ret, 8) != MemoryError::ok) return result(invalid);
            cpu.gpr[4] += 8;
            cpu.rip = ret;
            cpu.gpr[0] = 0;
            scheduler_->set_resume(tid, [](CpuState& state, std::int32_t res) {
                state.gpr[0] = static_cast<std::uint32_t>(res);
            });
            return {InvocationResult::Action::blocked, 0, 0, 0};
        }
        return result(0);
    }

    if (n == "scePthreadCondInit" || n == "pthread_cond_init") {
        if (!a(0)) return result(invalid);
        const auto handle = next_handle_++;
        guest_conds_[handle] = scheduler_->cond_create();
        put(mem_, a(0), handle);
        return result(0);
    }

    if (n == "scePthreadCondDestroy" || n == "pthread_cond_destroy") {
        if (!a(0)) return result(invalid);
        auto handle = get<std::uint64_t>(mem_, a(0));
        if (!guest_conds_.erase(handle)) return result(invalid);
        put<std::uint64_t>(mem_, a(0), 0);
        return result(0);
    }

    if (n == "scePthreadCondSignal" || n == "pthread_cond_signal" ||
        n == "scePthreadCondBroadcast" || n == "pthread_cond_broadcast") {
        if (!a(0)) return result(invalid);
        auto handle = get<std::uint64_t>(mem_, a(0));
        if (!guest_conds_.count(handle)) return result(invalid);
        bool broadcast = (n.find("Broadcast") != std::string::npos || n == "pthread_cond_broadcast");
        return result(static_cast<std::uint32_t>(scheduler_->cond_signal(guest_conds_.at(handle), broadcast)));
    }

    if (n == "scePthreadCondWait" || n == "pthread_cond_wait" ||
        n == "scePthreadCondTimedwait" || n == "pthread_cond_timedwait") {
        if (!a(0) || !a(1)) return result(invalid);
        auto ch = get<std::uint64_t>(mem_, a(0));
        auto mh = get<std::uint64_t>(mem_, a(1));
        if (!guest_conds_.count(ch) || !guest_mutexes_.count(mh)) return result(invalid);
        std::uint64_t timeout = 0;
        if (n.find("Timedwait") != std::string::npos || n == "pthread_cond_timedwait") {
            if (!a(2)) return result(invalid);
            timeout = get<std::uint64_t>(mem_, a(2));
        }
        auto status = scheduler_->cond_wait(tid, guest_conds_.at(ch), guest_mutexes_.at(mh), timeout);
        if (status) return result(static_cast<std::uint32_t>(status));
        if (scheduler_->state(tid) == GuestScheduler::State::waiting) {
            std::uint64_t ret = 0;
            if (mem_.load(cpu.gpr[4], &ret, 8) != MemoryError::ok) return result(invalid);
            cpu.gpr[4] += 8;
            cpu.rip = ret;
            cpu.gpr[0] = 0;
            scheduler_->set_resume(tid, [](CpuState& state, std::int32_t res) {
                state.gpr[0] = static_cast<std::uint32_t>(res);
            });
            return {InvocationResult::Action::blocked, 0, 0, 0};
        }
        return result(0);
    }

    if (n == "sceKernelCreateEventFlag") {
        if (!a(0) || a(2) > 3) return result(invalid);
        const auto handle = next_handle_++;
        guest_events_[handle] = scheduler_->event_create(a(3));
        put<std::uint32_t>(mem_, a(0), static_cast<std::uint32_t>(handle));
        return result(0);
    }

    if (n == "sceKernelDeleteEventFlag") {
        if (!guest_events_.erase(a(0))) return result(invalid);
        return result(0);
    }

    if (n == "sceKernelSetEventFlag") {
        if (!guest_events_.count(a(0))) return result(invalid);
        return result(static_cast<std::uint32_t>(scheduler_->event_set(guest_events_.at(a(0)), a(1))));
    }

    if (n == "sceKernelClearEventFlag") {
        if (!guest_events_.count(a(0))) return result(invalid);
        return result(static_cast<std::uint32_t>(scheduler_->event_clear(guest_events_.at(a(0)), a(1))));
    }

    if (n == "sceKernelWaitEventFlag" || n == "sceKernelPollEventFlag") {
        if (!guest_events_.count(a(0))) return result(invalid);
        const auto eid = guest_events_.at(a(0));
        const auto mask = a(1), mode = a(2);
        if (!mask || (mode & ~0x33)) return result(invalid);
        const bool poll = (n == "sceKernelPollEventFlag");
        const auto bits = scheduler_->event_bits(eid) & mask;
        const bool match = (mode & 1) ? (bits == mask) : (bits != 0);
        if (match) {
            if (!a(3)) return result(invalid);
            put(mem_, a(3), scheduler_->event_bits(eid));
            if (mode & 0x10) scheduler_->event_clear(eid, bits);
            if (mode & 0x20) scheduler_->event_clear(eid, ~0ULL);
            return result(0);
        }
        if (poll) return result(0x80020010); // EBUSY
        if (!a(3)) return result(invalid);
        std::uint64_t timeout = 0;
        if (a(4)) timeout = get<std::uint64_t>(mem_, a(4));
        auto status = scheduler_->event_wait(tid, eid, mask, (mode & 1) != 0, timeout);
        if (status) return result(static_cast<std::uint32_t>(status));
        if (scheduler_->state(tid) == GuestScheduler::State::waiting) {
            std::uint64_t ret = 0;
            if (mem_.load(cpu.gpr[4], &ret, 8) != MemoryError::ok) return result(invalid);
            cpu.gpr[4] += 8;
            cpu.rip = ret;
            cpu.gpr[0] = 0;
            auto out_ptr = a(3);
            scheduler_->set_resume(tid, [this, out_ptr, eid, mode, mask](CpuState& state, std::int32_t res) {
                state.gpr[0] = static_cast<std::uint32_t>(res);
                if (res == ORBIS_OK && out_ptr) {
                    auto b = scheduler_->event_bits(eid);
                    mem_.store(out_ptr, &b, 8);
                    if (mode & 0x10) scheduler_->event_clear(eid, b & mask);
                    if (mode & 0x20) scheduler_->event_clear(eid, ~0ULL);
                }
            });
            return {InvocationResult::Action::blocked, 0, 0, 0};
        }
        return result(0);
    }

    if (n == "scePthreadOnce") {
        if (!a(0) || !a(1) || !return_trap) return result(invalid);
        std::uint32_t done = 0;
        if (mem_.load(a(0), &done, 4) != MemoryError::ok) return result(invalid);
        if (done) return result(0);
        auto status = scheduler_->begin_initializer(tid, a(0), {a(1), 0});
        if (status) return result(static_cast<std::uint32_t>(status));
        if (scheduler_->state(tid) == GuestScheduler::State::waiting) {
            std::uint64_t ret = 0;
            if (mem_.load(cpu.gpr[4], &ret, 8) != MemoryError::ok) return result(invalid);
            cpu.gpr[4] += 8;
            cpu.rip = ret;
            cpu.gpr[0] = 0;
            scheduler_->set_resume(tid, [](CpuState& state, std::int32_t res) {
                state.gpr[0] = static_cast<std::uint32_t>(res);
            });
            return {InvocationResult::Action::blocked, 0, 0, 0};
        }
        std::uint64_t ret = 0;
        if (mem_.load(cpu.gpr[4], &ret, 8) != MemoryError::ok) return result(invalid);
        callbacks_[tid] = {a(0), ret, return_trap};
        put(mem_, cpu.gpr[4], return_trap);
        cpu.rip = a(1);
        return {InvocationResult::Action::callback, 0, a(1), 0};
    }

    if (n == "__cxa_guard_acquire") {
        if (!a(0)) return result(invalid);
        std::uint8_t done = 0;
        if (mem_.load(a(0), &done, 1) != MemoryError::ok) return result(invalid);
        if (done) return result(0);
        auto status = scheduler_->begin_initializer(tid, a(0), {1, 0});
        if (status) return result(static_cast<std::uint32_t>(status));
        if (scheduler_->state(tid) == GuestScheduler::State::waiting) {
            std::uint64_t ret = 0;
            if (mem_.load(cpu.gpr[4], &ret, 8) != MemoryError::ok) return result(invalid);
            cpu.gpr[4] += 8;
            cpu.rip = ret;
            cpu.gpr[0] = 0;
            auto guard_ptr = a(0);
            scheduler_->set_resume(tid, [this, guard_ptr](CpuState& state, std::int32_t res) {
                (void)res;
                std::uint8_t d = 0;
                mem_.load(guard_ptr, &d, 1);
                state.gpr[0] = d ? 0 : 1;
            });
            return {InvocationResult::Action::blocked, 0, 0, 0};
        }
        return result(1);
    }

    if (n == "__cxa_guard_release" || n == "__cxa_guard_abort") {
        if (!a(0)) return result(invalid);
        if (n == "__cxa_guard_release") {
            std::uint8_t one = 1;
            mem_.store(a(0), &one, 1);
            scheduler_->complete_initializer(tid, a(0), true);
        } else {
            scheduler_->complete_initializer(tid, a(0), false);
        }
        return result(0);
    }

    return result(handle(n, cpu));
}
OrbisRuntime::InvocationResult OrbisRuntime::complete_callback(CpuState& cpu, GuestScheduler::Id tid) {
    auto it = callbacks_.find(tid);
    if (it == callbacks_.end()) throw std::runtime_error("unexpected guest callback return");
    const auto pending = it->second;
    const auto status = scheduler_->complete_initializer(tid, pending.key, true);
    std::uint32_t one = 1;
    mem_.store(pending.key, &one, 4);
    cpu.rip = pending.return_address;
    cpu.gpr[0] = static_cast<std::uint32_t>(status);
    callbacks_.erase(it);
    return {InvocationResult::Action::returned, static_cast<std::uint32_t>(status), 0, 0};
}
std::uint64_t OrbisRuntime::handle(const std::string& n, CpuState& c) {
    auto a=[&](unsigned i){return arg(c,i);};
    if (n=="_init_env" || n=="_sceKernelRtldSetApplicationHeapAPI") return 0;
    if (n=="sceKernelGetProcParam") return procparam_;

    auto read_guest_string = [&](std::uint64_t addr, std::size_t max_len = 4096) -> std::string {
        std::string s;
        for (std::size_t i = 0; i < max_len; ++i) {
            char ch = 0;
            if (mem_.load(addr + i, &ch, 1) != MemoryError::ok || ch == '\0') break;
            s.push_back(ch);
        }
        return s;
    };
    auto get_double = [&](unsigned xmm_idx, unsigned gpr_idx) -> double {
        if (c.xmm[xmm_idx][0] != 0) {
            double val = 0;
            std::memcpy(&val, &c.xmm[xmm_idx][0], sizeof(val));
            return val;
        }
        double val = 0;
        std::uint64_t gpr_val = a(gpr_idx);
        std::memcpy(&val, &gpr_val, sizeof(val));
        return val;
    };
    auto put_double = [&](double val) -> std::uint64_t {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &val, sizeof(bits));
        c.xmm[0][0] = bits;
        return bits;
    };
    auto get_float = [&](unsigned xmm_idx, unsigned gpr_idx) -> float {
        if ((c.xmm[xmm_idx][0] & 0xFFFFFFFFULL) != 0) {
            std::uint32_t val = static_cast<std::uint32_t>(c.xmm[xmm_idx][0]);
            float f = 0;
            std::memcpy(&f, &val, sizeof(f));
            return f;
        }
        std::uint32_t val = static_cast<std::uint32_t>(a(gpr_idx));
        float f = 0;
        std::memcpy(&f, &val, sizeof(f));
        return f;
    };
    auto put_float = [&](float val) -> std::uint64_t {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &val, sizeof(bits));
        c.xmm[0][0] = bits;
        return static_cast<std::uint64_t>(bits);
    };

    // libScePad HLE
    if (n == "scePadOpen") {
        return static_cast<std::uint32_t>(pad_mgr_.scePadOpen(
            mem_, static_cast<std::uint32_t>(a(0)), static_cast<std::int32_t>(a(1)),
            static_cast<std::int32_t>(a(2)), a(3)));
    }
    if (n == "scePadClose") {
        return static_cast<std::uint32_t>(pad_mgr_.scePadClose(static_cast<std::int32_t>(a(0))));
    }
    if (n == "scePadReadState") {
        return static_cast<std::uint32_t>(pad_mgr_.scePadReadState(
            mem_, static_cast<std::int32_t>(a(0)), a(1)));
    }
    if (n == "scePadSetVibration") {
        return static_cast<std::uint32_t>(pad_mgr_.scePadSetVibration(
            mem_, static_cast<std::int32_t>(a(0)), a(1)));
    }
    if (n == "scePadSetLightBar") {
        return static_cast<std::uint32_t>(pad_mgr_.scePadSetLightBar(
            mem_, static_cast<std::int32_t>(a(0)), a(1)));
    }
    if (n == "scePadInit") return 0;

    // libSceUserService HLE
    if (n == "sceUserServiceInitialize") {
        return static_cast<std::uint32_t>(user_mgr_.sceUserServiceInitialize(mem_, a(0)));
    }
    if (n == "sceUserServiceGetInitialUser") {
        return static_cast<std::uint32_t>(user_mgr_.sceUserServiceGetInitialUser(mem_, a(0)));
    }
    if (n == "sceUserServiceGetLoginIdList" || n == "sceUserServiceGetLoginUserIdList") {
        return static_cast<std::uint32_t>(user_mgr_.sceUserServiceGetLoginIdList(mem_, a(0)));
    }
    if (n == "sceUserServiceGetUserName") {
        return static_cast<std::uint32_t>(user_mgr_.sceUserServiceGetUserName(
            mem_, static_cast<SceUserServiceUserId>(a(0)), a(1), static_cast<std::size_t>(a(2))));
    }
    if (n == "sceUserServiceTerminate") {
        return static_cast<std::uint32_t>(user_mgr_.sceUserServiceTerminate());
    }

    // libSceSaveData HLE
    if (n == "sceSaveDataInitialize" || n == "sceSaveDataInitialize2" || n == "sceSaveDataInitialize3") {
        std::size_t quota = SCE_SAVEDATA_DEFAULT_QUOTA;
        if (a(0)) {
            SceSaveDataInitParams p{};
            if (mem_.load(a(0), &p, sizeof(p)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            if (p.max_quota_per_slot > 0) quota = static_cast<std::size_t>(p.max_quota_per_slot);
        }
        std::string base = save_mgr_.get_base_directory();
        if (base.empty()) base = "savedata";
        bool ok = save_mgr_.initialize(base, quota, save_mgr_.get_vfs());
        return static_cast<std::uint32_t>(ok ? SCE_SAVEDATA_OK : SCE_SAVEDATA_ERROR_INTERNAL);
    }
    if (n == "sceSaveDataMount" || n == "sceSaveDataMount2") {
        if (!a(0) || !a(1)) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        const bool is_mount2 = (n == "sceSaveDataMount2");
        int32_t user = 0;
        uint64_t title_ptr = 0;
        uint64_t dir_ptr = 0;
        uint64_t blocks = 0;
        uint32_t mode = 0;
        if (is_mount2) {
            SceSaveDataMount2 m2{};
            if (mem_.load(a(0), &m2, sizeof(m2)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            user = m2.user;
            dir_ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m2.dir));
            blocks = m2.blocks;
            mode = m2.mode;
        } else {
            SceSaveDataMount m1{};
            if (mem_.load(a(0), &m1, sizeof(m1)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            user = m1.user;
            title_ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m1.title));
            dir_ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m1.dir));
            blocks = m1.blocks;
            mode = m1.mode;
        }
        SceSaveDataTitleId title_obj{};
        const char* title_str = nullptr;
        if (title_ptr) {
            if (mem_.load(title_ptr, &title_obj, sizeof(title_obj)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            title_str = title_obj.data;
        }
        SceSaveDataDirName dir_obj{};
        const char* dir_str = nullptr;
        if (dir_ptr) {
            if (mem_.load(dir_ptr, &dir_obj, sizeof(dir_obj)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            dir_str = dir_obj.data;
        }
        std::string mount_pt;
        uint32_t status = 0;
        int32_t res = save_mgr_.mount(user, title_str, dir_str, mode, blocks, mount_pt, status);
        if (res == SCE_SAVEDATA_OK) {
            SceSaveDataMountResult result{};
            std::strncpy(result.point.data, mount_pt.c_str(), sizeof(result.point.data) - 1);
            result.status = status;
            result.required_blocks = blocks;
            if (mem_.store(a(1), &result, sizeof(result)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        }
        return static_cast<std::uint32_t>(res);
    }
    if (n == "sceSaveDataUmount") {
        if (!a(0)) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        SceSaveDataMountPoint pt{};
        if (mem_.load(a(0), &pt, sizeof(pt)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        bool commit = (a(1) & SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) != 0;
        return static_cast<std::uint32_t>(save_mgr_.umount(pt.data, commit));
    }
    if (n == "sceSaveDataDelete") {
        if (!a(0)) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        SceSaveDataDelete d{};
        if (mem_.load(a(0), &d, sizeof(d)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        uint64_t title_ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d.title));
        uint64_t dir_ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d.dir));
        if (!dir_ptr) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        SceSaveDataTitleId title_obj{};
        const char* title_str = nullptr;
        if (title_ptr) {
            if (mem_.load(title_ptr, &title_obj, sizeof(title_obj)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            title_str = title_obj.data;
        }
        SceSaveDataDirName dir_obj{};
        if (mem_.load(dir_ptr, &dir_obj, sizeof(dir_obj)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        return static_cast<std::uint32_t>(save_mgr_.delete_save(d.user, title_str, dir_obj.data));
    }
    if (n == "sceSaveDataDirNameSearch") {
        if (!a(0) || !a(1)) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        SceSaveDataSearchCond cond{};
        if (mem_.load(a(0), &cond, sizeof(cond)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        SceSaveDataSearchResult result{};
        if (mem_.load(a(1), &result, sizeof(result)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        uint64_t title_ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cond.title));
        uint64_t dir_ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cond.dir));
        SceSaveDataTitleId title_obj{};
        const char* title_str = nullptr;
        if (title_ptr) {
            if (mem_.load(title_ptr, &title_obj, sizeof(title_obj)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            title_str = title_obj.data;
        }
        SceSaveDataDirName dir_obj{};
        const char* dir_pattern = nullptr;
        if (dir_ptr) {
            if (mem_.load(dir_ptr, &dir_obj, sizeof(dir_obj)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            dir_pattern = dir_obj.data;
        }
        std::vector<SaveSearchEntry> entries;
        int32_t res = save_mgr_.dir_name_search(cond.user, title_str, dir_pattern, cond.key, cond.order, entries);
        if (res != SCE_SAVEDATA_OK) return static_cast<std::uint32_t>(res);

        result.hits = static_cast<uint32_t>(entries.size());
        uint32_t set = static_cast<uint32_t>(std::min<std::size_t>(entries.size(), result.names_capacity));
        result.set_count = set;
        uint64_t names_addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result.names));
        uint64_t params_addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result.params));
        uint64_t infos_addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result.infos));
        for (uint32_t i = 0; i < set; ++i) {
            if (names_addr) {
                SceSaveDataDirName name{};
                std::strncpy(name.data, entries[i].dir_name.c_str(), sizeof(name.data) - 1);
                mem_.store(names_addr + i * sizeof(SceSaveDataDirName), &name, sizeof(name));
            }
            if (params_addr) {
                mem_.store(params_addr + i * sizeof(SceSaveDataParam), &entries[i].param, sizeof(SceSaveDataParam));
            }
            if (infos_addr) {
                SceSaveDataSearchInfo info{};
                info.blocks = entries[i].blocks;
                info.free_blocks = entries[i].free_blocks;
                mem_.store(infos_addr + i * sizeof(SceSaveDataSearchInfo), &info, sizeof(info));
            }
        }
        mem_.store(a(1), &result, sizeof(result));
        return SCE_SAVEDATA_OK;
    }
    if (n == "sceSaveDataTerminate") {
        save_mgr_.terminate();
        return 0;
    }

    // libSceAudioOut HLE
    if (n == "sceAudioOutOpen") {
        return static_cast<std::uint32_t>(audio_mgr_.open_port(
            static_cast<std::int32_t>(a(0)), static_cast<std::int32_t>(a(1)), static_cast<std::int32_t>(a(2)),
            static_cast<std::uint32_t>(a(3)), static_cast<std::uint32_t>(a(4)), static_cast<std::uint32_t>(a(5))));
    }
    if (n == "sceAudioOutClose") {
        return static_cast<std::uint32_t>(audio_mgr_.close_port(static_cast<std::int32_t>(a(0))));
    }
    if (n == "sceAudioOutOutput") {
        return static_cast<std::uint32_t>(audio_mgr_.output_hle(static_cast<std::int32_t>(a(0)), mem_, a(1)));
    }
    if (n == "sceAudioOutSetVolume") {
        if (!a(2)) return static_cast<std::uint32_t>(SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
        std::int32_t vols[2] = {0, 0};
        if (mem_.load(a(2), vols, sizeof(vols)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
        return static_cast<std::uint32_t>(audio_mgr_.set_volume(static_cast<std::int32_t>(a(0)), static_cast<std::int32_t>(a(1)), vols));
    }
    if (n == "sceAudioOutGetPortInfo" || n == "sceAudioOutGetPortState") {
        if (!a(1)) return static_cast<std::uint32_t>(SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
        SceAudioOutPortInfo info{};
        int32_t res = audio_mgr_.get_port_info(static_cast<std::int32_t>(a(0)), &info);
        if (res == SCE_OK) {
            if (mem_.store(a(1), &info, sizeof(info)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_AUDIO_OUT_ERROR_INVALID_POINTER);
        }
        return static_cast<std::uint32_t>(res);
    }
    if (n == "sceAudioOutInit") return 0;

    // Math libc functions
    if (n == "sin") return put_double(std::sin(get_double(0, 0)));
    if (n == "cos") return put_double(std::cos(get_double(0, 0)));
    if (n == "tan") return put_double(std::tan(get_double(0, 0)));
    if (n == "atan2") return put_double(std::atan2(get_double(0, 0), get_double(1, 1)));
    if (n == "pow") return put_double(std::pow(get_double(0, 0), get_double(1, 1)));
    if (n == "exp") return put_double(std::exp(get_double(0, 0)));
    if (n == "log") return put_double(std::log(get_double(0, 0)));
    if (n == "sqrt") return put_double(std::sqrt(get_double(0, 0)));
    if (n == "floor") return put_double(std::floor(get_double(0, 0)));
    if (n == "ceil") return put_double(std::ceil(get_double(0, 0)));
    if (n == "fmod") return put_double(std::fmod(get_double(0, 0), get_double(1, 1)));
    if (n == "round") return put_double(std::round(get_double(0, 0)));
    if (n == "fabs") return put_double(std::fabs(get_double(0, 0)));

    if (n == "sinf") return put_float(std::sin(get_float(0, 0)));
    if (n == "cosf") return put_float(std::cos(get_float(0, 0)));
    if (n == "tanf") return put_float(std::tan(get_float(0, 0)));
    if (n == "atan2f") return put_float(std::atan2(get_float(0, 0), get_float(1, 1)));
    if (n == "powf") return put_float(std::pow(get_float(0, 0), get_float(1, 1)));
    if (n == "expf") return put_float(std::exp(get_float(0, 0)));
    if (n == "logf") return put_float(std::log(get_float(0, 0)));
    if (n == "sqrtf") return put_float(std::sqrt(get_float(0, 0)));
    if (n == "floorf") return put_float(std::floor(get_float(0, 0)));
    if (n == "ceilf") return put_float(std::ceil(get_float(0, 0)));
    if (n == "fmodf") return put_float(std::fmod(get_float(0, 0), get_float(1, 1)));
    if (n == "roundf") return put_float(std::round(get_float(0, 0)));
    if (n == "fabsf") return put_float(std::fabs(get_float(0, 0)));

    // Conversions and Sorting
    if (n == "strtol" || n == "strtoll") {
        if (!a(0)) return 0;
        std::string str = read_guest_string(a(0));
        char* end = nullptr;
        int base = static_cast<int>(a(2));
        long long val = std::strtoll(str.c_str(), &end, base);
        if (a(1)) {
            std::uint64_t consumed = static_cast<std::uint64_t>(end - str.c_str());
            put<std::uint64_t>(mem_, a(1), a(0) + consumed);
        }
        return static_cast<std::uint64_t>(val);
    }
    if (n == "strtoul" || n == "strtoull") {
        if (!a(0)) return 0;
        std::string str = read_guest_string(a(0));
        char* end = nullptr;
        int base = static_cast<int>(a(2));
        unsigned long long val = std::strtoull(str.c_str(), &end, base);
        if (a(1)) {
            std::uint64_t consumed = static_cast<std::uint64_t>(end - str.c_str());
            put<std::uint64_t>(mem_, a(1), a(0) + consumed);
        }
        return static_cast<std::uint64_t>(val);
    }
    if (n == "strtod") {
        if (!a(0)) return put_double(0.0);
        std::string str = read_guest_string(a(0));
        char* end = nullptr;
        double val = std::strtod(str.c_str(), &end);
        if (a(1)) {
            std::uint64_t consumed = static_cast<std::uint64_t>(end - str.c_str());
            put<std::uint64_t>(mem_, a(1), a(0) + consumed);
        }
        return put_double(val);
    }
    if (n == "strtof") {
        if (!a(0)) return put_float(0.0f);
        std::string str = read_guest_string(a(0));
        char* end = nullptr;
        float val = std::strtof(str.c_str(), &end);
        if (a(1)) {
            std::uint64_t consumed = static_cast<std::uint64_t>(end - str.c_str());
            put<std::uint64_t>(mem_, a(1), a(0) + consumed);
        }
        return put_float(val);
    }
    if (n == "qsort") {
        std::uint64_t base = a(0);
        std::size_t nmemb = static_cast<std::size_t>(a(1));
        std::size_t size = static_cast<std::size_t>(a(2));
        std::uint64_t compar = a(3);
        if (!base || nmemb <= 1 || size == 0) return 0;
        if (nmemb > 100000 || size > 65536) throw std::runtime_error("argumentos de qsort excessivos");

        std::vector<std::vector<std::uint8_t>> items(nmemb);
        for (std::size_t i = 0; i < nmemb; ++i) {
            items[i].resize(size);
            if (mem_.load(base + i * size, items[i].data(), size) != MemoryError::ok) {
                throw std::runtime_error("memoria de qsort invalida");
            }
        }

        auto do_compare = [&](const std::vector<std::uint8_t>& item_a,
                              const std::vector<std::uint8_t>& item_b,
                              std::uint64_t orig_addr_a,
                              std::uint64_t orig_addr_b) -> int {
            if (!compar) {
                return std::memcmp(item_a.data(), item_b.data(), size);
            }
            X86Interpreter interp(mem_);
            constexpr std::uint64_t trap_addr = 0x7FFFFFFF0000ULL;
            interp.add_trap_range(trap_addr, 32);
            std::uint64_t rsp = (c.gpr[4] > 256) ? (c.gpr[4] - 128) : 0x20000;
            rsp = (rsp & ~15ULL) - 8;
            mem_.store(rsp, &trap_addr, 8);
            interp.state = c;
            interp.state.rip = compar;
            interp.state.gpr[4] = rsp;
            interp.state.gpr[7] = orig_addr_a;
            interp.state.gpr[6] = orig_addr_b;
            auto r = interp.run(100000);
            if (r.stop == CpuStop::trap && r.rip == trap_addr) {
                return static_cast<std::int32_t>(interp.state.gpr[0]);
            }
            return std::memcmp(item_a.data(), item_b.data(), size);
        };

        std::vector<std::size_t> indices(nmemb);
        for (std::size_t i = 0; i < nmemb; ++i) indices[i] = i;

        std::stable_sort(indices.begin(), indices.end(), [&](std::size_t i1, std::size_t i2) {
            return do_compare(items[i1], items[i2], base + i1 * size, base + i2 * size) < 0;
        });

        for (std::size_t i = 0; i < nmemb; ++i) {
            mem_.store(base + i * size, items[indices[i]].data(), size);
        }
        return 0;
    }
    if (n == "bsearch") {
        std::uint64_t key = a(0);
        std::uint64_t base = a(1);
        std::size_t nmemb = static_cast<std::size_t>(a(2));
        std::size_t size = static_cast<std::size_t>(a(3));
        std::uint64_t compar = a(4);
        if (!key || !base || nmemb == 0 || size == 0) return 0;
        if (nmemb > 100000 || size > 65536) throw std::runtime_error("argumentos de bsearch excessivos");

        auto do_compare = [&](std::uint64_t key_addr, std::uint64_t elem_addr) -> int {
            if (!compar) {
                std::vector<std::uint8_t> ka(size), ea(size);
                mem_.load(key_addr, ka.data(), size);
                mem_.load(elem_addr, ea.data(), size);
                return std::memcmp(ka.data(), ea.data(), size);
            }
            X86Interpreter interp(mem_);
            constexpr std::uint64_t trap_addr = 0x7FFFFFFF0000ULL;
            interp.add_trap_range(trap_addr, 32);
            std::uint64_t rsp = (c.gpr[4] > 256) ? (c.gpr[4] - 128) : 0x20000;
            rsp = (rsp & ~15ULL) - 8;
            mem_.store(rsp, &trap_addr, 8);
            interp.state = c;
            interp.state.rip = compar;
            interp.state.gpr[4] = rsp;
            interp.state.gpr[7] = key_addr;
            interp.state.gpr[6] = elem_addr;
            auto r = interp.run(100000);
            if (r.stop == CpuStop::trap && r.rip == trap_addr) {
                return static_cast<std::int32_t>(interp.state.gpr[0]);
            }
            std::vector<std::uint8_t> ka(size), ea(size);
            mem_.load(key_addr, ka.data(), size);
            mem_.load(elem_addr, ea.data(), size);
            return std::memcmp(ka.data(), ea.data(), size);
        };

        std::size_t low = 0, high = nmemb;
        while (low < high) {
            std::size_t mid = low + (high - low) / 2;
            std::uint64_t mid_addr = base + mid * size;
            int cmp = do_compare(key, mid_addr);
            if (cmp < 0) {
                high = mid;
            } else if (cmp > 0) {
                low = mid + 1;
            } else {
                return mid_addr;
            }
        }
        return 0;
    }

    // snprintf
    if (n == "snprintf") {
        std::uint64_t dst = a(0);
        std::uint64_t max_size = a(1);
        std::uint64_t fmt_addr = a(2);
        if (!fmt_addr) return 0;

        std::string format = read_guest_string(fmt_addr, 4096);
        std::string out;
        unsigned int_arg_idx = 3;
        unsigned float_arg_idx = 0;

        for (std::size_t i = 0; i < format.size(); ) {
            if (format[i] != '%') {
                out.push_back(format[i++]);
                continue;
            }
            if (i + 1 < format.size() && format[i + 1] == '%') {
                out.push_back('%');
                i += 2;
                continue;
            }
            std::size_t spec_start = i;
            ++i;
            std::string flags_width_prec;
            while (i < format.size() && (std::strchr("-+ #0123456789.", format[i]) != nullptr)) {
                flags_width_prec.push_back(format[i++]);
            }
            while (i < format.size() && (format[i] == 'l' || format[i] == 'z' || format[i] == 'h')) {
                ++i;
            }
            if (i >= format.size()) {
                out.append(format.substr(spec_start));
                break;
            }
            char spec = format[i++];
            char piece[512] = {};
            if (spec == 'd' || spec == 'i') {
                std::int64_t val = static_cast<std::int64_t>(a(int_arg_idx++));
                std::string subspec = "%" + flags_width_prec + "lld";
                std::snprintf(piece, sizeof(piece), subspec.c_str(), static_cast<long long>(val));
                out.append(piece);
            } else if (spec == 'u') {
                std::uint64_t val = a(int_arg_idx++);
                std::string subspec = "%" + flags_width_prec + "llu";
                std::snprintf(piece, sizeof(piece), subspec.c_str(), static_cast<unsigned long long>(val));
                out.append(piece);
            } else if (spec == 'x' || spec == 'X') {
                std::uint64_t val = a(int_arg_idx++);
                std::string subspec = "%" + flags_width_prec + (spec == 'x' ? "llx" : "llX");
                std::snprintf(piece, sizeof(piece), subspec.c_str(), static_cast<unsigned long long>(val));
                out.append(piece);
            } else if (spec == 's') {
                std::uint64_t s_addr = a(int_arg_idx++);
                std::string s_val = s_addr ? read_guest_string(s_addr, 4096) : "(null)";
                std::string subspec = "%" + flags_width_prec + "s";
                std::snprintf(piece, sizeof(piece), subspec.c_str(), s_val.c_str());
                out.append(piece);
            } else if (spec == 'f' || spec == 'F' || spec == 'g' || spec == 'G' || spec == 'e' || spec == 'E') {
                double d = 0.0;
                if (float_arg_idx < 8 && (c.xmm[float_arg_idx][0] != 0 || (c.gpr[0] > 0 && c.gpr[0] <= 8))) {
                    std::memcpy(&d, &c.xmm[float_arg_idx][0], sizeof(d));
                    float_arg_idx++;
                } else if (float_arg_idx < 8 && c.xmm[float_arg_idx][0] == 0 && a(int_arg_idx) == 0 && float_arg_idx > 0) {
                    std::memcpy(&d, &c.xmm[float_arg_idx][0], sizeof(d));
                    float_arg_idx++;
                } else {
                    std::uint64_t gpr_val = a(int_arg_idx++);
                    std::memcpy(&d, &gpr_val, sizeof(d));
                }
                std::string subspec = "%" + flags_width_prec + spec;
                std::snprintf(piece, sizeof(piece), subspec.c_str(), d);
                out.append(piece);
            } else if (spec == 'p') {
                std::uint64_t ptr_val = a(int_arg_idx++);
                std::snprintf(piece, sizeof(piece), "%p", reinterpret_cast<void*>(static_cast<uintptr_t>(ptr_val)));
                out.append(piece);
            } else if (spec == 'c') {
                char c_val = static_cast<char>(a(int_arg_idx++));
                out.push_back(c_val);
            } else {
                out.append(format.substr(spec_start, i - spec_start));
            }
        }

        if (dst && max_size > 0) {
            std::size_t to_write = std::min<std::size_t>(out.size(), static_cast<std::size_t>(max_size - 1));
            if (to_write > 0) {
                mem_.store(dst, out.data(), to_write);
            }
            char null_byte = '\0';
            mem_.store(dst + to_write, &null_byte, 1);
        }
        return static_cast<std::uint64_t>(out.size());
    }
    if (n=="atexit" || n=="__cxa_atexit") {
        if (!a(0)) return invalid;
        exit_callbacks_.push_back(a(0)); // registrados, mas não executados sem trampoline guest
        return 0;
    }
    if (n=="__cxa_finalize") return a(0) ? invalid : 0; // não simular execução de callbacks
    if (n=="__cxa_guard_acquire") {
        if (!a(0)) return invalid;
        if (get<std::uint8_t>(mem_,a(0))) return 0;
        if (guards_[a(0)]) return 0x80020010; // pendente: guest deve ceder ao scheduler
        guards_[a(0)]=true; return 1;
    }
    if (n=="__cxa_guard_release" || n=="__cxa_guard_abort") {
        if (!a(0) || !guards_.count(a(0)) || !guards_[a(0)]) return invalid;
        if (n=="__cxa_guard_release") put<std::uint8_t>(mem_,a(0),1);
        guards_.erase(a(0)); return 0;
    }
    if (n=="sceKernelGetDirectMemorySize") return pool;
    if (n=="sceKernelAllocateDirectMemory") {
        auto low=a(0), high=std::min(a(1),pool), size=a(2), alignment=a(3) ? a(3) : page;
        if (!a(5) || !size || size%page || !valid_alignment(alignment) || low>=high || a(4)>10) return invalid;
        if (size>logical_limit-physical_used_) return no_memory;
        if (low>UINT64_MAX-alignment) return invalid;
        auto at=align(low,alignment);
        for (const auto& block : physical_) {
            if (at+size<=block.first) break;
            if (at<block.first+block.second) at=align(block.first+block.second,alignment);
        }
        if (at>high || size>high-at) return no_space;
        put(mem_,a(5),at); physical_[at]=size; physical_used_+=size; return 0;
    }
    if (n=="sceKernelMapDirectMemory" || n=="sceKernelMmap") {
        const bool direct=n=="sceKernelMapDirectMemory";
        const auto size=a(1), prot=a(2), flags=a(3), phys=direct?a(4):0;
        const auto alignment=direct && a(5) ? a(5) : page;
        if (!a(0) || !size || size%page || !valid_alignment(alignment) || (prot & ~0x37)) return invalid;
        if (direct) {
            auto block=physical_.upper_bound(phys);
            if (block==physical_.begin()) return invalid;
            --block;
            if (phys<block->first || phys-block->first>block->second || size>block->second-(phys-block->first)) return invalid;
        }
        if (size>logical_limit-mem_.used()) return no_memory;
        auto at=get<std::uint64_t>(mem_,a(0));
        if (!(flags & 0x10) || !at) { if (next_virtual_>UINT64_MAX-alignment) return no_memory; at=align(next_virtual_,alignment); }
        if (mem_.map(at,static_cast<std::size_t>(size),read|write)!=MemoryError::ok) return no_memory;
        try { put(mem_,a(0),at); } catch (...) { mem_.unmap(at); throw; }
        mappings_[at]=size; if (direct) direct_mappings_[at]=phys;
        next_virtual_=std::max(next_virtual_,at+size); return 0;
    }
    if (n=="sceKernelMunmap") {
        auto it=mappings_.find(a(0));
        if (it==mappings_.end() || a(1)!=it->second) return invalid;
        mem_.unmap(it->first); direct_mappings_.erase(it->first); mappings_.erase(it); return 0;
    }
    if (n=="sceKernelReleaseDirectMemory") {
        auto it=physical_.find(a(0)); if (it==physical_.end() || it->second!=a(1)) return invalid;
        for (const auto& mapping : direct_mappings_) {
            const auto offset=mapping.second, length=mappings_.at(mapping.first);
            if (offset>=it->first && offset-it->first<it->second && length) return 0x80020010;
        }
        physical_used_-=it->second; physical_.erase(it); return 0;
    }
    if (n=="scePthreadMutexattrInit" || n=="pthread_mutexattr_init") {
        auto h=next_handle_++; put(mem_,a(0),h); attrs_[h]=1; return 0;
    }
    if (n=="scePthreadMutexattrSettype" || n=="pthread_mutexattr_settype") {
        auto h=get<std::uint64_t>(mem_,a(0)); if (!attrs_.count(h) || a(1)<1 || a(1)>4) return invalid;
        attrs_[h]=static_cast<unsigned>(a(1)); return 0;
    }
    if (n=="scePthreadMutexattrDestroy" || n=="pthread_mutexattr_destroy") {
        auto h=get<std::uint64_t>(mem_,a(0)); if (!attrs_.erase(h)) return invalid;
        put<std::uint64_t>(mem_,a(0),0); return 0;
    }
    if (n=="scePthreadMutexInit" || n=="pthread_mutex_init") {
        unsigned type=1;
        if (a(1)) { auto attr=get<std::uint64_t>(mem_,a(1)); if (!attrs_.count(attr)) return invalid; type=attrs_[attr]; }
        std::uint32_t id=0; auto ret=KernelSyncManager::instance().pthread_mutex_init(&id,&type);
        if (ret) return static_cast<std::uint32_t>(ret);
        auto h=next_handle_++; mutexes_[h]=id; put(mem_,a(0),h); return 0;
    }
    if (n=="scePthreadMutexLock" || n=="scePthreadMutexUnlock" || n=="scePthreadMutexTrylock" || n=="scePthreadMutexDestroy" || n=="pthread_mutex_lock" || n=="pthread_mutex_unlock") {
        auto h=get<std::uint64_t>(mem_,a(0));
        if (!h && n.find("Unlock")==std::string::npos) {
            std::uint32_t id=0; auto ret=KernelSyncManager::instance().pthread_mutex_init(&id);
            if (ret) return static_cast<std::uint32_t>(ret);
            h=next_handle_++; mutexes_[h]=id; put(mem_,a(0),h);
        }
        auto it=mutexes_.find(h); if (it==mutexes_.end()) return invalid;
        std::int32_t result=0;
        if (n.find("Destroy")!=std::string::npos) { result=KernelSyncManager::instance().pthread_mutex_destroy(it->second); if (!result) { mutexes_.erase(it); put<std::uint64_t>(mem_,a(0),2); } }
        else if (n.find("Unlock")!=std::string::npos || n=="pthread_mutex_unlock") result=KernelSyncManager::instance().pthread_mutex_unlock(it->second);
        else if (n.find("Trylock")!=std::string::npos) result=KernelSyncManager::instance().pthread_mutex_trylock(it->second);
        else result=KernelSyncManager::instance().pthread_mutex_lock(it->second);
        return static_cast<std::uint32_t>(result);
    }
    if (n.rfind("scePthreadRwlock",0)==0) {
        auto h=get<std::uint64_t>(mem_,a(0));
        if (n=="scePthreadRwlockInit") {
            if (h) return invalid;
            h=next_handle_++; rwlocks_[h]=0; put(mem_,a(0),h); return 0;
        }
        auto it=rwlocks_.find(h); if (it==rwlocks_.end()) return invalid;
        if (n=="scePthreadRwlockDestroy") {
            if (it->second) return 0x80020010;
            rwlocks_.erase(it); put<std::uint64_t>(mem_,a(0),0); return 0;
        }
        if (n=="scePthreadRwlockUnlock") {
            if (!it->second) return 0x80020001;
            if (it->second<0) it->second=0; else --it->second;
            return 0;
        }
        if (n=="scePthreadRwlockRdlock" || n=="scePthreadRwlockTryrdlock") {
            if (it->second<0) return 0x80020010; // nunca bloquear a thread hospedeira
            ++it->second; return 0;
        }
        if (n=="scePthreadRwlockWrlock" || n=="scePthreadRwlockTrywrlock") {
            if (it->second) return 0x80020010;
            it->second=-1; return 0;
        }
    }
    if (n=="scePthreadCondInit" || n=="pthread_cond_init") {
        if (!a(0)) return invalid;
        auto h=next_handle_++; put(mem_,a(0),h); conds_[h]=0; return 0;
    }
    if (n=="scePthreadCondDestroy" || n=="pthread_cond_destroy" || n=="scePthreadCondSignal" || n=="pthread_cond_signal" || n=="scePthreadCondBroadcast" || n=="pthread_cond_broadcast" || n=="scePthreadCondWait" || n=="pthread_cond_wait" || n=="scePthreadCondTimedwait" || n=="pthread_cond_timedwait") {
        if (!a(0)) return invalid;
        auto h=get<std::uint64_t>(mem_,a(0)); auto it=conds_.find(h);
        if (it==conds_.end()) return invalid;
        if (n.find("Destroy")!=std::string::npos || n=="pthread_cond_destroy") { conds_.erase(it); put<std::uint64_t>(mem_,a(0),0); return 0; }
        if (n.find("Wait")!=std::string::npos || n.find("wait")!=std::string::npos) return 0x80020010; // sem scheduler: jamais bloquear
        return 0;
    }
    if (n=="scePthreadKeyCreate") {
        if (!a(0)) return invalid;
        const auto key=next_handle_++; keys_[key]=a(1); put<std::uint32_t>(mem_,a(0),static_cast<std::uint32_t>(key)); return 0;
    }
    if (n=="scePthreadKeyDelete" || n=="scePthreadSetspecific" || n=="scePthreadGetspecific") {
        const auto key=a(0); if (!keys_.count(key)) return n=="scePthreadGetspecific" ? 0 : invalid;
        if (n=="scePthreadKeyDelete") { keys_.erase(key); key_values_.erase(key); return 0; }
        if (n=="scePthreadSetspecific") { key_values_[key]=a(1); return 0; }
        auto it=key_values_.find(key); return it==key_values_.end() ? 0 : it->second;
    }
    if (n=="scePthreadOnce") {
        if (!a(0) || !a(1)) return invalid;
        if (get<std::uint32_t>(mem_,a(0))) return 0;
        return 0x80020010; // exige invocação do callback guest; não marcar concluído
    }
    if (n=="sceKernelCreateEventFlag") {
        if (!a(0) || a(2)>3) return invalid;
        const auto h=next_handle_++; event_flags_[h]=a(3); put<std::uint32_t>(mem_,a(0),static_cast<std::uint32_t>(h)); return 0;
    }
    if (n=="sceKernelSetEventFlag" || n=="sceKernelDeleteEventFlag" || n=="sceKernelPollEventFlag" || n=="sceKernelWaitEventFlag") {
        auto it=event_flags_.find(a(0)); if (it==event_flags_.end()) return invalid;
        if (n=="sceKernelDeleteEventFlag") { event_flags_.erase(it); return 0; }
        if (n=="sceKernelSetEventFlag") { it->second |= a(1); return 0; }
        const auto mask=a(1), mode=a(2);
        if (!mask || (mode & ~0x33)) return invalid;
        const auto bits=it->second & mask;
        if ((mode&1 ? bits==mask : bits!=0)) {
            if (!a(3)) return invalid;
            put(mem_,a(3),it->second);
            if (mode&0x10) it->second &= ~bits;
            if (mode&0x20) it->second=0;
            return 0;
        }
        return 0x80020010;
    }
    if (n=="sceKernelUsleep" || n=="usleep") return 0; // yield cooperativo
    if (n=="sceKernelGetProcessTime") {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    if (n=="sceKernelClockGettime" || n=="clock_gettime") {
        if (!a(1) || a(0)>4) return invalid;
        const auto now=a(0)==0 ? std::chrono::system_clock::now().time_since_epoch() : std::chrono::steady_clock::now().time_since_epoch();
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        put<std::int64_t>(mem_,a(1),ns/1000000000); put<std::int64_t>(mem_,a(1)+8,ns%1000000000); return 0;
    }
    if (n=="sceSysmoduleLoadModule" || n=="sceSysmoduleUnloadModule" || n=="sceSysmoduleIsLoaded" || n=="sceAppContentInitialize" || n=="sceAppContentAppParamGetInt" || n=="sceAppContentGetAddcontInfoList") return 0x80020002; // serviço indisponível
    if (n=="memset" || n=="memcpy" || n=="memmove" || n=="memcmp") {
        const auto len=a(2); if (len>logical_limit || len>std::numeric_limits<std::size_t>::max()) throw std::runtime_error("tamanho libc inválido");
        if (!len) return n=="memcmp" ? 0 : a(0);
        if (a(0)>UINT64_MAX-len || (n!="memset" && a(1)>UINT64_MAX-len)) throw std::runtime_error("endereço libc inválido");
        std::array<std::uint8_t,4096> bytes{}, left{};
        const bool backward=n=="memmove" && a(0)>a(1) && a(0)-a(1)<len;
        for (std::uint64_t done=0;done<len;) {
            const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),len-done));
            const auto offset=backward ? len-done-count : done;
            if (n=="memset") std::fill_n(bytes.begin(),count,static_cast<std::uint8_t>(a(1)));
            else if (mem_.load(a(1)+offset,bytes.data(),count)!=MemoryError::ok) throw std::runtime_error("origem libc inválida");
            if (n=="memcmp") {
                if (mem_.load(a(0)+offset,left.data(),count)!=MemoryError::ok) throw std::runtime_error("origem memcmp inválida");
                for (std::size_t i=0;i<count;++i) if (left[i]!=bytes[i]) return static_cast<std::uint64_t>(static_cast<std::int64_t>(int(left[i])-int(bytes[i])));
            } else if (mem_.store(a(0)+offset,bytes.data(),count)!=MemoryError::ok) throw std::runtime_error("destino libc inválido");
            done+=count;
        }
        return n=="memcmp" ? 0 : a(0);
    }
    if (n=="vprintf" || n=="printf") {
        std::string format;
        for (std::uint64_t i=0;i<4096;++i) {
            auto ch=get<char>(mem_,a(0)+i);
            if (!ch) break;
            format+=ch;
        }
        if (format.size()==4096) throw std::runtime_error("formato printf excessivo");
        // Apenas strings literais: conversões exigem interpretar va_list Orbis.
        if (format.find('%')!=std::string::npos) throw std::runtime_error("import não implementado: formato vprintf");
        std::fwrite(format.data(),1,format.size(),stdout);
        return format.size();
    }
    if (n=="strlen" || n=="strcmp" || n=="strncmp" || n=="strcasecmp" || n=="strncasecmp" || n=="strchr" || n=="strrchr" || n=="strcpy" || n=="strstr") {
        auto read_char=[&](std::uint64_t base,std::uint64_t offset) {
            if (offset>=1u<<20 || base>UINT64_MAX-offset) throw std::runtime_error("string guest excessiva");
            return get<std::uint8_t>(mem_,base+offset);
        };
        const bool compare=n=="strcmp" || n=="strncmp" || n=="strcasecmp" || n=="strncasecmp";
        const bool fold=n=="strcasecmp" || n=="strncasecmp";
        const auto limit=(n=="strncmp" || n=="strncasecmp") ? std::min<std::uint64_t>(a(2),1u<<20) : 1u<<20;
        if (compare) {
            for (std::uint64_t i=0;i<limit;++i) {
                auto x=read_char(a(0),i), y=read_char(a(1),i);
                if (fold) { if (x>='A' && x<='Z') x+=32; if (y>='A' && y<='Z') y+=32; }
                if (x!=y || !x) return static_cast<std::uint64_t>(static_cast<std::int64_t>(int(x)-int(y)));
            }
            if (limit==1u<<20 && n!="strncmp" && n!="strncasecmp") throw std::runtime_error("string sem terminador");
            return 0;
        }
        if (n=="strstr" && !read_char(a(1),0)) return a(0);
        std::uint64_t last=0;
        for (std::uint64_t i=0;i<1u<<20;++i) {
            if (n=="strcpy") { const auto src=read_char(a(1),i); if (a(0)>UINT64_MAX-i) throw std::runtime_error("destino string inválido"); put<std::uint8_t>(mem_,a(0)+i,src); if (!src) return a(0); continue; }
            const auto ch=read_char(a(0),i);
            if (n=="strstr" && ch) {
                std::uint64_t j=0;
                while (i+j<1u<<20 && read_char(a(1),j) && read_char(a(0),i+j)==read_char(a(1),j)) ++j;
                if (j<1u<<20 && !read_char(a(1),j)) return a(0)+i;
            }
            if (n=="strchr" || n=="strrchr") {
                if (ch==static_cast<std::uint8_t>(a(1))) { if (n=="strchr") return a(0)+i; last=a(0)+i; }
            }
            if (!ch) return n=="strlen" ? i : (n=="strrchr" ? last : 0);
        }
        throw std::runtime_error("string sem terminador");
    }
    if (n=="realloc") {
        if (!a(0)) { CpuState copy=c; copy.gpr[7]=a(1); return handle("malloc",copy); }
        auto it=heap_.find(a(0)); if (it==heap_.end()) throw std::runtime_error("realloc guest inválido");
        if (!a(1)) { mem_.unmap(it->first); heap_.erase(it); return 0; }
        if (a(1)<=it->second) return a(0);
        CpuState copy=c; copy.gpr[7]=a(1);
        const auto fresh=handle("malloc",copy); if (!fresh) return 0;
        std::array<std::uint8_t,4096> buffer{};
        for (std::uint64_t pos=0;pos<it->second;) {
            const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),it->second-pos));
            if (mem_.load(a(0)+pos,buffer.data(),count)!=MemoryError::ok || mem_.store(fresh+pos,buffer.data(),count)!=MemoryError::ok) throw std::runtime_error("realloc memória inválida");
            pos+=count;
        }
        mem_.unmap(it->first); heap_.erase(it); return fresh;
    }
    if (n=="malloc" || n=="calloc" || n=="_Znwm" || n=="_Znam") {
        std::uint64_t size=a(0);
        if (n=="calloc") { if (a(1) && size>UINT64_MAX/a(1)) return 0; size*=a(1); }
        size=std::max<std::uint64_t>(size,1);
        if (size>logical_limit-mem_.used() || size>UINT64_MAX-15) return 0;
        const auto at=align(next_heap_,16), extent=align(size,16);
        if (at>UINT64_MAX-extent || mem_.map(at,static_cast<std::size_t>(extent),read|write)!=MemoryError::ok) return 0;
        heap_[at]=extent; next_heap_=at+extent; return at;
    }
    if (n=="free" || n=="_ZdlPv" || n=="_ZdaPv") {
        if (!a(0)) return 0;
        auto it=heap_.find(a(0)); if (it==heap_.end()) throw std::runtime_error("free guest inválido");
        mem_.unmap(it->first); heap_.erase(it); return 0;
    }
    throw std::runtime_error("import não implementado: " + n);
}
} // namespace bb
