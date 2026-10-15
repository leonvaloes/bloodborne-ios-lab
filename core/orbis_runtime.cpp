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
#include <filesystem>
#include <system_error>
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

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
[[maybe_unused]] bool is_likely_string_ptr(const GuestMemory& mem, std::uint64_t addr) {
    if (addr < 0x1000 || addr > 0x7fffffffffffULL) return false;
    char buf[64];
    std::size_t len = 0;
    for (; len < sizeof(buf); ++len) {
        char ch = 0;
        if (mem.load(addr + len, &ch, 1) != MemoryError::ok) return false;
        buf[len] = ch;
        if (ch == '\0') break;
        if (ch != '\t' && (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7E)) {
            return false;
        }
    }
    return len > 0 && len < sizeof(buf) && buf[len] == '\0';
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
        "scePthreadMutexTrylock", "scePthreadMutexTimedlock",
        "sceKernelMmap", "sceKernelMunmap", "sceKernelVirtualQuery",
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
        "snprintf", "vsnprintf",
        // Kernel semaphores
        "sceKernelCreateSema", "sceKernelDeleteSema", "sceKernelWaitSema", "sceKernelPollSema", "sceKernelSignalSema",
        "sceKernelGetSemaInfo",
        // Kernel event flags
        "sceKernelCreateEventFlag", "sceKernelDeleteEventFlag", "sceKernelSetEventFlag", "sceKernelClearEventFlag",
        "sceKernelWaitEventFlag", "sceKernelPollEventFlag",
        // Pthread condition variables (sce-prefixed)
        "scePthreadCondInit", "scePthreadCondDestroy", "scePthreadCondWait", "scePthreadCondTimedwait",
        "scePthreadCondSignal", "scePthreadCondBroadcast",
        // Pthread once/key
        "scePthreadOnce", "scePthreadKeyCreate", "scePthreadKeyDelete", "scePthreadSetspecific", "scePthreadGetspecific",
        "pthread_once", "pthread_key_create", "pthread_key_delete", "pthread_setspecific", "pthread_getspecific",
        // Pthread mutex attr (sce-prefixed)
        "scePthreadMutexattrSetprotocol", "scePthreadMutexattrGetprotocol",
        "scePthreadMutexattrSetprioceiling", "scePthreadMutexattrGetprioceiling",
        "scePthreadMutexattrGettype",
        "pthread_mutexattr_getprotocol", "pthread_mutexattr_setprotocol",
        "pthread_mutexattr_getprioceiling", "pthread_mutexattr_setprioceiling",
        "pthread_mutexattr_gettype",
        // Pthread rwlock (sce-prefixed)
        "scePthreadRwlockInit", "scePthreadRwlockDestroy", "scePthreadRwlockRdlock", "scePthreadRwlockTryrdlock",
        "scePthreadRwlockTimedrdlock", "scePthreadRwlockWrlock", "scePthreadRwlockTrywrlock",
        "scePthreadRwlockTimedwrlock", "scePthreadRwlockUnlock",
        // Pthread mutex (sce-prefixed, already in orbis_import_names.h but ensure coverage)
        "scePthreadMutexInit", "scePthreadMutexLock", "scePthreadMutexUnlock", "scePthreadMutexTrylock",
        "scePthreadMutexDestroy", "scePthreadMutexTimedlock",
        "scePthreadMutexGetprioceiling", "scePthreadMutexSetprioceiling",
        "pthread_mutex_getprioceiling", "pthread_mutex_setprioceiling",
    };
    for (const char* s : symbols) names_.emplace(orbis_nid(s), s);
}
OrbisRuntime::~OrbisRuntime() {
    for (auto& pair : open_files_) {
        if (pair.second) std::fclose(pair.second);
    }
    open_files_.clear();
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
        name=="scePthreadMutexTimedlock" || name=="pthread_mutex_timedlock" ||
        name=="scePthreadCondWait" || name=="pthread_cond_wait" ||
        name=="scePthreadCondTimedwait" || name=="pthread_cond_timedwait" ||
        name=="sceKernelWaitEventFlag" || name=="sceKernelPollEventFlag" ||
        name=="sceKernelWaitSema" || name=="scePthreadOnce" ||
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
    auto read_guest_string = [&](std::uint64_t addr, std::size_t max_len = 4096) -> std::string {
        std::string s;
        for (std::size_t i = 0; i < max_len; ++i) {
            char ch = 0;
            if (mem_.load(addr + i, &ch, 1) != MemoryError::ok || ch == '\0') break;
            s.push_back(ch);
        }
        return s;
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
            if (attrs_.count(attr)) recursive = (attrs_.at(attr) == 2);
            else if (attrs_.count(a(1))) recursive = (attrs_.at(a(1)) == 2);
            else if (attr == 2) recursive = true;
        }
        const auto id = scheduler_->mutex_create(recursive);
        const auto handle = next_handle_++;
        guest_mutexes_[handle] = id;
        put(mem_, a(0), handle);
        return result(0);
    }

    if (n == "scePthreadMutexGetprioceiling" || n == "pthread_mutex_getprioceiling") {
        if (!a(0) || !a(1)) return result(invalid);
        put<std::int32_t>(mem_, a(1), 0);
        return result(0);
    }
    if (n == "scePthreadMutexSetprioceiling" || n == "pthread_mutex_setprioceiling") {
        if (!a(0)) return result(invalid);
        if (a(2)) put<std::int32_t>(mem_, a(2), 0);
        return result(0);
    }
    if (n == "scePthreadMutexattrGetprioceiling" || n == "pthread_mutexattr_getprioceiling") {
        if (!a(0) || !a(1)) return result(invalid);
        put<std::int32_t>(mem_, a(1), 0);
        return result(0);
    }
    if (n == "scePthreadMutexattrSetprioceiling" || n == "pthread_mutexattr_setprioceiling") {
        if (!a(0)) return result(invalid);
        return result(0);
    }
    if (n == "scePthreadMutexattrGetprotocol" || n == "pthread_mutexattr_getprotocol" ||
        n == "scePthreadMutexattrSetprotocol" || n == "pthread_mutexattr_setprotocol") {
        if (!a(0)) return result(invalid);
        if (n.find("Get") != std::string::npos && a(1)) put<std::int32_t>(mem_, a(1), 0);
        return result(0);
    }
    if (n == "scePthreadMutexattrGettype" || n == "pthread_mutexattr_gettype") {
        if (!a(0) || !a(1)) return result(invalid);
        auto h = get<std::uint64_t>(mem_, a(0));
        unsigned t = 1;
        if (attrs_.count(h)) t = attrs_[h];
        put<std::int32_t>(mem_, a(1), static_cast<std::int32_t>(t));
        return result(0);
    }

    if (n == "scePthreadMutexLock" || n == "pthread_mutex_lock" ||
        n == "scePthreadMutexUnlock" || n == "pthread_mutex_unlock" ||
        n == "scePthreadMutexTrylock" || n == "pthread_mutex_trylock" ||
        n == "scePthreadMutexDestroy" || n == "pthread_mutex_destroy") {
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
        const auto status = (n == "scePthreadMutexTrylock" || n == "pthread_mutex_trylock")
            ? scheduler_->try_lock(tid, id)
            : scheduler_->lock(tid, id);
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
            if (a(2) < 0x1000) {
                timeout = a(2);
            } else {
                std::int64_t sec = 0, nsec = 0;
                if (mem_.load(a(2), &sec, 8) != MemoryError::ok ||
                    mem_.load(a(2) + 8, &nsec, 8) != MemoryError::ok) {
                    return result(invalid);
                }
                if (sec >= 1500000000) {
                    auto now_sys = std::chrono::system_clock::now().time_since_epoch();
                    auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(now_sys).count();
                    auto now_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(now_sys).count() % 1000000000;
                    if (sec < now_sec || (sec == now_sec && nsec <= now_nsec)) {
                        return result(0x8002003c); // ETIMEDOUT
                    }
                    timeout = static_cast<std::uint64_t>(sec - now_sec) * 1000000ULL +
                              static_cast<std::uint64_t>((nsec - now_nsec) / 1000);
                } else if (sec > 0 || nsec > 0) {
                    timeout = static_cast<std::uint64_t>(sec) * 1000000ULL +
                              static_cast<std::uint64_t>(nsec / 1000);
                }
            }
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
        bool is_sdk = is_likely_string_ptr(mem_, a(0)) || a(0) == 0;
        if (is_sdk) {
            const auto handle = next_handle_++;
            guest_events_[handle] = scheduler_->event_create(a(2));
            return result(handle);
        } else {
            if (!a(0) || a(2) > 3) return result(invalid);
            const auto handle = next_handle_++;
            guest_events_[handle] = scheduler_->event_create(a(3));
            put<std::uint32_t>(mem_, a(0), static_cast<std::uint32_t>(handle));
            return result(0);
        }
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

    if (n == "scePthreadOnce" || n == "pthread_once") {
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

    // Kernel Semaphores
    if (n == "sceKernelCreateSema") {
        bool is_sdk = is_likely_string_ptr(mem_, a(0)) || a(0) == 0;
        std::string sema_name = "sema";
        std::uint32_t attr = 0;
        std::int32_t init_val = 0, max_val = 1;
        std::uint64_t out_id_addr = 0;

        if (is_sdk) {
            if (a(0)) sema_name = read_guest_string(a(0), 32);
            attr = static_cast<std::uint32_t>(a(1));
            init_val = static_cast<std::int32_t>(a(2));
            max_val = static_cast<std::int32_t>(a(3));
        } else {
            out_id_addr = a(0);
            if (!out_id_addr) return result(invalid);
            if (a(1)) sema_name = read_guest_string(a(1), 32);
            if (a(3) != 0 && (a(3) >> 16) != 0) {
                init_val = static_cast<std::int32_t>(a(3) & 0xFFFF);
                max_val = static_cast<std::int32_t>((a(3) >> 16) & 0xFFFF);
            } else if (a(2) != 0 && (a(2) >> 16) != 0) {
                init_val = static_cast<std::int32_t>(a(2) & 0xFFFF);
                max_val = static_cast<std::int32_t>((a(2) >> 16) & 0xFFFF);
            } else if (a(3) != 0) {
                init_val = static_cast<std::int32_t>(a(2));
                max_val = static_cast<std::int32_t>(a(3));
            } else {
                init_val = static_cast<std::int32_t>(a(2));
                max_val = std::max<std::int32_t>(1, init_val);
            }
        }
        if (max_val <= 0) max_val = std::max<std::int32_t>(1, init_val);
        if (init_val < 0 || max_val <= 0 || init_val > max_val) return result(invalid);

        auto sid = scheduler_->sema_create(init_val, max_val);
        if (!sid) return result(no_memory);
        auto handle = next_handle_++;
        guest_semas_[handle] = sid;

        SemaRecord rec;
        rec.name = sema_name;
        rec.attr = attr;
        rec.init_count = init_val;
        rec.max_count = max_val;
        sema_records_[static_cast<std::uint32_t>(handle)] = rec;

        if (is_sdk) {
            return result(handle);
        } else {
            put<std::uint32_t>(mem_, out_id_addr, static_cast<std::uint32_t>(handle));
            return result(0);
        }
    }

    if (n == "sceKernelWaitSema") {
        if (!a(0)) return result(invalid);
        auto it = guest_semas_.find(a(0));
        if (it == guest_semas_.end()) return result(invalid);
        std::int32_t need = a(1) ? static_cast<std::int32_t>(a(1)) : 1;
        std::uint64_t timeout = 0;
        if (a(2)) {
            std::uint32_t to32 = 0;
            if (mem_.load(a(2), &to32, 4) == MemoryError::ok) timeout = to32;
        }
        auto status = scheduler_->sema_wait(tid, it->second, need, timeout);
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

    if (n == "sceKernelPollSema") {
        if (!a(0)) return result(invalid);
        auto it = guest_semas_.find(a(0));
        if (it == guest_semas_.end()) return result(invalid);
        std::int32_t need = a(1) ? static_cast<std::int32_t>(a(1)) : 1;
        return result(static_cast<std::uint32_t>(scheduler_->sema_poll(it->second, need)));
    }

    if (n == "sceKernelSignalSema") {
        if (!a(0)) return result(invalid);
        auto it = guest_semas_.find(a(0));
        if (it == guest_semas_.end()) return result(invalid);
        std::int32_t count = a(1) ? static_cast<std::int32_t>(a(1)) : 1;
        return result(static_cast<std::uint32_t>(scheduler_->sema_signal(it->second, count)));
    }

    if (n == "sceKernelDeleteSema") {
        if (!a(0)) return result(invalid);
        auto it = guest_semas_.find(a(0));
        if (it == guest_semas_.end()) return result(invalid);
        scheduler_->sema_delete(it->second);
        guest_semas_.erase(it);
        sema_records_.erase(static_cast<std::uint32_t>(a(0)));
        return result(0);
    }

    if (n == "sceKernelGetSemaInfo") {
        if (!a(0) || !a(1)) return result(invalid);
        auto sid_handle = static_cast<std::uint32_t>(a(0));
        auto it_rec = sema_records_.find(sid_handle);
        if (it_rec == sema_records_.end()) return result(0x80020003); // ESRCH
        auto it_guest = guest_semas_.find(a(0));
        std::int32_t cur_cnt = it_rec->second.init_count;
        std::uint32_t num_waiters = 0;
        if (it_guest != guest_semas_.end()) {
            cur_cnt = scheduler_->sema_count(it_guest->second);
            num_waiters = static_cast<std::uint32_t>(scheduler_->sema_waiters(it_guest->second));
        }
        SceKernelSemaInfo info{};
        std::uint64_t guest_size = 0;
        mem_.load(a(1), &guest_size, 8);
        info.size = (guest_size >= 48 && guest_size <= sizeof(SceKernelSemaInfo)) ? guest_size : sizeof(SceKernelSemaInfo);
        std::strncpy(info.name, it_rec->second.name.c_str(), sizeof(info.name) - 1);
        info.attr = it_rec->second.attr;
        info.init_count = it_rec->second.init_count;
        info.current_count = cur_cnt >= 0 ? cur_cnt : it_rec->second.init_count;
        info.max_count = it_rec->second.max_count;
        info.num_wait_threads = num_waiters;
        auto to_write = std::min<std::size_t>(sizeof(info), info.size);
        if (mem_.store(a(1), &info, to_write) != MemoryError::ok) return result(invalid);
        return result(0);
    }

    // Pthread TLS
    if (n == "scePthreadKeyCreate" || n == "pthread_key_create") {
        if (!a(0)) return result(invalid);
        std::uint64_t destructor = a(1);
        auto key = scheduler_->tls_create(destructor);
        put<std::uint32_t>(mem_, a(0), static_cast<std::uint32_t>(key));
        return result(0);
    }

    if (n == "scePthreadKeyDelete" || n == "pthread_key_delete") {
        if (!a(0)) return result(invalid);
        std::uint32_t key = static_cast<std::uint32_t>(a(0));
        scheduler_->tls_delete(key);
        return result(0);
    }

    if (n == "scePthreadSetspecific" || n == "pthread_setspecific") {
        std::uint32_t key = static_cast<std::uint32_t>(a(0));
        std::uint64_t value = a(1);
        auto status = scheduler_->tls_set(tid, key, value);
        return result(static_cast<std::uint32_t>(status));
    }

    if (n == "scePthreadGetspecific" || n == "pthread_getspecific") {
        std::uint32_t key = static_cast<std::uint32_t>(a(0));
        return result(scheduler_->tls_get(tid, key));
    }

    // Pthread Mutex Timedlock
    if (n == "scePthreadMutexTimedlock" || n == "pthread_mutex_timedlock") {
        if (!a(0) || !a(1)) return result(invalid);
        auto handle = get<std::uint64_t>(mem_, a(0));
        if (!handle) return result(invalid);
        if (!guest_mutexes_.count(handle)) return result(invalid);
        const auto id = guest_mutexes_.at(handle);
        std::uint64_t timeout = get<std::uint64_t>(mem_, a(1));
        auto status = scheduler_->lock(tid, id, timeout);
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
    // Debug: print unhandled imports
    // std::printf("HLE handle: %s\n", n.c_str());

    auto read_guest_string = [&](std::uint64_t addr, std::size_t max_len = 4096) -> std::string {
        std::string s;
        for (std::size_t i = 0; i < max_len; ++i) {
            char ch = 0;
            if (mem_.load(addr + i, &ch, 1) != MemoryError::ok || ch == '\0') break;
            s.push_back(ch);
        }
        return s;
    };
    // Pela ABI SysV, argumentos escalares FP vêm dos XMM, inclusive quando seus bits são zero.
    auto get_double = [&](unsigned xmm_idx) -> double {
        double val = 0;
        std::memcpy(&val, &c.xmm[xmm_idx][0], sizeof(val));
        return val;
    };
    auto put_double = [&](double val) -> std::uint64_t {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &val, sizeof(bits));
        c.xmm[0][0] = bits;
        return bits;
    };
    auto get_float = [&](unsigned xmm_idx) -> float {
        std::uint32_t bits = static_cast<std::uint32_t>(c.xmm[xmm_idx][0]);
        float val = 0;
        std::memcpy(&val, &bits, sizeof(val));
        return val;
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

    // libSceVideoOut HLE
    if (n == "sceVideoOutOpen") {
        return next_handle_++;
    }
    if (n == "sceVideoOutClose") {
        return 0;
    }
    if (n == "sceVideoOutSetFlipRate") {
        return 0;
    }
    if (n == "sceVideoOutAddFlipEvent") {
        return 0;
    }
    if (n == "sceVideoOutSetBufferAttribute") {
        std::uint64_t attr_addr = a(0);
        if (attr_addr) {
            std::uint32_t fields[6] = {
                static_cast<std::uint32_t>(a(1)), // pixelFormat
                static_cast<std::uint32_t>(a(2)), // tilingMode
                static_cast<std::uint32_t>(a(3)), // aspectRatio
                static_cast<std::uint32_t>(a(4)), // width
                static_cast<std::uint32_t>(a(5)), // height
                static_cast<std::uint32_t>(a(6))  // pitchInPixel
            };
            mem_.store(attr_addr, fields, sizeof(fields));
        }
        return 0;
    }
    if (n == "sceVideoOutRegisterBuffers") {
        int start_idx = static_cast<int>(a(1));
        std::uint64_t addrs_ptr = a(2);
        int buf_count = static_cast<int>(a(3));
        std::uint64_t attr_ptr = a(4);

        if (video_out_ && addrs_ptr && buf_count > 0 && buf_count <= 16 && attr_ptr) {
            std::uint32_t attr_fields[6] = {};
            if (mem_.load(attr_ptr, attr_fields, sizeof(attr_fields)) == MemoryError::ok) {
                std::uint32_t width = attr_fields[3];
                std::uint32_t height = attr_fields[4];
                std::uint32_t pitch = attr_fields[5];
                if (!pitch) pitch = width;
                std::uint32_t bpp = 4;

                for (int i = 0; i < buf_count; ++i) {
                    std::uint64_t buf_addr = 0;
                    if (mem_.load(addrs_ptr + i * 8, &buf_addr, 8) == MemoryError::ok) {
                        VideoBuffer vb{buf_addr, width, height, pitch, bpp};
                        video_out_->register_buffer(static_cast<std::uint32_t>(start_idx + i), vb);
                    }
                }
            }
        }
        return 0;
    }
    if (n == "sceVideoOutSubmitFlip") {
        int buf_idx = static_cast<int>(a(1));
        std::uint64_t flip_arg = a(3);
        if (video_out_) {
            auto status = video_out_->submit_flip(static_cast<std::uint32_t>(buf_idx), flip_arg);
            return status == VideoStatus::ok ? 0 : static_cast<std::uint64_t>(-1);
        }
        return 0;
    }
    if (n == "sceVideoOutGetFlipStatus") {
        std::uint64_t status_ptr = a(1);
        if (status_ptr && video_out_) {
            std::uint64_t count = video_out_->last_flip().sequence;
            mem_.store(status_ptr, &count, 8);
        }
        return 0;
    }

    // Math libc functions
    if (n == "sin") return put_double(std::sin(get_double(0)));
    if (n == "cos") return put_double(std::cos(get_double(0)));
    if (n == "tan") return put_double(std::tan(get_double(0)));
    if (n == "atan2") return put_double(std::atan2(get_double(0), get_double(1)));
    if (n == "pow") return put_double(std::pow(get_double(0), get_double(1)));
    if (n == "exp") return put_double(std::exp(get_double(0)));
    if (n == "log") return put_double(std::log(get_double(0)));
    if (n == "sqrt") return put_double(std::sqrt(get_double(0)));
    if (n == "floor") return put_double(std::floor(get_double(0)));
    if (n == "ceil") return put_double(std::ceil(get_double(0)));
    if (n == "fmod") return put_double(std::fmod(get_double(0), get_double(1)));
    if (n == "round") return put_double(std::round(get_double(0)));
    if (n == "fabs") return put_double(std::fabs(get_double(0)));

    if (n == "sinf") return put_float(std::sin(get_float(0)));
    if (n == "cosf") return put_float(std::cos(get_float(0)));
    if (n == "tanf") return put_float(std::tan(get_float(0)));
    if (n == "atan2f") return put_float(std::atan2(get_float(0), get_float(1)));
    if (n == "powf") return put_float(std::pow(get_float(0), get_float(1)));
    if (n == "expf") return put_float(std::exp(get_float(0)));
    if (n == "logf") return put_float(std::log(get_float(0)));
    if (n == "sqrtf") return put_float(std::sqrt(get_float(0)));
    if (n == "floorf") return put_float(std::floor(get_float(0)));
    if (n == "ceilf") return put_float(std::ceil(get_float(0)));
    if (n == "fmodf") return put_float(std::fmod(get_float(0), get_float(1)));
    if (n == "roundf") return put_float(std::round(get_float(0)));
    if (n == "fabsf") return put_float(std::fabs(get_float(0)));

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
    if (n=="scePthreadMutexattrGettype" || n=="pthread_mutexattr_gettype") {
        if (!a(0) || !a(1)) return invalid;
        auto h=get<std::uint64_t>(mem_,a(0));
        unsigned t=1; if (attrs_.count(h)) t=attrs_[h];
        put<std::int32_t>(mem_,a(1),static_cast<std::int32_t>(t)); return 0;
    }
    if (n=="scePthreadMutexattrGetprotocol" || n=="pthread_mutexattr_getprotocol" ||
        n=="scePthreadMutexattrSetprotocol" || n=="pthread_mutexattr_setprotocol" ||
        n=="scePthreadMutexattrGetprioceiling" || n=="pthread_mutexattr_getprioceiling" ||
        n=="scePthreadMutexattrSetprioceiling" || n=="pthread_mutexattr_setprioceiling") {
        if (!a(0)) return invalid;
        if (n.find("Get") != std::string::npos && a(1)) put<std::int32_t>(mem_, a(1), 0);
        return 0;
    }
    if (n=="scePthreadMutexGetprioceiling" || n=="pthread_mutex_getprioceiling") {
        if (!a(0) || !a(1)) return invalid;
        put<std::int32_t>(mem_, a(1), 0); return 0;
    }
    if (n=="scePthreadMutexSetprioceiling" || n=="pthread_mutex_setprioceiling") {
        if (!a(0)) return invalid;
        if (a(2)) put<std::int32_t>(mem_, a(2), 0);
        return 0;
    }
    if (n=="scePthreadMutexInit" || n=="pthread_mutex_init") {
        unsigned type=1;
        if (a(1)) {
            auto attr=get<std::uint64_t>(mem_,a(1));
            if (attrs_.count(attr)) type=attrs_[attr];
            else if (attrs_.count(a(1))) type=attrs_[a(1)];
            else if (attr >= 1 && attr <= 4) type=static_cast<unsigned>(attr);
            else return invalid;
        }
        std::uint32_t id=0; auto ret=KernelSyncManager::instance().pthread_mutex_init(&id,&type);
        if (ret) return static_cast<std::uint32_t>(ret);
        auto h=next_handle_++; mutexes_[h]=id; put(mem_,a(0),h); return 0;
    }
    if (n=="scePthreadMutexLock" || n=="scePthreadMutexUnlock" || n=="scePthreadMutexTrylock" || n=="scePthreadMutexDestroy" || n=="pthread_mutex_lock" || n=="pthread_mutex_unlock" || n=="pthread_mutex_trylock" || n=="pthread_mutex_destroy" || n=="scePthreadMutexTimedlock" || n=="pthread_mutex_timedlock") {
        auto h=get<std::uint64_t>(mem_,a(0));
        if (!h && n.find("Unlock")==std::string::npos && n.find("Destroy")==std::string::npos) {
            std::uint32_t id=0; auto ret=KernelSyncManager::instance().pthread_mutex_init(&id);
            if (ret) return static_cast<std::uint32_t>(ret);
            h=next_handle_++; mutexes_[h]=id; put(mem_,a(0),h);
        }
        auto it=mutexes_.find(h); if (it==mutexes_.end()) return invalid;
        std::int32_t result=0;
        if (n.find("Destroy")!=std::string::npos || n=="pthread_mutex_destroy") { result=KernelSyncManager::instance().pthread_mutex_destroy(it->second); if (!result) { mutexes_.erase(it); put<std::uint64_t>(mem_,a(0),2); } }
        else if (n.find("Unlock")!=std::string::npos || n=="pthread_mutex_unlock") result=KernelSyncManager::instance().pthread_mutex_unlock(it->second);
        else if (n.find("Trylock")!=std::string::npos || n=="pthread_mutex_trylock") result=KernelSyncManager::instance().pthread_mutex_trylock(it->second);
        else if (n.find("Timedlock")!=std::string::npos || n=="pthread_mutex_timedlock") {
            std::uint32_t to = 0;
            if (a(1)) to = get<std::uint32_t>(mem_, a(1));
            result=KernelSyncManager::instance().pthread_mutex_timedlock(it->second, to);
        }
        else result=KernelSyncManager::instance().pthread_mutex_lock(it->second);
        return static_cast<std::uint32_t>(result);
    }
    // Kernel semaphores
    if (n=="sceKernelCreateSema") {
        bool is_sdk = is_likely_string_ptr(mem_, a(0)) || a(0) == 0;
        std::string sema_name = "sema";
        std::uint32_t attr = SCE_KERNEL_SEMA_ATTR_TH_FIFO;
        std::int32_t init_val = 0, max_val = 1;
        std::uint64_t out_id_addr = 0;

        if (is_sdk) {
            if (a(0)) sema_name = read_guest_string(a(0), 32);
            attr = static_cast<std::uint32_t>(a(1));
            init_val = static_cast<std::int32_t>(a(2));
            max_val = static_cast<std::int32_t>(a(3));
        } else {
            out_id_addr = a(0);
            if (!out_id_addr) return invalid;
            if (a(1)) sema_name = read_guest_string(a(1), 32);
            if (a(3) != 0 && (a(3) >> 16) != 0) {
                init_val = static_cast<std::int32_t>(a(3) & 0xFFFF);
                max_val = static_cast<std::int32_t>((a(3) >> 16) & 0xFFFF);
            } else if (a(2) != 0 && (a(2) >> 16) != 0) {
                init_val = static_cast<std::int32_t>(a(2) & 0xFFFF);
                max_val = static_cast<std::int32_t>((a(2) >> 16) & 0xFFFF);
            } else if (a(3) != 0) {
                init_val = static_cast<std::int32_t>(a(2));
                max_val = static_cast<std::int32_t>(a(3));
            } else {
                init_val = static_cast<std::int32_t>(a(2));
                max_val = std::max<std::int32_t>(1, init_val);
            }
        }
        if (max_val <= 0) max_val = std::max<std::int32_t>(1, init_val);
        if (init_val < 0 || max_val <= 0 || init_val > max_val) return invalid;

        std::uint32_t id = 0;
        auto ret = KernelSyncManager::instance().create_sema(&id, sema_name.c_str(), attr, init_val, max_val);
        if (ret != ORBIS_OK) return static_cast<std::uint32_t>(ret);

        SemaRecord rec;
        rec.name = sema_name;
        rec.attr = attr;
        rec.init_count = init_val;
        rec.max_count = max_val;
        sema_records_[id] = rec;

        if (is_sdk) {
            return id;
        } else {
            put<std::uint32_t>(mem_, out_id_addr, id);
            return 0;
        }
    }
    if (n=="sceKernelWaitSema") {
        if (!a(0)) return invalid;
        std::int32_t need = a(1) ? static_cast<std::int32_t>(a(1)) : 1;
        std::uint32_t timeout=0, *ptimeout=nullptr;
        if (a(2)) {
            timeout=get<std::uint32_t>(mem_,a(2));
            ptimeout=&timeout;
        }
        std::int32_t res=KernelSyncManager::instance().wait_sema(static_cast<std::uint32_t>(a(0)), need, ptimeout);
        if (ptimeout && a(2)) put<std::uint32_t>(mem_,a(2),timeout);
        return static_cast<std::uint32_t>(res);
    }
    if (n=="sceKernelPollSema") {
        if (!a(0)) return invalid;
        std::int32_t need = a(1) ? static_cast<std::int32_t>(a(1)) : 1;
        return static_cast<std::uint32_t>(KernelSyncManager::instance().poll_sema(static_cast<std::uint32_t>(a(0)), need));
    }
    if (n=="sceKernelSignalSema") {
        if (!a(0)) return invalid;
        std::int32_t count = a(1) ? static_cast<std::int32_t>(a(1)) : 1;
        return static_cast<std::uint32_t>(KernelSyncManager::instance().signal_sema(static_cast<std::uint32_t>(a(0)), count));
    }
    if (n=="sceKernelDeleteSema") {
        if (!a(0)) return invalid;
        sema_records_.erase(static_cast<std::uint32_t>(a(0)));
        return static_cast<std::uint32_t>(KernelSyncManager::instance().delete_sema(static_cast<std::uint32_t>(a(0))));
    }
    if (n=="sceKernelGetSemaInfo") {
        if (!a(0) || !a(1)) return invalid;
        auto sid = static_cast<std::uint32_t>(a(0));
        auto it_rec = sema_records_.find(sid);
        if (it_rec == sema_records_.end()) return 0x80020003; // ESRCH
        std::int32_t cur_cnt = KernelSyncManager::instance().sema_count(sid);
        std::uint32_t num_waiters = static_cast<std::uint32_t>(KernelSyncManager::instance().sema_waiters(sid));
        SceKernelSemaInfo info{};
        std::uint64_t guest_size = 0;
        mem_.load(a(1), &guest_size, 8);
        info.size = (guest_size >= 48 && guest_size <= sizeof(SceKernelSemaInfo)) ? guest_size : sizeof(SceKernelSemaInfo);
        std::strncpy(info.name, it_rec->second.name.c_str(), sizeof(info.name) - 1);
        info.attr = it_rec->second.attr;
        info.init_count = it_rec->second.init_count;
        info.current_count = cur_cnt >= 0 ? cur_cnt : it_rec->second.init_count;
        info.max_count = it_rec->second.max_count;
        info.num_wait_threads = num_waiters;
        auto to_write = std::min<std::size_t>(sizeof(info), info.size);
        if (mem_.store(a(1), &info, to_write) != MemoryError::ok) return invalid;
        return 0;
    }
    // pthread thread attributes (scePthreadAttr*)
    if (n=="scePthreadAttrInit") {
        if (!a(0)) return invalid;
        auto h=next_handle_++; put(mem_,a(0),h); attr_defaults_[h]={}; return 0;
    }
    if (n=="scePthreadAttrDestroy") {
        attr_defaults_.erase(a(0)); return 0;
    }
    if (n=="scePthreadAttrSetstacksize") {
        if (!a(0) || !a(1)) return invalid;
        auto it=attr_defaults_.find(a(0)); if (it==attr_defaults_.end()) return invalid;
        it->second.stack_size=a(1); return 0;
    }
    if (n=="scePthreadAttrSetdetachstate") {
        if (!a(0)) return invalid;
        auto it=attr_defaults_.find(a(0)); if (it==attr_defaults_.end()) return invalid;
        it->second.detach_state=static_cast<std::uint32_t>(a(1)); return 0;
    }
    if (n=="scePthreadAttrSetschedpolicy") {
        if (!a(0)) return invalid;
        auto it=attr_defaults_.find(a(0)); if (it==attr_defaults_.end()) return invalid;
        it->second.sched_policy=static_cast<std::uint32_t>(a(1)); return 0;
    }
    if (n=="scePthreadAttrSetschedparam") {
        if (!a(0) || !a(1)) return invalid;
        auto it=attr_defaults_.find(a(0)); if (it==attr_defaults_.end()) return invalid;
        std::uint32_t param=0; if (mem_.load(a(1),&param,4)!=MemoryError::ok) return invalid;
        it->second.sched_param=param; return 0;
    }
    if (n=="scePthreadAttrSetaffinity") {
        if (!a(0) || !a(1)) return invalid;
        auto it=attr_defaults_.find(a(0)); if (it==attr_defaults_.end()) return invalid;
        std::uint64_t affinity=0; if (mem_.load(a(1),&affinity,8)!=MemoryError::ok) return invalid;
        it->second.affinity=affinity; return 0;
    }
    if (n=="scePthreadAttrSetinheritsched") {
        if (!a(0)) return invalid;
        auto it=attr_defaults_.find(a(0)); if (it==attr_defaults_.end()) return invalid;
        it->second.inherit_sched=static_cast<std::uint32_t>(a(1)); return 0;
    }
    if (n=="scePthreadAttrGet") {
        if (!a(0) || !a(1)) return invalid;
        auto it=attr_defaults_.find(a(0)); if (it==attr_defaults_.end()) return invalid;
        if (mem_.store(a(1),&it->second,sizeof(PthreadAttrDefaults))!=MemoryError::ok) return invalid;
        return 0;
    }
    if (n=="scePthreadAttrGetaffinity") {
        if (!a(0) || !a(1)) return invalid;
        auto it=attr_defaults_.find(a(0)); if (it==attr_defaults_.end()) return invalid;
        if (mem_.store(a(1),&it->second.affinity,8)!=MemoryError::ok) return invalid;
        return 0;
    }
    // pthread thread functions (stubs for non-scheduler mode)
    if (n=="scePthreadSelf") {
        return 1; // main thread ID
    }
    if (n=="scePthreadCreate") {
        // Simplified: allocate a thread ID and return success
        if (!a(0) || !a(2)) return invalid;
        std::uint64_t tid = next_handle_++;
        put<std::uint64_t>(mem_, a(0), tid);
        // Note: actual thread creation requires scheduler; returning success for now
        return 0;
    }
    if (n=="scePthreadExit") {
        // In non-scheduler mode, just return success
        return 0;
    }
    if (n=="scePthreadJoin") {
        // Simplified: return success immediately
        return 0;
    }
    if (n=="scePthreadDetach") {
        return 0;
    }
    if (n=="scePthreadYield") {
        return 0;
    }
    if (n=="scePthreadEqual") {
        // Compare two thread IDs
        return (a(0) == a(1)) ? 1 : 0;
    }
    if (n=="scePthreadKeyCreate") {
        if (!a(0)) return invalid;
        std::uint64_t key = next_handle_++;
        keys_[key] = a(1); // destructor
        put<std::uint32_t>(mem_, a(0), static_cast<std::uint32_t>(key));
        return 0;
    }
    if (n=="scePthreadKeyDelete") {
        keys_.erase(a(0));
        key_values_.erase(a(0));
        return 0;
    }
    if (n=="scePthreadSetspecific") {
        key_values_[a(0)] = a(1);
        return 0;
    }
    if (n=="scePthreadGetspecific") {
        auto it = key_values_.find(a(0));
        return it == key_values_.end() ? 0 : it->second;
    }
    if (n=="scePthreadGetprio") {
        if (!a(1)) return invalid;
        put<std::int32_t>(mem_, a(1), 0); // default priority
        return 0;
    }
    if (n=="scePthreadSetprio") {
        return 0;
    }
    if (n=="scePthreadSetaffinity") {
        return 0;
    }
    if (n=="scePthreadGetaffinity") {
        if (!a(1)) return invalid;
        put<std::uint64_t>(mem_, a(1), 1); // CPU 0
        return 0;
    }
    if (n=="scePthreadRename") {
        return 0;
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
    if (n=="scePthreadKeyCreate" || n=="pthread_key_create") {
        if (!a(0)) return invalid;
        const auto key=next_handle_++; keys_[key]=a(1); put<std::uint32_t>(mem_,a(0),static_cast<std::uint32_t>(key)); return 0;
    }
    if (n=="scePthreadKeyDelete" || n=="pthread_key_delete" ||
        n=="scePthreadSetspecific" || n=="pthread_setspecific" ||
        n=="scePthreadGetspecific" || n=="pthread_getspecific") {
        const auto key=a(0); if (!keys_.count(key)) return (n=="scePthreadGetspecific" || n=="pthread_getspecific") ? 0 : invalid;
        if (n=="scePthreadKeyDelete" || n=="pthread_key_delete") { keys_.erase(key); key_values_.erase(key); return 0; }
        if (n=="scePthreadSetspecific" || n=="pthread_setspecific") { key_values_[key]=a(1); return 0; }
        auto it=key_values_.find(key); return it==key_values_.end() ? 0 : it->second;
    }
    if (n=="scePthreadOnce" || n=="pthread_once") {
        if (!a(0) || !a(1)) return invalid;
        if (get<std::uint32_t>(mem_,a(0))) return 0;
        return 0x80020010; // exige invocação do callback guest; não marcar concluído
    }
    if (n=="sceKernelCreateEventFlag") {
        bool is_sdk = is_likely_string_ptr(mem_, a(0)) || a(0) == 0;
        if (is_sdk) {
            const auto h = next_handle_++;
            event_flags_[h] = a(2);
            return h;
        } else {
            if (!a(0) || a(2) > 3) return invalid;
            const auto h = next_handle_++;
            event_flags_[h] = a(3);
            put<std::uint32_t>(mem_, a(0), static_cast<std::uint32_t>(h));
            return 0;
        }
    }
    if (n=="sceKernelSetEventFlag" || n=="sceKernelClearEventFlag" || n=="sceKernelDeleteEventFlag" || n=="sceKernelPollEventFlag" || n=="sceKernelWaitEventFlag") {
        auto it=event_flags_.find(a(0)); if (it==event_flags_.end()) return invalid;
        if (n=="sceKernelDeleteEventFlag") { event_flags_.erase(it); return 0; }
        if (n=="sceKernelSetEventFlag") { it->second |= a(1); return 0; }
        if (n=="sceKernelClearEventFlag") { it->second &= ~a(1); return 0; }
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

    // vsnprintf - stub funcional básico
    if (n=="vsnprintf") {
        std::uint64_t dst = a(0);
        std::uint64_t max_size = a(1);
        (void)a(2); // fmt_addr
        // va_list is at a(3) - simplified: just return 0 for now
        if (dst && max_size > 0) {
            char null_byte = '\0';
            mem_.store(dst, &null_byte, 1);
        }
        return 0;
    }

    // String functions: strncpy, strcat, strncat
    if (n=="strncpy") {
        std::uint64_t dst = a(0);
        std::uint64_t src = a(1);
        std::uint64_t n_bytes = a(2);
        if (!dst || !src) return 0;
        for (std::uint64_t i = 0; i < n_bytes; ++i) {
            auto ch = get<std::uint8_t>(mem_, src + i);
            put<std::uint8_t>(mem_, dst + i, ch);
            if (!ch) {
                for (++i; i < n_bytes; ++i) put<std::uint8_t>(mem_, dst + i, 0);
                break;
            }
        }
        return dst;
    }
    if (n=="strcat" || n=="strncat") {
        std::uint64_t dst = a(0);
        std::uint64_t src = a(1);
        std::uint64_t max_append = (n=="strncat") ? a(2) : (1u<<20);
        if (!dst || !src) return dst;
        // Find end of dst
        std::uint64_t dst_len = 0;
        for (; dst_len < (1u<<20); ++dst_len) {
            if (!get<std::uint8_t>(mem_, dst + dst_len)) break;
        }
        for (std::uint64_t i = 0; i < max_append; ++i) {
            auto ch = get<std::uint8_t>(mem_, src + i);
            put<std::uint8_t>(mem_, dst + dst_len + i, ch);
            if (!ch) break;
        }
        return dst;
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

    // stdlib: exit, abort, abs, labs, llabs, div, ldiv, lldiv
    if (n=="exit" || n=="_exit") {
        // Em HLE, apenas registra e retorna - não termina o processo host
        exit_code_ = static_cast<int>(a(0));
        return 0;
    }
    if (n=="abort") {
        throw std::runtime_error("guest abort chamado");
    }
    if (n=="abs") {
        long val = static_cast<long>(a(0));
        return static_cast<std::uint64_t>(val >= 0 ? val : -val);
    }
    if (n=="labs") {
        long val = static_cast<long>(a(0));
        return static_cast<std::uint64_t>(val >= 0 ? val : -val);
    }
    if (n=="llabs") {
        long long val = static_cast<long long>(a(0));
        return static_cast<std::uint64_t>(val >= 0 ? val : -val);
    }
    if (n=="div") {
        long numer = static_cast<long>(a(0));
        long denom = static_cast<long>(a(1));
        if (denom == 0) return 0;
        std::uint64_t quot = static_cast<std::uint64_t>(numer / denom);
        std::uint64_t rem = static_cast<std::uint64_t>(numer % denom);
        if (a(2)) put<std::uint64_t>(mem_, a(2), quot); // quot in rdi
        if (a(3)) put<std::uint64_t>(mem_, a(3), rem); // rem in rsi
        return (quot << 32) | (rem & 0xFFFFFFFF);
    }
    if (n=="ldiv") {
        long numer = static_cast<long>(a(0));
        long denom = static_cast<long>(a(1));
        if (denom == 0) return 0;
        std::uint64_t quot = static_cast<std::uint64_t>(numer / denom);
        std::uint64_t rem = static_cast<std::uint64_t>(numer % denom);
        if (a(2)) put<std::uint64_t>(mem_, a(2), quot);
        if (a(3)) put<std::uint64_t>(mem_, a(3), rem);
        return (quot << 32) | (rem & 0xFFFFFFFF);
    }
    if (n=="lldiv") {
        long long numer = static_cast<long long>(a(0));
        long long denom = static_cast<long long>(a(1));
        if (denom == 0) return 0;
        std::uint64_t quot = static_cast<std::uint64_t>(numer / denom);
        std::uint64_t rem = static_cast<std::uint64_t>(numer % denom);
        if (a(2)) put<std::uint64_t>(mem_, a(2), quot);
        if (a(3)) put<std::uint64_t>(mem_, a(3), rem);
        return (quot << 32) | (rem & 0xFFFFFFFF);
    }

    // stdio: FILE* implementation with opaque handles and memory-safe transfers
    auto get_file = [&](std::uint64_t handle) -> FILE* {
        auto it = open_files_.find(handle);
        return it != open_files_.end() ? it->second : nullptr;
    };
    if (n=="fopen") {
        std::string path = read_guest_string(a(0));
        std::string mode = read_guest_string(a(1));
        if (path.empty() || mode.empty() || path.find("..") != std::string::npos) return 0;
        FILE* fp = std::fopen(path.c_str(), mode.c_str());
        if (!fp) return 0;
        std::uint64_t handle = next_handle_++;
        open_files_[handle] = fp;
        return handle;
    }
    if (n=="fclose") {
        auto it = open_files_.find(a(0));
        if (it == open_files_.end()) return static_cast<std::uint64_t>(-1);
        int res = std::fclose(it->second);
        open_files_.erase(it);
        return static_cast<std::uint64_t>(res);
    }
    if (n=="fread") {
        std::uint64_t dst = a(0);
        std::size_t size = static_cast<std::size_t>(a(1));
        std::size_t nmemb = static_cast<std::size_t>(a(2));
        FILE* fp = get_file(a(3));
        if (!fp || !dst || !size || !nmemb) return 0;
        constexpr std::size_t max_transfer = 64ull * 1024 * 1024;
        if (nmemb > max_transfer / size) return 0;
        std::size_t total = size * nmemb;
        std::vector<std::uint8_t> tmp(total);
        std::size_t read_bytes = std::fread(tmp.data(), 1, total, fp);
        if (read_bytes > 0) {
            if (mem_.store(dst, tmp.data(), read_bytes) != MemoryError::ok) return 0;
        }
        return read_bytes / size;
    }
    if (n=="fwrite") {
        std::uint64_t src = a(0);
        std::size_t size = static_cast<std::size_t>(a(1));
        std::size_t nmemb = static_cast<std::size_t>(a(2));
        FILE* fp = get_file(a(3));
        if (!fp || !src || !size || !nmemb) return 0;
        constexpr std::size_t max_transfer = 64ull * 1024 * 1024;
        if (nmemb > max_transfer / size) return 0;
        std::size_t total = size * nmemb;
        std::vector<std::uint8_t> tmp(total);
        if (mem_.load(src, tmp.data(), total) != MemoryError::ok) return 0;
        std::size_t written_bytes = std::fwrite(tmp.data(), 1, total, fp);
        return written_bytes / size;
    }
    if (n=="fseek") {
        FILE* fp = get_file(a(0));
        if (!fp) return static_cast<std::uint64_t>(-1);
        long offset = static_cast<long>(a(1));
        int whence = static_cast<int>(a(2));
        return std::fseek(fp, offset, whence);
    }
    if (n=="ftell") {
        FILE* fp = get_file(a(0));
        if (!fp) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(std::ftell(fp));
    }
    if (n=="rewind") {
        FILE* fp = get_file(a(0));
        if (fp) std::rewind(fp);
        return 0;
    }
    if (n=="fflush") {
        FILE* fp = get_file(a(0));
        return fp ? std::fflush(fp) : 0;
    }
    if (n=="feof") {
        FILE* fp = get_file(a(0));
        return fp ? std::feof(fp) : 1;
    }
    if (n=="ferror") {
        FILE* fp = get_file(a(0));
        return fp ? std::ferror(fp) : 1;
    }
    if (n=="clearerr") {
        FILE* fp = get_file(a(0));
        if (fp) std::clearerr(fp);
        return 0;
    }
    if (n=="fprintf" || n=="vfprintf") {
        FILE* fp = get_file(a(0));
        if (!fp) return 0;
        std::string fmt = read_guest_string(a(1));
        // Simplified: just print the format string without args
        if (fmt.find('%') != std::string::npos) return 0;
        return std::fprintf(fp, "%s", fmt.c_str());
    }
    if (n=="sprintf" || n=="vsprintf") {
        std::uint64_t dst = a(0);
        std::string fmt = read_guest_string(a(1));
        if (fmt.find('%') != std::string::npos) return 0;
        if (dst) {
            mem_.store(dst, fmt.data(), fmt.size());
            char null_byte = '\0';
            mem_.store(dst + fmt.size(), &null_byte, 1);
        }
        return fmt.size();
    }
    if (n=="puts") {
        std::string s = read_guest_string(a(0));
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fputc('\n', stdout);
        return static_cast<int>(s.size()) + 1;
    }
    if (n=="putchar") {
        int c = static_cast<int>(a(0));
        return std::fputc(c, stdout);
    }
    if (n=="getchar") {
        return std::getchar();
    }
    if (n=="fgetc") {
        FILE* fp = get_file(a(0));
        return fp ? std::fgetc(fp) : EOF;
    }
    if (n=="fgets") {
        std::uint64_t dst = a(0);
        int size = static_cast<int>(a(1));
        FILE* fp = get_file(a(2));
        if (!dst || size <= 1 || !fp) return 0;
        if (size > 65536) size = 65536;
        std::vector<char> tmp(size);
        char* res = std::fgets(tmp.data(), size, fp);
        if (!res) return 0;
        std::size_t len = std::strlen(res) + 1;
        if (mem_.store(dst, res, len) != MemoryError::ok) return 0;
        return dst;
    }
    if (n=="fputc") {
        int c = static_cast<int>(a(0));
        FILE* fp = get_file(a(1));
        return fp ? std::fputc(c, fp) : EOF;
    }
    if (n=="fputs") {
        std::string s = read_guest_string(a(0));
        FILE* fp = get_file(a(1));
        return fp ? std::fputs(s.c_str(), fp) : EOF;
    }

    // pthreads stubs
    if (n=="pthread_self") {
        return reinterpret_cast<std::uint64_t>(&mem_); // unique-ish per instance
    }
    if (n=="pthread_join") {
        // Stub: return success without waiting
        return 0;
    }
    if (n=="pthread_exit") {
        // Stub: do nothing
        return 0;
    }
    if (n=="pthread_attr_init") {
        std::uint64_t attr = a(0);
        if (!attr) return 0x16; // EINVAL
        put<std::uint32_t>(mem_, attr, 0); // default attr
        return 0;
    }
    if (n=="pthread_attr_destroy") {
        std::uint64_t attr = a(0);
        if (!attr) return 0x16;
        put<std::uint32_t>(mem_, attr, 0xDEAD);
        return 0;
    }
    if (n=="pthread_attr_setdetachstate") {
        std::uint64_t attr = a(0);
        int state = static_cast<int>(a(1));
        if (!attr) return 0x16;
        // Store detach state in attr (simplified)
        put<std::uint32_t>(mem_, attr + 4, static_cast<std::uint32_t>(state));
        return 0;
    }
    if (n=="pthread_attr_getdetachstate") {
        std::uint64_t attr = a(0);
        std::uint64_t state_ptr = a(1);
        if (!attr || !state_ptr) return 0x16;
        std::uint32_t state = get<std::uint32_t>(mem_, attr + 4);
        put<std::uint32_t>(mem_, state_ptr, state);
        return 0;
    }

    // signals stubs
    if (n=="signal") {
        // Return SIG_DFL (0) as previous handler
        return 0;
    }
    if (n=="raise") {
        // Stub: do nothing
        return 0;
    }
    if (n=="sigaction") {
        // Stub: return success
        return 0;
    }
    if (n=="sigemptyset") {
        std::uint64_t set = a(0);
        if (set) put<std::uint64_t>(mem_, set, 0);
        return 0;
    }
    if (n=="sigfillset") {
        std::uint64_t set = a(0);
        if (set) put<std::uint64_t>(mem_, set, ~0ULL);
        return 0;
    }
    if (n=="sigaddset") {
        // Stub: return success
        return 0;
    }
    if (n=="sigdelset") {
        // Stub: return success
        return 0;
    }
    if (n=="sigismember") {
        // Stub: return 0 (not a member)
        return 0;
    }

    // time functions
    if (n=="time") {
        std::time_t t = std::time(nullptr);
        if (a(0)) put<std::int64_t>(mem_, a(0), t);
        return static_cast<std::uint64_t>(t);
    }
    if (n=="clock") {
        return static_cast<std::uint64_t>(std::clock());
    }
    if (n=="difftime") {
        double time1 = 0, time2 = 0;
        std::uint64_t a0 = a(0);
        std::uint64_t a1 = a(1);
        std::memcpy(&time1, &a0, sizeof(time1));
        std::memcpy(&time2, &a1, sizeof(time2));
        double diff = time1 - time2;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &diff, sizeof(bits));
        return bits;
    }
    if (n=="mktime") {
        // Stub: return 0
        return 0;
    }
    if (n=="localtime") {
        // Stub: return a static pointer (simplified)
        std::time_t t = static_cast<std::time_t>(a(0));
        std::tm* tm = std::localtime(&t);
        // Allocate a simple struct in guest memory
        std::uint64_t buf = next_heap_;
        next_heap_ += 128;
        mem_.map(buf, 128, read | write);
        if (tm) {
            put<std::int32_t>(mem_, buf + 0, tm->tm_sec);
            put<std::int32_t>(mem_, buf + 4, tm->tm_min);
            put<std::int32_t>(mem_, buf + 8, tm->tm_hour);
            put<std::int32_t>(mem_, buf + 12, tm->tm_mday);
            put<std::int32_t>(mem_, buf + 16, tm->tm_mon);
            put<std::int32_t>(mem_, buf + 20, tm->tm_year);
            put<std::int32_t>(mem_, buf + 24, tm->tm_wday);
            put<std::int32_t>(mem_, buf + 28, tm->tm_yday);
            put<std::int32_t>(mem_, buf + 32, tm->tm_isdst);
        }
        return buf;
    }
    if (n=="gmtime") {
        std::time_t t = static_cast<std::time_t>(a(0));
        std::tm* tm = std::gmtime(&t);
        std::uint64_t buf = next_heap_;
        next_heap_ += 128;
        mem_.map(buf, 128, read | write);
        if (tm) {
            put<std::int32_t>(mem_, buf + 0, tm->tm_sec);
            put<std::int32_t>(mem_, buf + 4, tm->tm_min);
            put<std::int32_t>(mem_, buf + 8, tm->tm_hour);
            put<std::int32_t>(mem_, buf + 12, tm->tm_mday);
            put<std::int32_t>(mem_, buf + 16, tm->tm_mon);
            put<std::int32_t>(mem_, buf + 20, tm->tm_year);
            put<std::int32_t>(mem_, buf + 24, tm->tm_wday);
            put<std::int32_t>(mem_, buf + 28, tm->tm_yday);
            put<std::int32_t>(mem_, buf + 32, tm->tm_isdst);
        }
        return buf;
    }
    if (n=="asctime") {
        std::uint64_t tm_ptr = a(0);
        if (!tm_ptr) return 0;
        std::int32_t sec = get<std::int32_t>(mem_, tm_ptr + 0);
        std::int32_t min = get<std::int32_t>(mem_, tm_ptr + 4);
        std::int32_t hour = get<std::int32_t>(mem_, tm_ptr + 8);
        std::int32_t mday = get<std::int32_t>(mem_, tm_ptr + 12);
        std::int32_t mon = get<std::int32_t>(mem_, tm_ptr + 16);
        std::int32_t year = get<std::int32_t>(mem_, tm_ptr + 20);
        static const char* wday_name[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char* mon_name[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        std::uint64_t buf = next_heap_;
        next_heap_ += 64;
        mem_.map(buf, 64, read | write);
        char out[64];
        std::snprintf(out, sizeof(out), "%.3s %.3s %2d %.2d:%.2d:%.2d %d\n",
            wday_name[0], mon_name[mon % 12], mday, hour, min, sec, 1900 + year);
        mem_.store(buf, out, std::strlen(out) + 1);
        return buf;
    }
    if (n=="ctime") {
        std::time_t t = static_cast<std::time_t>(a(0));
        std::string s = std::ctime(&t);
        std::uint64_t buf = next_heap_;
        next_heap_ += s.size() + 1;
        mem_.map(buf, s.size() + 1, read | write);
        mem_.store(buf, s.data(), s.size() + 1);
        return buf;
    }
    if (n=="strftime") {
        // Stub: return 0
        return 0;
    }
    if (n=="nanosleep") {
        // Stub: return 0 (slept)
        return 0;
    }

    // Other common libc functions
    if (n=="memalign") {
        std::size_t alignment = static_cast<std::size_t>(a(0));
        std::size_t size = static_cast<std::size_t>(a(1));
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) return 0;
        CpuState copy = c; copy.gpr[7] = size;
        std::uint64_t ptr = handle("malloc", copy);
        if (ptr && (ptr % alignment) != 0) {
            // Align up
            ptr = (ptr + alignment - 1) & ~(alignment - 1);
        }
        return ptr;
    }
    if (n=="memchr") {
        std::uint64_t ptr = a(0);
        int val = static_cast<int>(a(1));
        std::size_t n_bytes = static_cast<std::size_t>(a(2));
        for (std::size_t i = 0; i < n_bytes; ++i) {
            auto ch = get<std::uint8_t>(mem_, ptr + i);
            if (ch == static_cast<std::uint8_t>(val)) return ptr + i;
        }
        return 0;
    }
    if (n=="getpagesize") {
        return 4096;
    }
    if (n=="remove") {
        std::string path = read_guest_string(a(0));
        if (path.empty() || path.find("..") != std::string::npos) return static_cast<std::uint64_t>(-1);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return ec ? static_cast<std::uint64_t>(-1) : 0;
    }
    if (n=="rename") {
        std::string oldpath = read_guest_string(a(0));
        std::string newpath = read_guest_string(a(1));
        if (oldpath.empty() || newpath.empty() || oldpath.find("..") != std::string::npos || newpath.find("..") != std::string::npos)
            return static_cast<std::uint64_t>(-1);
        std::error_code ec;
        std::filesystem::rename(oldpath, newpath, ec);
        return ec ? static_cast<std::uint64_t>(-1) : 0;
    }
    if (n=="mkdir") {
        std::string path = read_guest_string(a(0));
        if (path.empty() || path.find("..") != std::string::npos) return static_cast<std::uint64_t>(-1);
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return ec ? static_cast<std::uint64_t>(-1) : 0;
    }
    if (n=="rmdir") {
        std::string path = read_guest_string(a(0));
        if (path.empty() || path.find("..") != std::string::npos) return static_cast<std::uint64_t>(-1);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return ec ? static_cast<std::uint64_t>(-1) : 0;
    }
    if (n=="fgetpos") {
        FILE* fp = get_file(a(0));
        if (!fp) return static_cast<std::uint64_t>(-1);
        std::fpos_t pos;
        int res = std::fgetpos(fp, &pos);
        if (res == 0 && a(1)) put<std::uint64_t>(mem_, a(1), static_cast<std::uint64_t>(pos));
        return res;
    }
    if (n=="fsetpos") {
        FILE* fp = get_file(a(0));
        if (!fp) return static_cast<std::uint64_t>(-1);
        std::fpos_t pos = static_cast<std::fpos_t>(a(1));
        return std::fsetpos(fp, &pos);
    }
    if (n=="setvbuf") {
        // Stub: return success
        return 0;
    }
    if (n=="freopen") {
        std::string path = read_guest_string(a(0));
        std::string mode = read_guest_string(a(1));
        auto it = open_files_.find(a(2));
        if (it == open_files_.end() || path.empty() || path.find("..") != std::string::npos) return 0;
        FILE* res = std::freopen(path.c_str(), mode.c_str(), it->second);
        if (!res) {
            open_files_.erase(it);
            return 0;
        }
        it->second = res;
        return a(2);
    }
    if (n=="frexp" || n=="frexpf") {
        // Stub: return 0.5, exponent in a(1)
        double x = 0.0;
        std::uint64_t arg0 = a(0);
        if (n=="frexp") std::memcpy(&x, &arg0, sizeof(x));
        else {
            float xf = 0.0f;
            std::memcpy(&xf, &arg0, sizeof(xf));
            x = xf;
        }
        int exp = 0;
        double frac = std::frexp(x, &exp);
        if (a(1)) put<std::int32_t>(mem_, a(1), exp);
        std::uint64_t bits = 0;
        if (n=="frexp") std::memcpy(&bits, &frac, sizeof(bits));
        else {
            float ffrac = static_cast<float>(frac);
            std::memcpy(&bits, &ffrac, sizeof(ffrac));
        }
        return bits;
    }
    if (n=="ldexp" || n=="ldexpf") {
        double x = 0.0;
        std::uint64_t arg0 = a(0);
        if (n=="ldexp") std::memcpy(&x, &arg0, sizeof(x));
        else {
            float xf = 0.0f;
            std::memcpy(&xf, &arg0, sizeof(xf));
            x = xf;
        }
        int exp = static_cast<int>(a(1));
        double res = std::ldexp(x, exp);
        std::uint64_t bits = 0;
        if (n=="ldexp") std::memcpy(&bits, &res, sizeof(bits));
        else {
            float fres = static_cast<float>(res);
            std::memcpy(&bits, &fres, sizeof(fres));
        }
        return bits;
    }
    if (n=="modf" || n=="modff") {
        double x = 0.0;
        std::uint64_t arg0 = a(0);
        if (n=="modf") std::memcpy(&x, &arg0, sizeof(x));
        else {
            float xf = 0.0f;
            std::memcpy(&xf, &arg0, sizeof(xf));
            x = xf;
        }
        double intpart = 0.0;
        double frac = std::modf(x, &intpart);
        if (a(1)) {
            std::uint64_t bits = 0;
            if (n=="modf") std::memcpy(&bits, &intpart, sizeof(bits));
            else {
                float fintpart = static_cast<float>(intpart);
                std::memcpy(&bits, &fintpart, sizeof(fintpart));
            }
            put<std::uint64_t>(mem_, a(1), bits);
        }
        std::uint64_t bits = 0;
        if (n=="modf") std::memcpy(&bits, &frac, sizeof(bits));
        else {
            float ffrac = static_cast<float>(frac);
            std::memcpy(&bits, &ffrac, sizeof(ffrac));
        }
        return bits;
    }
    if (n=="exp2" || n=="exp2f") {
        double x = 0.0;
        std::uint64_t arg0 = a(0);
        if (n=="exp2") std::memcpy(&x, &arg0, sizeof(x));
        else {
            float xf = 0.0f;
            std::memcpy(&xf, &arg0, sizeof(xf));
            x = xf;
        }
        double res = std::exp2(x);
        std::uint64_t bits = 0;
        if (n=="exp2") std::memcpy(&bits, &res, sizeof(bits));
        else {
            float fres = static_cast<float>(res);
            std::memcpy(&bits, &fres, sizeof(fres));
        }
        return bits;
    }
    if (n=="acos" || n=="acosf") {
        double x = 0.0;
        std::uint64_t arg0 = a(0);
        if (n=="acos") std::memcpy(&x, &arg0, sizeof(x));
        else {
            float xf = 0.0f;
            std::memcpy(&xf, &arg0, sizeof(xf));
            x = xf;
        }
        double res = std::acos(x);
        std::uint64_t bits = 0;
        if (n=="acos") std::memcpy(&bits, &res, sizeof(bits));
        else {
            float fres = static_cast<float>(res);
            std::memcpy(&bits, &fres, sizeof(fres));
        }
        return bits;
    }
    if (n=="asin" || n=="asinf") {
        double x = 0.0;
        std::uint64_t arg0 = a(0);
        if (n=="asin") std::memcpy(&x, &arg0, sizeof(x));
        else {
            float xf = 0.0f;
            std::memcpy(&xf, &arg0, sizeof(xf));
            x = xf;
        }
        double res = std::asin(x);
        std::uint64_t bits = 0;
        if (n=="asin") std::memcpy(&bits, &res, sizeof(bits));
        else {
            float fres = static_cast<float>(res);
            std::memcpy(&bits, &fres, sizeof(fres));
        }
        return bits;
    }
    if (n=="atan" || n=="atanf") {
        double x = 0.0;
        std::uint64_t arg0 = a(0);
        if (n=="atan") std::memcpy(&x, &arg0, sizeof(x));
        else {
            float xf = 0.0f;
            std::memcpy(&xf, &arg0, sizeof(xf));
            x = xf;
        }
        double res = std::atan(x);
        std::uint64_t bits = 0;
        if (n=="atan") std::memcpy(&bits, &res, sizeof(bits));
        else {
            float fres = static_cast<float>(res);
            std::memcpy(&bits, &fres, sizeof(fres));
        }
        return bits;
    }
    if (n=="tanhf") {
        float x = 0.0f;
        std::uint64_t arg0 = a(0);
        std::memcpy(&x, &arg0, sizeof(x));
        float res = std::tanh(x);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &res, sizeof(bits));
        return bits;
    }
    if (n=="srand") {
        std::srand(static_cast<unsigned>(a(0)));
        return 0;
    }
    if (n=="rand") {
        return std::rand();
    }
    if (n=="gettimeofday") {
        if (a(0)) {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(now);
            auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(now - secs);
            put<std::int64_t>(mem_, a(0), secs.count());
            put<std::int64_t>(mem_, a(0) + 8, usecs.count());
        }
        return 0;
    }
    if (n=="localtime_s") {
        // Stub
        return 0;
    }
    if (n=="strerror") {
        int errnum = static_cast<int>(a(0));
        std::string msg = std::strerror(errnum);
        std::uint64_t buf = next_heap_;
        next_heap_ += msg.size() + 1;
        mem_.map(buf, msg.size() + 1, read | write);
        mem_.store(buf, msg.data(), msg.size() + 1);
        return buf;
    }
    if (n=="strcoll") {
        // Use strcmp for now
        return handle("strcmp", c);
    }
    if (n=="strcspn" || n=="strspn" || n=="strpbrk" || n=="strtok") {
        // Stubs
        return 0;
    }
    if (n=="setlocale") {
        // Stub
        return 0;
    }
    if (n=="perror") {
        // Stub
        return 0;
    }
    if (n=="fscanf" || n=="sscanf" || n=="swscanf") {
        // Stubs
        return 0;
    }
    if (n=="fgetpos") {
        // Already handled above
    }
    if (n=="ungetc") {
        int c = static_cast<int>(a(0));
        FILE* fp = reinterpret_cast<FILE*>(a(1));
        return std::ungetc(c, fp);
    }
    if (n=="fscanf") {
        // Stub
        return 0;
    }
    if (n=="setvbuf") {
        // Stub
        return 0;
    }
    if (n=="freopen") {
        // Already handled above
    }
    if (n=="getc") {
        FILE* fp = reinterpret_cast<FILE*>(a(0));
        return std::getc(fp);
    }
    if (n=="fputc") {
        // Already handled above
    }
    if (n=="putchar") {
        // Already handled above
    }
    if (n=="fgets") {
        // Already handled above
    }
    if (n=="fputs") {
        // Already handled above
    }
    if (n=="feof") {
        // Already handled above
    }
    if (n=="ferror") {
        // Already handled above
    }
    if (n=="clearerr") {
        // Already handled above
    }
    if (n=="fileno") {
        FILE* fp = get_file(a(0));
#if defined(_WIN32)
        return fp ? static_cast<std::uint64_t>(_fileno(fp)) : static_cast<std::uint64_t>(-1);
#else
        return fp ? static_cast<std::uint64_t>(fileno(fp)) : static_cast<std::uint64_t>(-1);
#endif
    }
    if (n=="isatty") {
        // Stub
        return 0;
    }
    if (n=="ttyname") {
        // Stub
        return 0;
    }
    if (n=="ctermid") {
        // Stub
        return 0;
    }
    if (n=="tmpnam") {
        // Stub
        return 0;
    }
    if (n=="tmpfile") {
        // Stub
        return 0;
    }
    if (n=="fstat" || n=="stat") {
        // Stub
        return 0;
    }
    if (n=="lstat") {
        // Stub
        return 0;
    }
    if (n=="access") {
        // Stub
        return 0;
    }
    if (n=="chmod" || n=="fchmod") {
        // Stub
        return 0;
    }
    if (n=="chown" || n=="fchown" || n=="lchown") {
        // Stub
        return 0;
    }
    if (n=="link" || n=="symlink" || n=="readlink") {
        // Stub
        return 0;
    }
    if (n=="unlink") {
        std::string path = read_guest_string(a(0));
        if (path.empty() || path.find("..") != std::string::npos) return static_cast<std::uint64_t>(-1);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return ec ? static_cast<std::uint64_t>(-1) : 0;
    }
    // POSIX I/O functions (libc low-level)
    if (n=="_write" || n=="write") {
        int fd = static_cast<int>(a(0));
        std::uint64_t buf = a(1);
        std::size_t count = static_cast<std::size_t>(a(2));
        if (count > 1024*1024) count = 1024*1024;
        std::vector<char> data(count);
        if (mem_.load(buf, data.data(), count) != MemoryError::ok) return static_cast<std::uint64_t>(-1);
        if (fd == 1 || fd == 2) {
            std::fwrite(data.data(), 1, count, fd == 1 ? stdout : stderr);
            return count;
        }
        return static_cast<std::uint64_t>(::write(fd, data.data(), static_cast<unsigned int>(count)));
    }
    if (n=="_read" || n=="read") {
        int fd = static_cast<int>(a(0));
        std::uint64_t buf = a(1);
        std::size_t count = static_cast<std::size_t>(a(2));
        if (count > 1024*1024) count = 1024*1024;
        std::vector<char> data(count);
#if defined(_WIN32)
        int res = ::read(fd, data.data(), static_cast<unsigned int>(count));
#else
        ssize_t res = ::read(fd, data.data(), count);
#endif
        if (res > 0 && mem_.store(buf, data.data(), static_cast<std::size_t>(res)) != MemoryError::ok) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(res);
    }
    if (n=="_open" || n=="open") {
        std::string path = read_guest_string(a(0));
        int oflag = static_cast<int>(a(1));
        int mode = static_cast<int>(a(2));
        int fd = ::open(path.c_str(), oflag, mode);
        return static_cast<std::uint64_t>(fd);
    }
    if (n=="_close" || n=="close") {
        int fd = static_cast<int>(a(0));
        return static_cast<std::uint64_t>(::close(fd));
    }
    if (n=="_lseek" || n=="lseek") {
        int fd = static_cast<int>(a(0));
#if defined(_WIN32)
        long offset = static_cast<long>(a(1));
#else
        off_t offset = static_cast<off_t>(a(1));
#endif
        int whence = static_cast<int>(a(2));
        return static_cast<std::uint64_t>(::lseek(fd, offset, whence));
    }
    if (n=="getcwd") {
        // Stub
        return 0;
    }
    if (n=="chdir" || n=="fchdir") {
        // Stub
        return 0;
    }
    if (n=="opendir" || n=="readdir" || n=="closedir") {
        // Stub
        return 0;
    }
    if (n=="getpid") {
#if defined(_WIN32)
        return static_cast<std::uint64_t>(_getpid());
#else
        return static_cast<std::uint64_t>(getpid());
#endif
    }
    if (n=="getppid") {
        return 1;
    }
    if (n=="getuid" || n=="geteuid" || n=="getgid" || n=="getegid") {
        return 0;
    }
    if (n=="sysctl") {
        // Stub
        return 0;
    }
    if (n=="mmap" || n=="munmap") {
        // Use sceKernelMmap/sceKernelMunmap
        return handle(n=="mmap" ? "sceKernelMmap" : "sceKernelMunmap", c);
    }
    if (n=="msync") {
        return 0;
    }
    if (n=="mlock" || n=="munlock" || n=="mlockall" || n=="munlockall") {
        return 0;
    }
    if (n=="sched_yield") {
        return 0;
    }
    if (n=="sleep") {
        // Stub
        return 0;
    }
    if (n=="usleep") {
        return 0;
    }
    if (n=="getrusage") {
        // Stub
        return 0;
    }
    if (n=="srand48" || n=="drand48" || n=="lrand48" || n=="mrand48") {
        // Stubs
        return 0;
    }
    if (n=="qsort" || n=="bsearch") {
        // Already implemented above
    }

    std::printf("HLE handle: unhandled import: %s\n", n.c_str());
    throw std::runtime_error("import não implementado: " + n);
}
} // namespace bb
