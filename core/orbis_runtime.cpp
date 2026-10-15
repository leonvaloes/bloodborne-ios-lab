#include "orbis_runtime.h"
#include "kernel_threads.h"
#include "dialog_hle.h"
#include "gnm_context.h"
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
constexpr std::uint64_t page = 16384, pool = 5056ULL << 20, logical_limit = 6ULL << 30;
constexpr std::uint64_t invalid = 0x80020016, no_memory = 0x8002000c, no_space = 0x80020023;
std::uint64_t align(std::uint64_t value, std::uint64_t unit) { return (value + unit - 1) & ~(unit - 1); }
bool valid_alignment(std::uint64_t a) { return a >= page && a <= pool && !(a & (a - 1)); }

FILE* portable_fopen(const std::string& path, const char* mode) {
#if defined(_WIN32)
    std::filesystem::path p = std::filesystem::u8path(path);
    std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(p.c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}
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
    : mem_(memory), user_mgr_(), pad_mgr_(nullptr, &user_mgr_), gnm_context_(std::make_unique<GnmContext>(memory)) {
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
    names_.emplace("H2e8t5ScQGc", "__cxa_finalize");
    names_.emplace("5bBacGLyLOs", "gmtime_s");
    names_.emplace("ioKMruft1ek", "sceDiscMapUnknownIoKM");
}
OrbisRuntime::~OrbisRuntime() {
    for (auto& pair : kernel_files_) {
        if (pair.second.fp) std::fclose(pair.second.fp);
    }
    kernel_files_.clear();
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
std::uint64_t OrbisRuntime::get_errno_addr(GuestScheduler::Id tid) {
    if (!errno_page_) {
        errno_page_ = next_virtual_;
        next_virtual_ += 0x10000;
        auto err = mem_.map(errno_page_, 0x10000, read | write);
        if (err != MemoryError::ok) {
            std::printf("[HLE] failed to map errno page at 0x%llx\n", (unsigned long long)errno_page_);
        }
        mappings_[errno_page_] = 0x10000;
    }
    const std::uint64_t offset = (static_cast<std::uint64_t>(tid) % 1024) * 64;
    return errno_page_ + offset;
}
OrbisRuntime::PthreadAttrDefaults* OrbisRuntime::find_attr(std::uint64_t slot_addr) {
    if (!slot_addr) return nullptr;
    auto it = attr_defaults_.find(slot_addr);
    if (it != attr_defaults_.end()) return &it->second;
    std::uint64_t h = 0;
    if (mem_.load(slot_addr, &h, 8) == MemoryError::ok && h != 0) {
        it = attr_defaults_.find(h);
        if (it != attr_defaults_.end()) return &it->second;
    }
    return &attr_defaults_[slot_addr];
}

#pragma pack(push, 1)
struct SceKernelStat {
    std::uint32_t st_dev;
    std::uint32_t st_ino;
    std::uint16_t st_mode;
    std::uint16_t st_nlink;
    std::uint32_t st_uid;
    std::uint32_t st_gid;
    std::uint32_t st_rdev;
    std::int64_t  st_atime_sec;
    std::int64_t  st_atime_nsec;
    std::int64_t  st_mtime_sec;
    std::int64_t  st_mtime_nsec;
    std::int64_t  st_ctime_sec;
    std::int64_t  st_ctime_nsec;
    std::int64_t  st_size;
    std::int64_t  st_blocks;
    std::uint32_t st_blksize;
    std::uint32_t st_flags;
    std::uint32_t st_gen;
    std::int32_t  st_lspare;
    std::int64_t  st_birthtime_sec;
    std::int64_t  st_birthtime_nsec;
};
#pragma pack(pop)
static_assert(sizeof(SceKernelStat) == 120, "SceKernelStat size must be 120 bytes");

std::string OrbisRuntime::resolve_guest_path(const std::string& guest_path) const {
    if (guest_path.empty()) return "";
    std::string norm = guest_path;
    for (char& c : norm) {
        if (c == '\\') c = '/';
    }
    std::string rel;
    if (norm.rfind("/app0/", 0) == 0) {
        rel = norm.substr(6);
    } else if (norm.rfind("app0:/", 0) == 0) {
        rel = norm.substr(7);
    } else if (norm.rfind("app0:", 0) == 0) {
        rel = norm.substr(5);
    } else if (norm == "/app0" || norm == "app0") {
        rel = "";
    } else if (norm.rfind("/app0", 0) == 0) {
        rel = norm.substr(5);
    } else if (norm.rfind("/temp/", 0) == 0 || norm.rfind("/tmp/", 0) == 0) {
        auto tmp_base = std::filesystem::temp_directory_path() / "antigravity";
        std::error_code ec;
        std::filesystem::create_directories(tmp_base, ec);
        std::string sub = (norm.rfind("/temp/", 0) == 0) ? norm.substr(6) : norm.substr(5);
        return (tmp_base / sub).lexically_normal().string();
    } else if (norm.rfind("/savedata/", 0) == 0 || norm.rfind("savedata:/", 0) == 0) {
        auto save_base = app0_dir_.empty() ? (std::filesystem::current_path() / "savedata")
                                           : (app0_dir_.parent_path() / "savedata");
        std::error_code ec;
        std::filesystem::create_directories(save_base, ec);
        std::string sub = (norm.rfind("/savedata/", 0) == 0) ? norm.substr(10) : norm.substr(11);
        return (save_base / sub).lexically_normal().string();
    } else if (norm[0] == '/') {
        std::error_code ec;
        std::filesystem::path p(norm);
        if (app0_dir_.empty() || std::filesystem::exists(p, ec) || std::filesystem::exists(p.parent_path(), ec)) {
            return p.lexically_normal().string();
        }
        rel = norm.substr(1);
    } else {
        rel = norm;
    }

    if (!app0_dir_.empty()) {
        auto cand = (app0_dir_ / rel).lexically_normal();
        std::error_code ec;
        if (std::filesystem::exists(cand, ec)) {
            return cand.string();
        }
        if (std::filesystem::exists(rel, ec)) {
            return std::filesystem::path(rel).lexically_normal().string();
        }
        return cand.string();
    }
    return rel;
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

    if (n == "scePthreadCreate" || n == "pthread_create" || n == "pthread_create_name_np") {
        std::size_t thread_stack_size = (mem_.budget() > (16ULL << 20)) ? (2 * 1024 * 1024) : 0x10000;
        if (a(1)) {
            auto* at = find_attr(a(1));
            if (at && at->stack_size >= 0x10000 && at->stack_size <= (mem_.budget() / 2)) {
                thread_stack_size = static_cast<std::size_t>(at->stack_size);
            }
        }
        if (!a(0) || !a(2) || !return_trap || thread_stack_next_ > UINT64_MAX - thread_stack_size) {
            std::printf("[PTHREAD] invalid args in scePthreadCreate: a0=0x%llx a2=0x%llx ret_trap=0x%llx\n",
                        (unsigned long long)a(0), (unsigned long long)a(2), (unsigned long long)return_trap);
            return result(invalid);
        }
        const auto bottom = thread_stack_next_;
        auto err1 = mem_.map(bottom, thread_stack_size, read | write);
        if (err1 != MemoryError::ok) {
            std::printf("[PTHREAD] mem_.map stack bottom=0x%llx sz=0x%zx failed: err=%d\n",
                        (unsigned long long)bottom, thread_stack_size, (int)err1);
            return result(no_memory);
        }

        std::uint64_t main_tls_size = tls_template_.memsz ? (tls_template_.memsz + 15) & ~15ULL : 0x1000;
        if (main_tls_size < 0x1000) main_tls_size = 0x1000;
        const std::uint64_t thread_tls_region = 0x10000; // 64 KB for TLS + TCB
        std::uint64_t thread_tls_base = next_virtual_;
        next_virtual_ += thread_tls_region;
        auto err2 = mem_.map(thread_tls_base, thread_tls_region, read | write);
        if (err2 != MemoryError::ok) {
            std::printf("[PTHREAD] mem_.map tls base=0x%llx failed: err=%d\n", (unsigned long long)thread_tls_base, (int)err2);
            mem_.unmap(bottom);
            return result(no_memory);
        }
        mappings_[thread_tls_base] = thread_tls_region;

        std::uint64_t thread_tcb = thread_tls_base + main_tls_size;
        if (tls_template_.filesz && tls_template_.address) {
            std::vector<std::uint8_t> tls_data(static_cast<std::size_t>(tls_template_.filesz));
            mem_.load(tls_template_.address, tls_data.data(), tls_data.size());
            mem_.store(thread_tls_base, tls_data.data(), tls_data.size());
        }
        put<std::uint64_t>(mem_, thread_tcb, thread_tcb);
        std::uint64_t thread_dtv = thread_tcb + 64;
        put<std::uint64_t>(mem_, thread_tcb + 8, thread_dtv);
        put<std::uint64_t>(mem_, thread_tcb + 16, thread_tcb + 256);
        put<std::uint64_t>(mem_, thread_dtv, 1);
        put<std::uint64_t>(mem_, thread_dtv + 8, thread_tls_base);

        auto id = scheduler_->create(a(2), bottom, bottom + thread_stack_size, return_trap, a(3), thread_tcb);
        if (!id) {
            std::printf("[PTHREAD] scheduler_->create failed\n");
            mem_.unmap(bottom);
            return result(invalid);
        }
        std::uint64_t thread_handle = id;
        if (mem_.store(a(0), &thread_handle, sizeof(thread_handle)) != MemoryError::ok) {
            mem_.unmap(bottom);
            return result(invalid);
        }
        thread_stack_next_ += thread_stack_size;
        auto thread_name = (n == "pthread_create") ? "pthread" : (a(4) ? read_guest_string(a(4)) : std::string("unnamed"));
        auto start_res = scheduler_->start(id);
        std::printf("[HLE] Created guest thread %u: '%s' entry=0x%llx arg=0x%llx stack=[0x%llx..0x%llx] start_res=0x%x\n",
                    id, thread_name.c_str(), (unsigned long long)a(2), (unsigned long long)a(3),
                    (unsigned long long)bottom, (unsigned long long)(bottom + thread_stack_size), start_res);
        return result(static_cast<std::uint32_t>(start_res));
    }

    if (n == "scePthreadSelf" || n == "pthread_self") {
        return result(tid);
    }

    if (n == "scePthreadEqual" || n == "pthread_equal") {
        return result(a(0) == a(1) ? 1 : 0);
    }

    if (n == "scePthreadYield" || n == "pthread_yield" || n == "sched_yield") {
        return result(0);
    }

    if (n == "scePthreadSetName" || n == "scePthreadRename" || n == "pthread_setname_np") {
        return result(0);
    }

    if (n == "scePthreadExit" || n == "pthread_exit") {
        scheduler_->exit(tid, a(0));
        return {InvocationResult::Action::exited, 0, 0, 0};
    }

    if (n == "scePthreadJoin" || n == "pthread_join") {
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
        if (!guest_conds_.count(handle)) {
            handle = next_handle_++;
            guest_conds_[handle] = scheduler_->cond_create();
            put(mem_, a(0), handle);
        }
        bool broadcast = (n.find("Broadcast") != std::string::npos || n == "pthread_cond_broadcast");
        return result(static_cast<std::uint32_t>(scheduler_->cond_signal(guest_conds_.at(handle), broadcast)));
    }

    if (n == "scePthreadCondWait" || n == "pthread_cond_wait" ||
        n == "scePthreadCondTimedwait" || n == "pthread_cond_timedwait") {
        if (!a(0) || !a(1)) return result(invalid);
        auto ch = get<std::uint64_t>(mem_, a(0));
        auto mh = get<std::uint64_t>(mem_, a(1));
        if (!guest_conds_.count(ch)) {
            ch = next_handle_++;
            guest_conds_[ch] = scheduler_->cond_create();
            put(mem_, a(0), ch);
        }
        if (!guest_mutexes_.count(mh)) {
            mh = next_handle_++;
            guest_mutexes_[mh] = scheduler_->mutex_create(false);
            put(mem_, a(1), mh);
        }
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
        std::printf("[HLE TID %u] scePthreadCondWait cond=0x%llx mutex=0x%llx status=0x%x (state=%d)\n",
                    tid, (unsigned long long)a(0), (unsigned long long)a(1), (unsigned int)status,
                    (int)scheduler_->state(tid));
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
            const auto handle = static_cast<std::uint32_t>(next_handle_++);
            guest_events_[handle] = scheduler_->event_create(a(2));
            return result(handle);
        } else {
            if (!a(0) || a(2) > 3) return result(invalid);
            const auto handle = static_cast<std::uint32_t>(next_handle_++);
            guest_events_[handle] = scheduler_->event_create(a(3));
            put<std::uint64_t>(mem_, a(0), static_cast<std::uint64_t>(handle));
            return result(0);
        }
    }

    if (n == "sceKernelDeleteEventFlag") {
        auto handle = static_cast<std::uint32_t>(a(0));
        if (!guest_events_.erase(handle)) return result(0x80020003); // ESRCH
        return result(0);
    }

    if (n == "sceKernelSetEventFlag") {
        auto handle = static_cast<std::uint32_t>(a(0));
        if (!guest_events_.count(handle)) return result(0x80020003); // ESRCH
        return result(static_cast<std::uint32_t>(scheduler_->event_set(guest_events_.at(handle), a(1))));
    }

    if (n == "sceKernelClearEventFlag") {
        auto handle = static_cast<std::uint32_t>(a(0));
        if (!guest_events_.count(handle)) return result(0x80020003); // ESRCH
        return result(static_cast<std::uint32_t>(scheduler_->event_clear(guest_events_.at(handle), a(1))));
    }

    if (n == "sceKernelWaitEventFlag" || n == "sceKernelPollEventFlag") {
        auto handle = static_cast<std::uint32_t>(a(0));
        if (!guest_events_.count(handle)) return result(0x80020003); // ESRCH
        const auto eid = guest_events_.at(handle);
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
        std::string sema_name = "sema";
        std::uint32_t attr = 0;
        std::int32_t init_val = 0, max_val = 1;
        std::uint64_t out_id_addr = 0;
        bool is_sdk = false;

        if (a(4) > 0) {
            // Standard Orbis API: sceKernelCreateSema(out_id, name, attr, init, max, opt)
            out_id_addr = a(0);
            if (!out_id_addr) return result(invalid);
            if (a(1)) sema_name = read_guest_string(a(1), 32);
            attr = static_cast<std::uint32_t>(a(2));
            init_val = static_cast<std::int32_t>(a(3));
            max_val = static_cast<std::int32_t>(a(4));
            is_sdk = false;
        } else if ((a(2) >> 16) > 0) {
            // Packed legacy test format: a(0)=out, a(1)=name, a(2)=max<<16|init
            out_id_addr = a(0);
            if (!out_id_addr) return result(invalid);
            if (a(1)) sema_name = read_guest_string(a(1), 32);
            init_val = static_cast<std::int32_t>(a(2) & 0xFFFF);
            max_val = static_cast<std::int32_t>((a(2) >> 16) & 0xFFFF);
            is_sdk = false;
        } else {
            // Unit test SDK format: a(0)=name, a(1)=attr, a(2)=init, a(3)=max -> returns handle
            if (a(0)) sema_name = read_guest_string(a(0), 32);
            attr = static_cast<std::uint32_t>(a(1));
            init_val = static_cast<std::int32_t>(a(2));
            max_val = static_cast<std::int32_t>(a(3));
            is_sdk = true;
        }
        if (max_val <= 0) max_val = std::max<std::int32_t>(1, init_val);
        if (init_val < 0 || max_val <= 0 || init_val > max_val) return result(invalid);

        auto sid = scheduler_->sema_create(init_val, max_val);
        if (!sid) return result(no_memory);
        auto handle = static_cast<std::uint32_t>(next_handle_++);
        guest_semas_[handle] = sid;

        SemaRecord rec;
        rec.name = sema_name;
        rec.attr = attr;
        rec.init_count = init_val;
        rec.max_count = max_val;
        sema_records_[handle] = rec;

        std::printf("[SEMA] sceKernelCreateSema name='%s' init=%d max=%d is_sdk=%d -> handle=0x%x sid=%u out_addr=0x%llx\n",
                    sema_name.c_str(), init_val, max_val, is_sdk ? 1 : 0, handle, sid, (unsigned long long)out_id_addr);

        if (is_sdk) {
            return result(handle);
        } else {
            put<std::uint64_t>(mem_, out_id_addr, static_cast<std::uint64_t>(handle));
            return result(0);
        }
    }

    if (n == "sceKernelWaitSema") {
        auto sid_handle = static_cast<std::uint32_t>(a(0));
        if (!sid_handle) {
            std::printf("[SEMA] sceKernelWaitSema called with a(0)=0 (TID %u)\n", tid);
            return result(invalid);
        }
        auto it = guest_semas_.find(sid_handle);
        if (it == guest_semas_.end()) {
            std::printf("[SEMA] sceKernelWaitSema handle 0x%x NOT FOUND in guest_semas_ (TID %u)\n",
                        sid_handle, tid);
            return result(0x80020003); // ESRCH
        }
        std::int32_t need = a(1) ? static_cast<std::int32_t>(a(1)) : 1;
        std::uint64_t timeout = 0;
        if (a(2)) {
            std::uint32_t to32 = 0;
            if (mem_.load(a(2), &to32, 4) == MemoryError::ok) timeout = to32;
        }
        auto status = scheduler_->sema_wait(tid, it->second, need, timeout);
        if (status) {
            std::printf("[SEMA] sema_wait failed with status=0x%x (handle=0x%x sid=%u need=%d tid=%u)\n",
                        status, sid_handle, it->second, need, tid);
            return result(static_cast<std::uint32_t>(status));
        }
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
        auto sid_handle = static_cast<std::uint32_t>(a(0));
        if (!sid_handle) return result(invalid);
        auto it = guest_semas_.find(sid_handle);
        if (it == guest_semas_.end()) return result(0x80020003); // ESRCH
        std::int32_t need = a(1) ? static_cast<std::int32_t>(a(1)) : 1;
        return result(static_cast<std::uint32_t>(scheduler_->sema_poll(it->second, need)));
    }

    if (n == "sceKernelSignalSema") {
        auto sid_handle = static_cast<std::uint32_t>(a(0));
        if (!sid_handle) return result(invalid);
        auto it = guest_semas_.find(sid_handle);
        if (it == guest_semas_.end()) return result(0x80020003); // ESRCH
        std::int32_t count = a(1) ? static_cast<std::int32_t>(a(1)) : 1;
        return result(static_cast<std::uint32_t>(scheduler_->sema_signal(it->second, count)));
    }

    if (n == "sceKernelDeleteSema") {
        auto sid_handle = static_cast<std::uint32_t>(a(0));
        if (!sid_handle) return result(invalid);
        auto it = guest_semas_.find(sid_handle);
        if (it == guest_semas_.end()) return result(0x80020003); // ESRCH
        scheduler_->sema_delete(it->second);
        guest_semas_.erase(it);
        sema_records_.erase(sid_handle);
        return result(0);
    }

    if (n == "sceKernelGetSemaInfo") {
        if (!a(0) || !a(1)) return result(invalid);
        auto sid_handle = static_cast<std::uint32_t>(a(0));
        auto it_rec = sema_records_.find(sid_handle);
        if (it_rec == sema_records_.end()) return result(0x80020003); // ESRCH
        auto it_guest = guest_semas_.find(sid_handle);
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

    if (n == "__error") {
        return result(get_errno_addr(tid));
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
    if (n=="__error") return get_errno_addr(1);
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
    if (n == "sceUserServiceGetEvent") {
        auto out = a(0);
        if (!out) return 0x80960005; // USER_INVALID_ARGUMENT
        if (!user_login_event_pending_) return 0x80960007; // USER_NO_EVENT
        user_login_event_pending_ = false;
        std::int32_t event[2] = {0, 1}; // LOGIN, USER_ID = 1
        mem_.store(out, event, sizeof(event));
        return 0;
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

    // libSceAvPlayer HLE (stubs para transição direta aos menus)
    if (n == "sceAvPlayerInit") return next_handle_++;
    if (n == "sceAvPlayerClose" || n == "sceAvPlayerPostInit" ||
        n == "sceAvPlayerAddSource" || n == "sceAvPlayerSetLooping" ||
        n == "sceAvPlayerStart" || n == "sceAvPlayerStop" ||
        n == "sceAvPlayerPause" || n == "sceAvPlayerResume" ||
        n == "sceAvPlayerEnableStream") {
        return 0;
    }
    if (n == "sceAvPlayerIsActive" || n == "sceAvPlayerGetVideoDataEx" ||
        n == "sceAvPlayerGetAudioData" || n == "sceAvPlayerCurrentTime" ||
        n == "sceAvPlayerStreamCount" || n == "sceAvPlayerGetStreamInfo") {
        return 0;
    }

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
        auto eq_id = static_cast<std::uint32_t>(a(0));
        auto udata = a(2);
        auto it = equeues_.find(eq_id);
        if (it != equeues_.end()) {
            it->second.has_flip_event = true;
            it->second.flip_udata = udata;
        }
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

        if (!video_out_ || !addrs_ptr || !attr_ptr || start_idx < 0 ||
            buf_count <= 0 || buf_count > 16 || start_idx > 16 - buf_count) {
            return static_cast<std::uint64_t>(-1);
        }
        std::uint32_t attr_fields[6] = {};
        if (mem_.load(attr_ptr, attr_fields, sizeof(attr_fields)) != MemoryError::ok) {
            return static_cast<std::uint64_t>(-1);
        }
        std::uint32_t width = attr_fields[3];
        std::uint32_t height = attr_fields[4];
        std::uint32_t pitch = attr_fields[5] ? attr_fields[5] : width;
        std::uint32_t bpp = 4;
        for (int i = 0; i < buf_count; ++i) {
            std::uint64_t buf_addr = 0;
            if (mem_.load(addrs_ptr + static_cast<std::uint64_t>(i) * 8, &buf_addr, 8) != MemoryError::ok) {
                return static_cast<std::uint64_t>(-1);
            }
            VideoBuffer vb{buf_addr, width, height, pitch, bpp};
            if (video_out_->register_buffer(static_cast<std::uint32_t>(start_idx + i), vb) != VideoStatus::ok) {
                return static_cast<std::uint64_t>(-1);
            }
        }
        return 0;
    }
    if (n == "sceVideoOutSubmitFlip") {
        int buf_idx = static_cast<int>(a(1));
        std::uint64_t flip_arg = a(3);
        if (!video_out_ || buf_idx < 0 ||
            video_out_->submit_flip(static_cast<std::uint32_t>(buf_idx), flip_arg) != VideoStatus::ok) {
            return static_cast<std::uint64_t>(-1);
        }
        for (auto& pair : equeues_) {
            if (pair.second.has_flip_event) {
                EqueueEvent ev{0x6, -13, 0, 0, 1, pair.second.flip_udata};
                pair.second.pending_events.push_back(ev);
            }
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

    // Kernel Event Queue (Equeue) HLE
    if (n == "sceKernelCreateEqueue") {
        auto out_eq = a(0);
        auto name_ptr = a(1);
        std::string name = name_ptr ? read_guest_string(name_ptr) : "equeue";
        auto id = static_cast<std::uint32_t>(next_equeue_id_++);
        equeues_[id] = EqueueRecord{name, {}, 0, false};
        if (out_eq) put<std::uint64_t>(mem_, out_eq, id);
        return 0;
    }
    if (n == "sceKernelDeleteEqueue") {
        auto id = static_cast<std::uint32_t>(a(0));
        equeues_.erase(id);
        return 0;
    }
    if (n == "sceKernelWaitEqueue") {
        auto id = static_cast<std::uint32_t>(a(0));
        auto ev_ptr = a(1);
        auto num = static_cast<int>(a(2));
        auto out_ptr = a(3);

        auto it = equeues_.find(id);
        if (it == equeues_.end()) {
            if (out_ptr) put<std::int32_t>(mem_, out_ptr, 0);
            return 0;
        }

        if (num > 0 && ev_ptr) {
            if (!it->second.pending_events.empty()) {
                auto ev = it->second.pending_events.front();
                it->second.pending_events.erase(it->second.pending_events.begin());
                mem_.store(ev_ptr, &ev, sizeof(EqueueEvent));
                if (out_ptr) put<std::int32_t>(mem_, out_ptr, 1);
                return 0;
            }
        }

        if (out_ptr) put<std::int32_t>(mem_, out_ptr, 0);
        return 0;
    }

    // libSceGnmDriver HLE
    if (n == "sceGnmGetTheTessellationFactorRingBufferBaseAddress") {
        if (!tessellation_ring_addr_) {
            const std::size_t size = 0x800000; // 8 MB
            tessellation_ring_addr_ = next_virtual_;
            next_virtual_ += size;
            mem_.map(tessellation_ring_addr_, size, bb::Permission::read | bb::Permission::write);
            mappings_[tessellation_ring_addr_] = size;
        }
        return tessellation_ring_addr_;
    }
    if (n == "sceGnmDrawInitDefaultHardwareState200" || n == "sceGnmDispatchInitDefaultHardwareState") {
        auto cmdbuf = a(0);
        auto size = a(1);
        if (cmdbuf && size >= 0x100) {
            std::vector<std::uint32_t> nops(0x100, 0xffff0000);
            nops[0] = 0xc0001000 | (0x100 - 2); // Type 3 PM4 NOP
            mem_.store(cmdbuf, nops.data(), 0x100 * sizeof(std::uint32_t));
        }
        return 0x100;
    }
    if (n == "sceGnmAreSubmitsAllowed") {
        return 1;
    }
    if (n == "sceGnmMapComputeQueue" || n == "sceGnmMapComputeQueueWithPriority") {
        auto read_ptr = a(4);
        if (read_ptr) {
            put<std::uint32_t>(mem_, read_ptr, 0);
        }
        return 1;
    }
    if (n == "sceGnmUnmapComputeQueue" || n == "sceGnmDingDong" || n == "sceGnmFlushGarlic") {
        return 0;
    }
    if (n == "sceGnmLogicalCuIndexToPhysicalCuIndex" || n == "sceGnmLogicalCuMaskToPhysicalCuMask") {
        return a(1) != 0 ? a(1) : a(0);
    }
    if (n == "sceGnmInsertWaitFlipDone") {
        auto cmdbuf = a(0);
        auto size = a(1);
        if (cmdbuf && size >= 7) {
            std::vector<std::uint32_t> nops(7, 0xffff0000);
            mem_.store(cmdbuf, nops.data(), 7 * sizeof(std::uint32_t));
        }
        return 0;
    }
    if (n == "sceGnmInsertSetMarker" || n == "sceGnmInsertSetColorMarker" ||
        n == "sceGnmInsertPushMarker" || n == "sceGnmInsertPushColorMarker" ||
        n == "sceGnmInsertPopMarker") {
        return 0;
    }
    if (n == "sceGnmSetPsShader" || n == "sceGnmUpdatePsShader" ||
        n == "sceGnmSetVsShader" || n == "sceGnmUpdateVsShader" ||
        n == "sceGnmSetEsShader" || n == "sceGnmSetHsShader" ||
        n == "sceGnmUpdateHsShader" || n == "sceGnmSetLsShader" ||
        n == "sceGnmSetCsShader" || n == "sceGnmSetGsShader" ||
        n == "sceGnmUpdateGsShader") {
        return 0;
    }
    if (n == "sceGnmSubmitCommandBuffers" || n == "sceGnmSubmitAndFlipCommandBuffers") {
        uint32_t count = static_cast<uint32_t>(a(0));
        uint64_t dcb_addrs_ptr = a(1);
        uint64_t dcb_sizes_ptr = a(2);
        if (!gnm_context_ || !dcb_addrs_ptr || !dcb_sizes_ptr || count == 0 || count >= 64) {
            return static_cast<std::uint64_t>(-1);
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t dcb_addr = 0;
            uint32_t dcb_size = 0;
            if (mem_.load(dcb_addrs_ptr + i * 8, &dcb_addr, 8) != MemoryError::ok ||
                mem_.load(dcb_sizes_ptr + i * 4, &dcb_size, 4) != MemoryError::ok ||
                dcb_addr == 0 || dcb_size < 4 || dcb_size % 4 != 0 ||
                gnm_context_->submit(dcb_addr, dcb_size / 4) != Pm4Status::ok) {
                return static_cast<std::uint64_t>(-1);
            }
        }
        if (n == "sceGnmSubmitAndFlipCommandBuffers") {
            auto h = static_cast<std::uint32_t>(a(5));
            auto buf_idx = static_cast<std::uint32_t>(a(6));
            auto flip_arg = a(8);
            if (!video_out_) return static_cast<std::uint64_t>(-1);
            VideoStatus flip_status = VideoStatus::invalid;
            if (buf_idx < 16) {
                flip_status = video_out_->submit_flip(buf_idx, flip_arg);
            } else if (h < 16) {
                flip_status = video_out_->submit_flip(h, a(7));
            }
            if (flip_status != VideoStatus::ok) return static_cast<std::uint64_t>(-1);
            for (auto& pair : equeues_) {
                if (pair.second.has_flip_event) {
                    EqueueEvent ev{0x6, -13, 0, 0, 1, pair.second.flip_udata};
                    pair.second.pending_events.push_back(ev);
                }
            }
        }
        return 0;
    }
    if (n == "sceGnmValidateCommandBuffers" || n == "sceGnmAddEqEvent" ||
        n == "sceGnmDeleteEqEvent" || n == "sceGnmGetEqEventType" ||
        n == "sceGnmGetEqTimeStamp" || n == "sceGnmDebugHardwareStatus" ||
        n == "sceGnmSubmitDone" || n == "sceGnmRequestFlipAndSubmitDone" ||
        n == "sceGnmSetupMipStatsReport" || n == "sceGnmRequestMipStatsReportAndReset" ||
        n == "sceGnmDisableMipStatsReport" || n == "sceGnmSetGsRingSizes" ||
        n == "sceGnmDriverTriggerCapture" || n == "sceGnmDriverCaptureInProgress" ||
        n == "sceGnmIsUserPaEnabled") {
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
    if (n=="sceKernelMapDirectMemory" || n=="sceKernelMmap" ||
        n=="sceKernelMapFlexibleMemory" || n=="sceKernelMapNamedFlexibleMemory") {
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
    if (n=="sceKernelMunmap" || n=="munmap" || n=="sceKernelReleaseFlexibleMemory") {
        auto it=mappings_.find(a(0));
        if (it==mappings_.end() || a(1)!=it->second) return invalid;
        mem_.unmap(it->first); direct_mappings_.erase(it->first); mappings_.erase(it); return 0;
    }
    if (n=="sceKernelReserveVirtualRange") {
        auto inout = a(0);
        auto size = a(1);
        auto flags = a(2);
        auto alignment = a(3) ? a(3) : page;
        if (!inout || !size || size%page || !valid_alignment(alignment)) return invalid;
        auto at = get<std::uint64_t>(mem_, inout);
        if (!(flags & 0x10) || !at) {
            at = align(next_virtual_, alignment);
        }
        if (mem_.map(at, static_cast<std::size_t>(size), read|write) != MemoryError::ok) return no_memory;
        put(mem_, inout, at);
        mappings_[at] = size;
        next_virtual_ = std::max(next_virtual_, at + size);
        return 0;
    }
    if (n=="sceKernelMprotect") return 0;
    if (n=="sceKernelQueryMemoryProtection") {
        auto addr = a(0);
        auto out_start = a(1);
        auto out_end = a(2);
        auto out_prot = a(3);
        std::uint64_t base = 0;
        std::size_t size = 0;
        unsigned perm = bb::Permission::read | bb::Permission::write;
        if (!mem_.query_region(addr, base, size, perm)) {
            if (addr < 0x1000) return 0x8002000e; // EFAULT
            base = addr & ~0x3fffULL;
            size = 0x4000;
            perm = bb::Permission::read | bb::Permission::write;
        }
        if (out_start) put<std::uint64_t>(mem_, out_start, base);
        if (out_end) put<std::uint64_t>(mem_, out_end, base + size);
        if (out_prot) {
            std::uint32_t p = 0;
            if (perm & bb::Permission::read) p |= 0x11;  // CPU_READ | GPU_READ
            if (perm & bb::Permission::write) p |= 0x22; // CPU_WRITE | GPU_WRITE
            put<std::uint32_t>(mem_, out_prot, p);
        }
        return 0;
    }
    if (n=="sceKernelVirtualQuery") {
        auto addr = a(0);
        auto info = a(2);
        auto info_size = a(3);
        if (!info || info_size < 32) return invalid;
        std::uint64_t base = 0;
        std::size_t size = 0;
        unsigned perm = bb::Permission::read | bb::Permission::write;
        if (!mem_.query_region(addr, base, size, perm)) {
            if (addr < 0x1000) return 0x8002000e;
            base = addr & ~0x3fffULL;
            size = 0x4000;
            perm = bb::Permission::read | bb::Permission::write;
        }
        put<std::uint64_t>(mem_, info, base);
        put<std::uint64_t>(mem_, info + 8, base + size);
        put<std::uint64_t>(mem_, info + 16, 0); // offset
        std::uint32_t p = 0;
        if (perm & bb::Permission::read) p |= 0x11;
        if (perm & bb::Permission::write) p |= 0x22;
        put<std::int32_t>(mem_, info + 24, p);
        put<std::int32_t>(mem_, info + 28, 0); // memory_type
        if (info_size >= 72) {
            put<std::uint8_t>(mem_, info + 32, 0x13); // is_flexible | is_direct | is_committed
            std::vector<std::uint8_t> zeros(32, 0);
            mem_.store(info + 33, zeros.data(), 32);
        }
        return 0;
    }
    if (n=="sceKernelGetDirectMemoryType") {
        auto out_type = a(1);
        if (out_type) put<std::int32_t>(mem_, out_type, 0); // main memory type
        return 0;
    }
    if (n=="sceKernelBatchMap" || n=="sceKernelBatchMap2") {
        auto entries_addr = a(0);
        auto count = static_cast<int>(a(1));
        auto processed_addr = a(2);
        auto flags = (n=="sceKernelBatchMap2") ? a(3) : 0x10;
        int done = 0;
        for (; done < count; ++done) {
            auto entry_ptr = entries_addr + static_cast<std::uint64_t>(done) * 32;
            std::uint64_t start_addr = 0, offset = 0, length = 0;
            std::int8_t prot = 0, type = 0;
            std::int16_t reserved = 0;
            std::int32_t operation = 0;
            mem_.load(entry_ptr, &start_addr, 8);
            mem_.load(entry_ptr + 8, &offset, 8);
            mem_.load(entry_ptr + 16, &length, 8);
            mem_.load(entry_ptr + 24, &prot, 1);
            mem_.load(entry_ptr + 25, &type, 1);
            mem_.load(entry_ptr + 26, &reserved, 2);
            mem_.load(entry_ptr + 28, &operation, 4);

            if (operation == 0 || operation == 3) { // map direct ou flexible
                if (!start_addr || !length || length % page) break;
                if (!(flags & 0x10) || !start_addr) {
                    start_addr = align(next_virtual_, page);
                }
                if (mem_.map(start_addr, static_cast<std::size_t>(length), read|write) != MemoryError::ok) break;
                put(mem_, entry_ptr, start_addr);
                mappings_[start_addr] = length;
                next_virtual_ = std::max(next_virtual_, start_addr + length);
            } else if (operation == 1) { // unmap
                auto it = mappings_.find(start_addr);
                if (it != mappings_.end()) {
                    mem_.unmap(it->first);
                    mappings_.erase(it);
                }
            } else if (operation == 2 || operation == 4) { // protect
                // no-op no ambiente emulado
            }
        }
        if (processed_addr) put<std::int32_t>(mem_, processed_addr, done);
        return (done == count) ? 0 : invalid;
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
        std::string sema_name = "sema";
        std::uint32_t attr = SCE_KERNEL_SEMA_ATTR_TH_FIFO;
        std::int32_t init_val = 0, max_val = 1;
        std::uint64_t out_id_addr = 0;
        bool is_sdk = false;

        if (a(4) > 0) {
            // Standard Orbis API: sceKernelCreateSema(out_id, name, attr, init, max, opt)
            out_id_addr = a(0);
            if (!out_id_addr) return invalid;
            if (a(1)) sema_name = read_guest_string(a(1), 32);
            attr = static_cast<std::uint32_t>(a(2));
            init_val = static_cast<std::int32_t>(a(3));
            max_val = static_cast<std::int32_t>(a(4));
            is_sdk = false;
        } else if ((a(2) >> 16) > 0) {
            // Packed legacy test format: a(0)=out, a(1)=name, a(2)=max<<16|init
            out_id_addr = a(0);
            if (!out_id_addr) return invalid;
            if (a(1)) sema_name = read_guest_string(a(1), 32);
            init_val = static_cast<std::int32_t>(a(2) & 0xFFFF);
            max_val = static_cast<std::int32_t>((a(2) >> 16) & 0xFFFF);
            is_sdk = false;
        } else {
            // Unit test SDK format: a(0)=name, a(1)=attr, a(2)=init, a(3)=max -> returns handle
            if (a(0)) sema_name = read_guest_string(a(0), 32);
            attr = static_cast<std::uint32_t>(a(1));
            init_val = static_cast<std::int32_t>(a(2));
            max_val = static_cast<std::int32_t>(a(3));
            is_sdk = true;
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
    // pthread thread attributes (scePthreadAttr* / pthread_attr_*)
    if (n=="scePthreadAttrInit" || n=="pthread_attr_init") {
        if (!a(0)) return invalid;
        auto h = next_handle_++;
        put(mem_, a(0), h);
        auto& at = attr_defaults_[h];
        at = {};
        at.affinity = 0x7F;
        at.stack_size = 0x100000;
        return 0;
    }
    if (n=="scePthreadAttrDestroy" || n=="pthread_attr_destroy") {
        if (a(0)) {
            std::uint64_t h = 0;
            if (mem_.load(a(0), &h, 8) == MemoryError::ok) {
                attr_defaults_.erase(h);
                put<std::uint64_t>(mem_, a(0), 0);
            }
            attr_defaults_.erase(a(0));
        }
        return 0;
    }
    if (n=="scePthreadAttrSetstacksize" || n=="pthread_attr_setstacksize") {
        if (!a(0) || !a(1)) return invalid;
        auto* at = find_attr(a(0));
        if (!at) return invalid;
        at->stack_size = a(1);
        return 0;
    }
    if (n=="scePthreadAttrGetstacksize" || n=="pthread_attr_getstacksize") {
        if (!a(0) || !a(1)) return invalid;
        auto* at = find_attr(a(0));
        std::uint64_t sz = at && at->stack_size ? at->stack_size : (2 * 1024 * 1024);
        put<std::uint64_t>(mem_, a(1), sz);
        return 0;
    }
    if (n=="scePthreadAttrSetguardsize" || n=="pthread_attr_setguardsize") {
        return 0;
    }
    if (n=="scePthreadAttrGetguardsize" || n=="pthread_attr_getguardsize") {
        if (a(1)) put<std::uint64_t>(mem_, a(1), 4096);
        return 0;
    }
    if (n=="scePthreadAttrSetstack" || n=="pthread_attr_setstack") {
        if (!a(0) || !a(2)) return invalid;
        auto* at = find_attr(a(0));
        if (at) at->stack_size = a(2);
        return 0;
    }
    if (n=="scePthreadAttrGetstack" || n=="pthread_attr_getstack") {
        if (!a(0) || !a(1) || !a(2)) return invalid;
        auto* at = find_attr(a(0));
        std::uint64_t sz = at && at->stack_size ? at->stack_size : (2 * 1024 * 1024);
        put<std::uint64_t>(mem_, a(1), 0);
        put<std::uint64_t>(mem_, a(2), sz);
        return 0;
    }
    if (n=="scePthreadAttrSetdetachstate" || n=="pthread_attr_setdetachstate") {
        if (!a(0)) return invalid;
        auto* at = find_attr(a(0));
        if (!at) return invalid;
        at->detach_state = static_cast<std::uint32_t>(a(1));
        return 0;
    }
    if (n=="scePthreadAttrGetdetachstate" || n=="pthread_attr_getdetachstate") {
        if (!a(0) || !a(1)) return invalid;
        auto* at = find_attr(a(0));
        put<std::uint32_t>(mem_, a(1), at ? at->detach_state : 0);
        return 0;
    }
    if (n=="scePthreadAttrSetschedpolicy" || n=="pthread_attr_setschedpolicy") {
        if (!a(0)) return invalid;
        auto* at = find_attr(a(0));
        if (!at) return invalid;
        at->sched_policy = static_cast<std::uint32_t>(a(1));
        return 0;
    }
    if (n=="scePthreadAttrGetschedpolicy" || n=="pthread_attr_getschedpolicy") {
        if (!a(0) || !a(1)) return invalid;
        auto* at = find_attr(a(0));
        put<std::uint32_t>(mem_, a(1), at ? at->sched_policy : 0);
        return 0;
    }
    if (n=="scePthreadAttrSetschedparam" || n=="pthread_attr_setschedparam") {
        if (!a(0) || !a(1)) return invalid;
        auto* at = find_attr(a(0));
        if (!at) return invalid;
        std::uint32_t param = 0;
        if (mem_.load(a(1), &param, 4) != MemoryError::ok) return invalid;
        at->sched_param = param;
        return 0;
    }
    if (n=="scePthreadAttrGetschedparam" || n=="pthread_attr_getschedparam") {
        if (!a(0) || !a(1)) return invalid;
        auto* at = find_attr(a(0));
        put<std::uint32_t>(mem_, a(1), at ? at->sched_param : 0);
        return 0;
    }
    if (n=="scePthreadAttrSetaffinity") {
        if (!a(0) || !a(1)) return invalid;
        auto* at = find_attr(a(0));
        if (!at) return invalid;
        std::uint64_t affinity = 0;
        if (mem_.load(a(1), &affinity, 8) != MemoryError::ok) return invalid;
        at->affinity = affinity;
        return 0;
    }
    if (n=="scePthreadAttrSetinheritsched" || n=="pthread_attr_setinheritsched") {
        if (!a(0)) return invalid;
        auto* at = find_attr(a(0));
        if (!at) return invalid;
        at->inherit_sched = static_cast<std::uint32_t>(a(1));
        return 0;
    }
    if (n=="scePthreadAttrGetinheritsched" || n=="pthread_attr_getinheritsched") {
        if (!a(0) || !a(1)) return invalid;
        auto* at = find_attr(a(0));
        put<std::uint32_t>(mem_, a(1), at ? at->inherit_sched : 0);
        return 0;
    }
    if (n=="scePthreadAttrGet") {
        if (!a(0) || !a(1)) return invalid;
        auto* at = find_attr(a(1));
        if (!at) return invalid;
        at->affinity = 0x7F;
        at->stack_size = 0x100000;
        at->detach_state = 0;
        at->sched_policy = 1;
        return 0;
    }
    if (n=="scePthreadAttrGetaffinity") {
        if (!a(0) || !a(1)) return invalid;
        auto* at = find_attr(a(0));
        const std::uint64_t aff = at ? at->affinity : 0x7F;
        if (mem_.store(a(1), &aff, 8) != MemoryError::ok) return invalid;
        return 0;
    }
    // pthread thread functions (stubs for non-scheduler mode)
    if (n=="scePthreadSelf" || n=="pthread_self" || n=="pthread_getthreadid_np") {
        return 1; // main thread ID
    }
    if (n=="scePthreadCreate" || n=="pthread_create" || n=="pthread_create_name_np") {
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
    if (n=="sceKernelReadTsc") {
        if (scheduler_) return scheduler_->now();
        static std::uint64_t tsc_val = 1000000;
        return tsc_val += 100;
    }
    if (n=="sceKernelGetTscFrequency") {
        return 1600000000ULL;
    }
    if (n=="_Assert") {
        std::string expr = a(0) ? read_guest_string(a(0)) : "";
        std::string file = a(1) ? read_guest_string(a(1)) : "";
        std::printf("[GUEST ASSERT] expr='%s' file='%s' line=%llu\n",
                    expr.c_str(), file.c_str(), (unsigned long long)a(2));
        return 0;
    }
    if (n=="sceKernelGetProcessTime") {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    if (n=="sceKernelClockGettime" || n=="clock_gettime") {
        if (!a(1) || a(0)>4) return invalid;
        const auto now=a(0)==0 ? std::chrono::system_clock::now().time_since_epoch() : std::chrono::steady_clock::now().time_since_epoch();
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        put<std::int64_t>(mem_,a(1),ns/1000000000); put<std::int64_t>(mem_,a(1)+8,ns%1000000000); return 0;
    }
    if (n=="sceSysmoduleLoadModule") {
        const auto id = static_cast<std::uint16_t>(a(0));
        if (!id) return 0x805A1000; // SCE_SYSMODULE_ERROR_INVALID_VALUE
        return 0; // SCE_SYSMODULE_OK
    }
    if (n=="sceSysmoduleUnloadModule") {
        const auto id = static_cast<std::uint16_t>(a(0));
        if (!id) return 0x805A1000;
        return 0;
    }
    if (n=="sceSysmoduleIsLoaded") {
        const auto id = static_cast<std::uint16_t>(a(0));
        if (!id) return 0x805A1000;
        return 0; // SCE_SYSMODULE_LOADED
    }
    if (n=="sceAppContentInitialize") {
        const auto boot_param_addr = a(1);
        if (boot_param_addr) {
            std::uint8_t zero[40] = {};
            mem_.store(boot_param_addr, zero, sizeof(zero));
        }
        return 0; // SCE_APP_CONTENT_OK
    }
    if (n=="sceAppContentAppParamGetInt") {
        const auto param_id = static_cast<std::uint32_t>(a(0));
        const auto out_addr = a(1);
        if (!out_addr) return 0x80D90002; // SCE_APP_CONTENT_ERROR_PARAMETER
        std::int32_t val = 0;
        if (param_id == 0) val = 3; // SCE_APP_CONTENT_SKU_FLAG_FULL
        else if (param_id == 1) val = 13; // USER_DEFINED_PARAM_1 (param.sfo Bloodborne)
        else if (param_id <= 4) val = 0;
        else return 0x80D90002;
        mem_.store(out_addr, &val, sizeof(val));
        return 0;
    }
    if (n=="sceAppContentGetAddcontInfoList") {
        const auto hits_addr = a(3);
        if (hits_addr) {
            std::uint32_t hits = 0;
            mem_.store(hits_addr, &hits, sizeof(hits));
        }
        return 0;
    }
    // Dialog HLE: CommonDialog, MsgDialog, ImeDialog, SaveDataDialog, NpProfileDialog, NpCommerceDialog
    if (n=="sceCommonDialogInitialize") {
        return default_dialog_manager().sceCommonDialogInitialize();
    }
    if (n=="sceCommonDialogGetStatus") {
        return default_dialog_manager().sceCommonDialogGetStatus();
    }
    if (n=="sceMsgDialogInitialize") {
        return default_dialog_manager().sceMsgDialogInitialize();
    }
    if (n=="sceMsgDialogOpen") {
        return default_dialog_manager().sceMsgDialogOpen(mem_, a(0));
    }
    if (n=="sceMsgDialogGetStatus") {
        return default_dialog_manager().sceMsgDialogGetStatus();
    }
    if (n=="sceMsgDialogUpdateStatus") {
        return default_dialog_manager().sceMsgDialogUpdateStatus();
    }
    if (n=="sceMsgDialogGetResult") {
        return default_dialog_manager().sceMsgDialogGetResult(mem_, a(0));
    }
    if (n=="sceMsgDialogTerminate") {
        return default_dialog_manager().sceMsgDialogTerminate();
    }
    if (n=="sceImeDialogInit") {
        return default_dialog_manager().sceImeDialogInit(mem_, a(0));
    }
    if (n=="sceImeDialogGetStatus") {
        return default_dialog_manager().sceImeDialogGetStatus();
    }
    if (n=="sceImeDialogGetResult") {
        return default_dialog_manager().sceImeDialogGetResult(mem_, a(0));
    }
    if (n=="sceImeDialogTerm") {
        return default_dialog_manager().sceImeDialogTerm();
    }
    if (n=="sceImeDialogAbort") {
        return default_dialog_manager().sceImeDialogAbort();
    }
    if (n=="sceSaveDataDialogInitialize") return 0;
    if (n=="sceSaveDataDialogOpen") return 0;
    if (n=="sceSaveDataDialogUpdateStatus") return 3; // FINISHED
    if (n=="sceSaveDataDialogTerminate") return 0;
    if (n=="sceNpProfileDialogInitialize") return 0;
    if (n=="sceNpProfileDialogOpen") return 0;
    if (n=="sceNpProfileDialogUpdateStatus") return 3; // FINISHED
    if (n=="sceNpProfileDialogGetResult") {
        if (a(0)) put<std::uint32_t>(mem_, a(0), 0);
        return 0;
    }
    if (n=="sceNpProfileDialogTerminate") return 0;
    if (n=="sceNpCommerceDialogInitialize") return 0;
    if (n=="sceNpCommerceDialogOpen") return 0;
    if (n=="sceNpCommerceDialogUpdateStatus") return 3; // FINISHED
    if (n=="sceNpCommerceDialogTerminate") return 0;

    // libSceSystemService HLE
    if (n == "sceSystemServiceParamGetInt") {
        auto id = static_cast<std::int32_t>(a(0));
        auto out = a(1);
        if (!out) return 0x80A10003; // SYSTEM_PARAMETER
        std::int32_t val = 0;
        switch (id) {
            case 1: val = 1; break; // Language (1 = English US)
            case 2: val = 1; break; // Date format DD/MM/YYYY
            case 3: val = 1; break; // 24-hour clock
            case 4: val = -180; break; // UTC offset in minutes (-3h)
            case 5: val = 0; break; // Summer time off
            case 7: val = 0; break; // Parental level off
            case 1000: val = 1; break; // Enter button = cross
            default: val = 0; break;
        }
        mem_.store(out, &val, sizeof(val));
        return 0;
    }
    if (n == "sceSystemServiceGetStatus") {
        auto out = a(0);
        if (!out) return 0x80A10003;
        std::uint8_t status[12] = {}; // event_num=0, no overlay, foreground, normal CPU mode
        mem_.store(out, status, sizeof(status));
        return 0;
    }
    if (n == "sceSystemServiceReceiveEvent") {
        return 0x80A10004; // SYSTEM_NO_EVENT
    }
    if (n == "sceSystemServiceHideSplashScreen" || n == "sceSystemServiceLaunchWebBrowser") {
        return 0;
    }

    // libScePlayGo HLE
    if (n == "scePlayGoInitialize") return 0;
    if (n == "scePlayGoOpen") {
        auto handle_addr = a(0);
        if (!handle_addr) return 0x80B2000A; // PLAYGO_BAD_POINTER
        std::int32_t h = 1;
        mem_.store(handle_addr, &h, sizeof(h));
        return 0;
    }
    if (n == "scePlayGoGetChunkId") {
        auto handle = static_cast<std::int32_t>(a(0));
        auto ids_addr = a(1);
        auto count = static_cast<std::uint32_t>(a(2));
        auto out_addr = a(3);
        if (handle != 1) return 0x80B20009; // PLAYGO_BAD_HANDLE
        if (!out_addr) return 0x80B2000A; // PLAYGO_BAD_POINTER
        if (ids_addr && !count) return 0x80B2000B; // PLAYGO_BAD_SIZE
        if (!ids_addr) {
            std::uint32_t total = 1;
            mem_.store(out_addr, &total, sizeof(total));
            return 0;
        }
        std::uint16_t chunk_id = 0;
        mem_.store(ids_addr, &chunk_id, sizeof(chunk_id));
        std::uint32_t written = 1;
        mem_.store(out_addr, &written, sizeof(written));
        return 0;
    }
    if (n == "scePlayGoGetLocus") {
        auto handle = static_cast<std::int32_t>(a(0));
        auto ids_addr = a(1);
        auto count = static_cast<std::uint32_t>(a(2));
        auto loci_addr = a(3);
        if (handle != 1) return 0x80B20009;
        if (!ids_addr || !loci_addr) return 0x80B2000A;
        if (!count) return 0x80B2000B;
        std::vector<std::int8_t> loci(count, 3); // 3 = LocalFast (instalado localmente)
        mem_.store(loci_addr, loci.data(), loci.size());
        return 0;
    }
    if (n == "scePlayGoSetInstallSpeed") {
        return 0;
    }

    // libSceDiscMap HLE
    if (n == "sceDiscMapIsRequestOnHDD" || n == "sceDiscMap_8A828CAEE7EDD5E9") {
        return 0x81100004; // DISC_MAP_NO_BITMAP (jogo totalmente instalado no HDD/armazenamento)
    }

    // libSceMouse HLE
    if (n == "sceMouseInit") return 0;
    if (n == "sceMouseOpen") return 1;
    if (n == "sceMouseRead") {
        auto data_addr = a(1);
        auto count = a(2);
        if (!data_addr || count < 1) return 0x80DF0001;
        std::uint8_t empty_mouse[40] = {};
        mem_.store(data_addr, empty_mouse, sizeof(empty_mouse));
        return 1;
    }
    if (n == "sceMouseClose") return 0;

    // libSceAudioIn HLE
    if (n == "sceAudioInOpen" || n == "sceAudioInInput" || n == "sceAudioInClose") {
        return 0x80260109; // AUDIO_IN_NOT_OPENED
    }

    // libSceVoice HLE
    if (n == "sceVoiceInit" || n == "sceVoiceEnd" || n == "sceVoiceDeletePort" ||
        n == "sceVoiceConnectIPortToOPort" || n == "sceVoiceDisconnectIPortFromOPort" ||
        n == "sceVoiceStart" || n == "sceVoiceStop" || n == "sceVoiceReadFromOPort" ||
        n == "sceVoiceWriteToIPort") {
        return 0;
    }
    if (n == "sceVoiceCreatePort") {
        if (a(0)) put<std::int32_t>(mem_, a(0), 1);
        return 0;
    }
    if (n == "sceVoiceGetPortInfo") {
        if (a(1)) {
            std::uint8_t info[40] = {};
            mem_.store(a(1), info, sizeof(info));
        }
        return 0;
    }

    if (n == "sceAjmInitialize") {
        if (a(1)) put<std::int32_t>(mem_, a(1), 1);
        else if (a(0)) put<std::int32_t>(mem_, a(0), 1);
        return 0;
    }
    if (n == "sceAjmFinalize") {
        return 0;
    }
    if (n == "sceAjmModuleRegister" || n == "sceAjmModuleUnregister") {
        return 0;
    }
    if (n == "sceAjmInstanceCreate") {
        if (a(2)) put<std::int32_t>(mem_, a(2), 1);
        else if (a(1)) put<std::int32_t>(mem_, a(1), 1);
        return 0;
    }
    if (n == "sceAjmInstanceDestroy") {
        return 0;
    }
    if (n == "sceAjmBatchStartBuffer") {
        if (a(3)) put<std::int32_t>(mem_, a(3), 1);
        return 0;
    }
    if (n == "sceAjmBatchWait" || n == "sceAjmBatchCancel" ||
        n == "sceAjmBatchJobRunBufferRa" || n == "sceAjmBatchJobControlBufferRa" ||
        n == "sceAjmBatchErrorDump") {
        return 0;
    }

    // libSceNpTrophy HLE
    if (n == "sceNpTrophyCreateContext" || n == "sceNpTrophyCreateHandle") {
        if (a(0)) put<std::int32_t>(mem_, a(0), 1);
        return 0;
    }
    if (n == "sceNpTrophyRegisterContext" || n == "sceNpTrophyUnlockTrophy") {
        return 0;
    }
    if (n == "sceNpTrophyGetGameInfo" || n == "sceNpTrophyGetTrophyInfo") {
        auto ptr = (n == "sceNpTrophyGetGameInfo") ? a(2) : a(3);
        if (ptr) {
            std::uint8_t info[64] = {};
            mem_.store(ptr, info, sizeof(info));
        }
        return 0;
    }

    // libSceNet / libSceNetCtl / libSceSsl / libSceHttp HLE
    if (n == "sceNetInit" || n == "sceNetTerm") return 0;
    if (n == "sceNetErrnoLoc") return 0;
    if (n == "sceNetPoolCreate" || n == "sceNetEpollCreate" || n == "sceNetResolverCreate") return 1;
    if (n == "sceNetPoolDestroy" || n == "sceNetEpollDestroy" || n == "sceNetResolverDestroy") return 0;
    if (n == "sceNetHtons" || n == "sceNetNtohs") {
        return static_cast<std::uint16_t>(((a(0) & 0xff) << 8) | ((a(0) >> 8) & 0xff));
    }
    if (n == "sceNetHtonl" || n == "sceNetNtohl") {
        auto v = static_cast<std::uint32_t>(a(0));
        return ((v & 0xff) << 24) | (((v >> 8) & 0xff) << 16) | (((v >> 16) & 0xff) << 8) | ((v >> 24) & 0xff);
    }
    if (n == "sceNetInetPton" || n == "sceNetInetNtop") return 0;
    if (n == "sceNetSocket" || n == "sceNetConnect" || n == "sceNetBind" || n == "sceNetListen" ||
        n == "sceNetAccept" || n == "sceNetSend" || n == "sceNetSendto" || n == "sceNetRecv" ||
        n == "sceNetRecvfrom" || n == "sceNetSetsockopt" || n == "sceNetGetsockopt" ||
        n == "sceNetGetsockname" || n == "sceNetShutdown" || n == "sceNetSocketClose" ||
        n == "sceNetSocketAbort" || n == "sceNetEpollControl" || n == "sceNetEpollWait" ||
        n == "sceNetEpollAbort" || n == "sceNetResolverStartNtoa" || n == "sceNetResolverStartAton") {
        return 0x80410133; // NET_ENETUNREACH
    }
    if (n == "sceNetCtlGetState") {
        if (a(0)) put<std::int32_t>(mem_, a(0), 0); // Desconectado
        return 0;
    }
    if (n == "sceNetCtlGetInfo") return 0x80412108; // NET_CTL_NOT_CONNECTED
    if (n == "sceNetCtlRegisterCallback") {
        if (a(2)) put<std::int32_t>(mem_, a(2), 1);
        return 0;
    }
    if (n == "sceNetCtlCheckCallback" || n == "sceNetCtlUnregisterCallback") return 0;
    if (n == "sceNetCtlGetNatInfo") {
        if (a(0)) {
            std::uint32_t nat[4] = {0, 0, 3, 0};
            mem_.store(a(0), nat, sizeof(nat));
        }
        return 0;
    }
    if (n == "sceSslInit") return 1;
    if (n == "sceSslTerm") return 0;
    if (n == "sceHttpInit" || n == "sceHttpCreateTemplate" || n == "sceHttpCreateConnectionWithURL" ||
        n == "sceHttpCreateRequestWithURL" || n == "sceHttpCreateEpoll") return 1;
    if (n == "sceHttpTerm" || n == "sceHttpDeleteTemplate" || n == "sceHttpSetNonblock" ||
        n == "sceHttpSetConnectTimeOut" || n == "sceHttpsEnableOption" || n == "sceHttpsDisableOption" ||
        n == "sceHttpAddRequestHeader" || n == "sceHttpSetRequestContentLength" ||
        n == "sceHttpDeleteConnection" || n == "sceHttpDeleteRequest" || n == "sceHttpAbortWaitRequest" ||
        n == "sceHttpDestroyEpoll" || n == "sceHttpSetEpoll" || n == "sceHttpUnsetEpoll") return 0;
    if (n == "sceHttpSendRequest" || n == "sceHttpWaitRequest" || n == "sceHttpGetStatusCode" ||
        n == "sceHttpGetResponseContentLength" || n == "sceHttpReadData") return 0x80431063; // HTTP_NETWORK

    // libSceNp / libSceNpWebApi / libSceNpMatching2 / libSceNpSignaling / libSceNpScore HLE
    if (n == "sceNpGetState") {
        if (a(1)) put<std::int32_t>(mem_, a(1), 0); // signed out
        return 0;
    }
    if (n == "sceNpGetOnlineId" || n == "sceNpGetNpId" || n == "sceNpCheckNpAvailability" ||
        n == "sceNpGetParentalControlInfo" || n == "sceNpCheckPlus" || n == "sceNpGetGamePresenceStatus") {
        return 0x80550006; // NP_SIGNED_OUT
    }
    if (n == "sceNpRegisterStateCallback" || n == "sceNpRegisterPlusEventCallback") {
        if (a(2)) put<std::int32_t>(mem_, a(2), 1);
        return 0;
    }
    if (n == "sceNpUnregisterStateCallback" || n == "sceNpRegisterGamePresenceCallback" ||
        n == "sceNpUnregisterPlusEventCallback" || n == "sceNpCheckCallback" ||
        n == "sceNpSetNpTitleId" || n == "sceNpNotifyPlusFeature" || n == "sceNpSetContentRestriction" ||
        n == "sceNpDeleteRequest" || n == "sceNpAbortRequest" || n == "sceNpPollAsync") return 0;
    if (n == "sceNpCreateAsyncRequest") return 1;
    if (n == "sceNpCmpNpId" || n == "sceNpCmpOnlineId") return 0;
    if (n == "sceNpAuthCreateAsyncRequest") return 1;
    if (n == "sceNpAuthDeleteRequest" || n == "sceNpAuthPollAsync") return 0;
    if (n == "sceNpAuthGetAuthorizationCode") return 0x80550006;
    if (n == "sceNpLookupCreateTitleCtx" || n == "sceNpLookupCreateAsyncRequest") return 1;
    if (n == "sceNpLookupDeleteTitleCtx" || n == "sceNpLookupDeleteRequest" ||
        n == "sceNpLookupAbortRequest" || n == "sceNpLookupPollAsync") return 0;
    if (n == "sceNpLookupNpId") return 0x80550006;
    if (n == "sceNpScoreCreateNpTitleCtx" || n == "sceNpScoreCreateRequest") return 1;
    if (n == "sceNpScoreDeleteNpTitleCtx" || n == "sceNpScoreDeleteRequest" ||
        n == "sceNpScoreAbortRequest" || n == "sceNpScoreSetPlayerCharacterId") return 0;
    if (n == "sceNpScoreCensorComment" || n == "sceNpScoreSanitizeComment" ||
        n == "sceNpScoreGetBoardInfo" || n == "sceNpScoreGetGameData" ||
        n == "sceNpScoreGetRankingByNpIdPcId" || n == "sceNpScoreGetRankingByRange" ||
        n == "sceNpScoreRecordGameData" || n == "sceNpScoreRecordScore") return 0x80550006;
    if (n == "sceNpWebApiInitialize") return 1;
    if (n == "sceNpWebApiTerminate" || n == "sceNpWebApiDeletePushEventFilter" ||
        n == "sceNpWebApiDeleteContext" || n == "sceNpWebApiDeleteRequest" ||
        n == "sceNpWebApiAbortRequest" || n == "sceNpWebApiUnregisterPushEventCallback") return 0;
    if (n == "sceNpWebApiCreateContext" || n == "sceNpWebApiCreateRequest" ||
        n == "sceNpWebApiSendRequest" || n == "sceNpWebApiReadData" ||
        n == "sceNpWebApiGetHttpStatusCode" || n == "sceNpWebApiGetHttpResponseHeaderValue" ||
        n == "sceNpWebApiGetHttpResponseHeaderValueLength" || n == "sceNpWebApiCreatePushEventFilter" ||
        n == "sceNpWebApiRegisterPushEventCallback" || n == "sceNpWebApiUtilityParseNpId") return 0x80550006;
    if (n == "sceNpMatching2Initialize" || n == "sceNpMatching2Terminate" ||
        n == "sceNpMatching2ContextStop" || n == "sceNpMatching2DestroyContext" ||
        n == "sceNpMatching2RegisterContextCallback" || n == "sceNpMatching2RegisterLobbyEventCallback" ||
        n == "sceNpMatching2RegisterRoomEventCallback" || n == "sceNpMatching2RegisterSignalingCallback" ||
        n == "sceNpMatching2SetDefaultRequestOptParam") return 0;
    if (n == "sceNpMatching2CreateContext" || n == "sceNpMatching2ContextStart" ||
        n == "sceNpMatching2CreateJoinRoom" || n == "sceNpMatching2JoinRoom" ||
        n == "sceNpMatching2LeaveRoom" || n == "sceNpMatching2SearchRoom" ||
        n == "sceNpMatching2GetServerId" || n == "sceNpMatching2GetWorldInfoList" ||
        n == "sceNpMatching2GetLobbyInfoList" || n == "sceNpMatching2JoinLobby" ||
        n == "sceNpMatching2LeaveLobby" || n == "sceNpMatching2GrantRoomOwner" ||
        n == "sceNpMatching2KickoutRoomMember" || n == "sceNpMatching2SetRoomDataExternal" ||
        n == "sceNpMatching2SetRoomDataInternal" || n == "sceNpMatching2SetRoomMemberDataInternal" ||
        n == "sceNpMatching2SignalingGetConnectionStatus" || n == "sceNpMatching2SignalingGetPingInfo") return 0x80550006;
    if (n == "sceNpSignalingInitialize" || n == "sceNpSignalingTerminate" ||
        n == "sceNpSignalingDeleteContext" || n == "sceNpSignalingDeactivateConnection") return 0;
    if (n == "sceNpSignalingCreateContext" || n == "sceNpSignalingActivateConnection" ||
        n == "sceNpSignalingGetConnectionStatus") return 0x80550006;
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
    auto fill_stat = [&](const std::string& host_path, SceKernelStat& sb) -> bool {
        std::error_code ec;
        auto status = std::filesystem::status(host_path, ec);
        if (ec || !std::filesystem::exists(status)) return false;
        sb = {};
        sb.st_nlink = 1;
        if (std::filesystem::is_directory(status)) {
            sb.st_mode = 0040777; // S_IFDIR | 0777
            sb.st_size = 65536;
            sb.st_blksize = 65536;
            sb.st_blocks = 128;
        } else {
            sb.st_mode = 0100777; // S_IFREG | 0777
            sb.st_size = static_cast<std::int64_t>(std::filesystem::file_size(host_path, ec));
            sb.st_blksize = 4096;
            sb.st_blocks = (sb.st_size + 511) / 512;
        }
        auto ftime = std::filesystem::last_write_time(host_path, ec);
        if (!ec) {
            auto sctp = std::chrono::time_point_cast<std::chrono::nanoseconds>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            auto secs = std::chrono::time_point_cast<std::chrono::seconds>(sctp);
            auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(sctp - secs);
            sb.st_mtime_sec = static_cast<std::int64_t>(secs.time_since_epoch().count());
            sb.st_mtime_nsec = static_cast<std::int64_t>(nsecs.count());
            sb.st_atime_sec = sb.st_mtime_sec;
            sb.st_ctime_sec = sb.st_mtime_sec;
            sb.st_birthtime_sec = sb.st_mtime_sec;
        }
        return true;
    };

    // Kernel & libc filesystem operations
    if (n == "sceKernelStat" || n == "stat") {
        if (!a(0) || !a(1)) return (n == "stat") ? static_cast<std::uint64_t>(-1) : 0x80020016;
        auto path = read_guest_string(a(0));
        auto host_path = resolve_guest_path(path);
        SceKernelStat sb{};
        if (!fill_stat(host_path, sb)) {
            return (n == "stat") ? static_cast<std::uint64_t>(-1) : 0x80020002; // ENOENT
        }
        if (mem_.store(a(1), &sb, sizeof(sb)) != MemoryError::ok) {
            return (n == "stat") ? static_cast<std::uint64_t>(-1) : 0x80020016;
        }
        return 0;
    }

    if (n == "sceKernelFstat" || n == "fstat") {
        int fd = static_cast<int>(a(0));
        if (!a(1)) return (n == "fstat") ? static_cast<std::uint64_t>(-1) : 0x80020016;
        auto it = kernel_files_.find(fd);
        if (it == kernel_files_.end()) {
            return (n == "fstat") ? static_cast<std::uint64_t>(-1) : 0x80020009; // EBADF
        }
        SceKernelStat sb{};
        if (!fill_stat(it->second.path, sb)) {
            sb.st_mode = 0100777;
            sb.st_size = static_cast<std::int64_t>(it->second.size);
            sb.st_blksize = 4096;
            sb.st_blocks = (sb.st_size + 511) / 512;
            sb.st_nlink = 1;
        }
        if (mem_.store(a(1), &sb, sizeof(sb)) != MemoryError::ok) {
            return (n == "fstat") ? static_cast<std::uint64_t>(-1) : 0x80020016;
        }
        return 0;
    }

    if (n == "sceKernelOpen" || n == "open") {
        if (!a(0)) return (n == "open") ? static_cast<std::uint64_t>(-1) : 0x80020016;
        auto path = read_guest_string(a(0));
        auto host_path = resolve_guest_path(path);
        int flags = static_cast<int>(a(1));
        std::printf("[FILE] %s: '%s' -> '%s' (flags=0x%x)\n", n.c_str(), path.c_str(), host_path.c_str(), flags);
        
        const char* fmode = "rb";
        if (flags & 0x0200) { // O_CREAT
            if (flags & 0x0400) fmode = (flags & 0x0002) ? "w+b" : "wb"; // O_TRUNC
            else if (flags & 0x0008) fmode = (flags & 0x0002) ? "a+b" : "ab"; // O_APPEND
            else fmode = (flags & 0x0002) ? "r+b" : "wb";
        } else if (flags & 0x0008) { // O_APPEND
            fmode = (flags & 0x0002) ? "a+b" : "ab";
        } else if ((flags & 0x0003) == 0x0002) { // O_RDWR
            fmode = "r+b";
        } else if ((flags & 0x0003) == 0x0001) { // O_WRONLY
            fmode = "wb";
        } else {
            fmode = "rb";
        }

        FILE* fp = portable_fopen(host_path, fmode);
        if (!fp && (flags & 0x0200)) {
            fp = portable_fopen(host_path, "w+b");
        }
        if (!fp) {
            return (n == "open") ? static_cast<std::uint64_t>(-1) : 0x80020002; // ENOENT
        }

        std::uint64_t fsz = 0;
        std::error_code ec;
        if (std::filesystem::exists(host_path, ec) && !std::filesystem::is_directory(host_path, ec)) {
            fsz = std::filesystem::file_size(host_path, ec);
        }

        int fd = next_kernel_fd_++;
        kernel_files_[fd] = KernelFileEntry{fp, host_path, flags, fsz};
        std::printf("[FILE] %s: '%s' -> fd=%d size=%llu\n", n.c_str(), path.c_str(), fd, (unsigned long long)fsz);
        return static_cast<std::uint64_t>(fd);
    }

    if (n == "sceKernelClose" || n == "close") {
        int fd = static_cast<int>(a(0));
        auto it = kernel_files_.find(fd);
        if (it == kernel_files_.end()) {
            std::printf("[FILE] %s: fd=%d EBADF\n", n.c_str(), fd);
            return (n == "close") ? static_cast<std::uint64_t>(-1) : 0x80020009; // EBADF
        }
        std::printf("[FILE] %s: fd=%d ('%s')\n", n.c_str(), fd, it->second.path.c_str());
        if (it->second.fp) std::fclose(it->second.fp);
        kernel_files_.erase(it);
        return 0;
    }

    if (n == "sceKernelRead" || n == "read" || n == "_read") {
        int fd = static_cast<int>(a(0));
        std::uint64_t buf = a(1);
        std::size_t nbyte = static_cast<std::size_t>(a(2));
        auto it = kernel_files_.find(fd);
        if (it != kernel_files_.end()) {
            std::printf("[FILE] %s: fd=%d ('%s') nbyte=%zu\n", n.c_str(), fd, it->second.path.c_str(), nbyte);
        }
        if (!buf) return (n == "sceKernelRead") ? 0x80020016 : static_cast<std::uint64_t>(-1);
        if (it == kernel_files_.end() || !it->second.fp) {
            return (n == "sceKernelRead") ? 0x80020009 : static_cast<std::uint64_t>(-1);
        }
        constexpr std::size_t max_chunk = 64ull * 1024 * 1024;
        std::size_t to_read = std::min(nbyte, max_chunk);
        std::vector<std::uint8_t> tmp(to_read);
        std::size_t bytes_read = std::fread(tmp.data(), 1, to_read, it->second.fp);
        if (bytes_read > 0) {
            if (mem_.store(buf, tmp.data(), bytes_read) != MemoryError::ok) {
                return (n == "sceKernelRead") ? 0x80020016 : static_cast<std::uint64_t>(-1);
            }
        }
        return static_cast<std::uint64_t>(bytes_read);
    }

    if (n == "sceKernelPread") {
        int fd = static_cast<int>(a(0));
        std::uint64_t buf = a(1);
        std::size_t nbyte = static_cast<std::size_t>(a(2));
        std::int64_t offset = static_cast<std::int64_t>(a(3));
        auto it = kernel_files_.find(fd);
        if (it != kernel_files_.end()) {
            std::printf("[FILE] sceKernelPread: fd=%d ('%s') off=%lld nbyte=%zu\n",
                        fd, it->second.path.c_str(), (long long)offset, nbyte);
        }
        if (!buf) return 0x80020016;
        if (it == kernel_files_.end() || !it->second.fp) {
            return 0x80020009;
        }
        auto fp = it->second.fp;
#if defined(_WIN32)
        std::int64_t cur = _ftelli64(fp);
        if (_fseeki64(fp, offset, SEEK_SET) != 0) return 0x80020016;
#else
        off_t cur = ftello(fp);
        if (fseeko(fp, offset, SEEK_SET) != 0) return 0x80020016;
#endif
        constexpr std::size_t max_chunk = 64ull * 1024 * 1024;
        std::size_t to_read = std::min(nbyte, max_chunk);
        std::vector<std::uint8_t> tmp(to_read);
        std::size_t bytes_read = std::fread(tmp.data(), 1, to_read, fp);
        if (bytes_read > 0) {
            if (mem_.store(buf, tmp.data(), bytes_read) != MemoryError::ok) {
#if defined(_WIN32)
                _fseeki64(fp, cur, SEEK_SET);
#else
                fseeko(fp, cur, SEEK_SET);
#endif
                return 0x80020016;
            }
        }
#if defined(_WIN32)
        _fseeki64(fp, cur, SEEK_SET);
#else
        fseeko(fp, cur, SEEK_SET);
#endif
        return static_cast<std::uint64_t>(bytes_read);
    }

    if (n == "sceKernelWrite" || n == "write" || n == "_write") {
        int fd = static_cast<int>(a(0));
        std::uint64_t buf = a(1);
        std::size_t nbyte = static_cast<std::size_t>(a(2));
        if (!buf) return (n == "sceKernelWrite") ? 0x80020016 : static_cast<std::uint64_t>(-1);
        if (fd == 1 || fd == 2) {
            std::vector<char> tmp(nbyte + 1, 0);
            if (mem_.load(buf, tmp.data(), nbyte) == MemoryError::ok) {
                std::fwrite(tmp.data(), 1, nbyte, (fd == 2) ? stderr : stdout);
                std::fflush((fd == 2) ? stderr : stdout);
            }
            return nbyte;
        }
        auto it = kernel_files_.find(fd);
        if (it == kernel_files_.end() || !it->second.fp) {
            return (n == "sceKernelWrite") ? 0x80020009 : static_cast<std::uint64_t>(-1);
        }
        constexpr std::size_t max_chunk = 64ull * 1024 * 1024;
        std::size_t to_write = std::min(nbyte, max_chunk);
        std::vector<std::uint8_t> tmp(to_write);
        if (mem_.load(buf, tmp.data(), to_write) != MemoryError::ok) {
            return (n == "sceKernelWrite") ? 0x80020016 : static_cast<std::uint64_t>(-1);
        }
        std::size_t bytes_written = std::fwrite(tmp.data(), 1, to_write, it->second.fp);
        return static_cast<std::uint64_t>(bytes_written);
    }

    if (n == "sceKernelPwrite") {
        int fd = static_cast<int>(a(0));
        std::uint64_t buf = a(1);
        std::size_t nbyte = static_cast<std::size_t>(a(2));
        std::int64_t offset = static_cast<std::int64_t>(a(3));
        if (!buf) return 0x80020016;
        auto it = kernel_files_.find(fd);
        if (it == kernel_files_.end() || !it->second.fp) {
            return 0x80020009;
        }
        auto fp = it->second.fp;
#if defined(_WIN32)
        std::int64_t cur = _ftelli64(fp);
        if (_fseeki64(fp, offset, SEEK_SET) != 0) return 0x80020016;
#else
        off_t cur = ftello(fp);
        if (fseeko(fp, offset, SEEK_SET) != 0) return 0x80020016;
#endif
        constexpr std::size_t max_chunk = 64ull * 1024 * 1024;
        std::size_t to_write = std::min(nbyte, max_chunk);
        std::vector<std::uint8_t> tmp(to_write);
        if (mem_.load(buf, tmp.data(), to_write) != MemoryError::ok) {
#if defined(_WIN32)
            _fseeki64(fp, cur, SEEK_SET);
#else
            fseeko(fp, cur, SEEK_SET);
#endif
            return 0x80020016;
        }
        std::size_t written = std::fwrite(tmp.data(), 1, to_write, fp);
#if defined(_WIN32)
        _fseeki64(fp, cur, SEEK_SET);
#else
        fseeko(fp, cur, SEEK_SET);
#endif
        return static_cast<std::uint64_t>(written);
    }

    if (n == "sceKernelLseek" || n == "lseek") {
        int fd = static_cast<int>(a(0));
        std::int64_t offset = static_cast<std::int64_t>(a(1));
        int whence = static_cast<int>(a(2));
        auto it = kernel_files_.find(fd);
        if (it == kernel_files_.end() || !it->second.fp) {
            return (n == "lseek") ? static_cast<std::uint64_t>(-1) : 0x80020009;
        }
#if defined(_WIN32)
        if (_fseeki64(it->second.fp, offset, whence) != 0) {
            return (n == "lseek") ? static_cast<std::uint64_t>(-1) : 0x80020016;
        }
        return static_cast<std::uint64_t>(_ftelli64(it->second.fp));
#else
        if (fseeko(it->second.fp, offset, whence) != 0) {
            return (n == "lseek") ? static_cast<std::uint64_t>(-1) : 0x80020016;
        }
        return static_cast<std::uint64_t>(ftello(it->second.fp));
#endif
    }

    if (n == "sceKernelFtruncate" || n == "ftruncate") {
        int fd = static_cast<int>(a(0));
        std::int64_t length = static_cast<std::int64_t>(a(1));
        auto it = kernel_files_.find(fd);
        if (it == kernel_files_.end() || !it->second.fp) {
            return (n == "ftruncate") ? static_cast<std::uint64_t>(-1) : 0x80020009;
        }
#if defined(_WIN32)
        int win_fd = _fileno(it->second.fp);
        if (win_fd >= 0) {
            _chsize_s(win_fd, length);
        }
#else
        int posix_fd = fileno(it->second.fp);
        if (posix_fd >= 0) {
            if (ftruncate(posix_fd, length) != 0) {
                // Truncate failed
            }
        }
#endif
        return 0;
    }

    if (n == "sceKernelCheckReachability") {
        if (!a(0)) return 0x80020016;
        auto path = read_guest_string(a(0));
        auto host_path = resolve_guest_path(path);
        std::error_code ec;
        if (!std::filesystem::exists(host_path, ec)) {
            return 0x80020002; // ENOENT
        }
        return 0;
    }

    if (n == "mkdir") {
        if (!a(0)) return static_cast<std::uint64_t>(-1);
        auto path = read_guest_string(a(0));
        auto host_path = resolve_guest_path(path);
        std::error_code ec;
        std::filesystem::create_directories(host_path, ec);
        return ec ? static_cast<std::uint64_t>(-1) : 0;
    }

    if (n == "rename") {
        if (!a(0) || !a(1)) return static_cast<std::uint64_t>(-1);
        auto old_p = resolve_guest_path(read_guest_string(a(0)));
        auto new_p = resolve_guest_path(read_guest_string(a(1)));
        std::error_code ec;
        std::filesystem::rename(old_p, new_p, ec);
        return ec ? static_cast<std::uint64_t>(-1) : 0;
    }

    auto get_file = [&](std::uint64_t handle) -> FILE* {
        auto it = open_files_.find(handle);
        return it != open_files_.end() ? it->second : nullptr;
    };
    if (n=="fopen") {
        std::string path = read_guest_string(a(0));
        std::string mode = read_guest_string(a(1));
        if (path.empty() || mode.empty() || path.find("..") != std::string::npos) return 0;
        auto host_path = resolve_guest_path(path);
        FILE* fp = portable_fopen(host_path, mode.c_str());
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
        int ch = static_cast<int>(a(0));
        return std::fputc(ch, stdout);
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
        int ch = static_cast<int>(a(0));
        FILE* fp = get_file(a(1));
        return fp ? std::fputc(ch, fp) : EOF;
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
    if (n=="pthread_setname_np" || n=="scePthreadRename" || n=="scePthreadSetName") {
        return 0;
    }
    if (n=="pthread_getname_np") {
        if (a(1) && a(2) > 0) {
            std::string name = "guest_thread";
            std::size_t len = std::min<std::size_t>(name.size(), a(2) - 1);
            mem_.store(a(1), name.c_str(), len);
            char zero = 0;
            mem_.store(a(1) + len, &zero, 1);
        }
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
    if (n=="sigprocmask" || n=="_sigprocmask") {
        auto how = a(0);
        auto set = a(1);
        auto old = a(2);
        static std::uint32_t current_mask[4] = {0, 0, 0, 0};
        if (old) {
            mem_.store(old, current_mask, sizeof(current_mask));
        }
        if (set) {
            std::uint32_t new_mask[4] = {};
            if (mem_.load(set, new_mask, sizeof(new_mask)) == MemoryError::ok) {
                for (int i = 0; i < 4; ++i) {
                    if (how == 1) current_mask[i] |= new_mask[i]; // SIG_BLOCK
                    else if (how == 2) current_mask[i] &= ~new_mask[i]; // SIG_UNBLOCK
                    else if (how == 3) current_mask[i] = new_mask[i]; // SIG_SETMASK
                }
            }
        }
        return 0;
    }
    if (n=="getrusage") {
        (void)a(0); // who
        auto out = a(1);
        if (!out) return 0x16; // EINVAL
        std::uint8_t usage[144] = {};
        mem_.store(out, usage, sizeof(usage));
        return 0;
    }
    if (n=="sysctl") {
        auto name_ptr = a(0);
        auto namelen = a(1);
        auto old_ptr = a(2);
        auto oldlen_ptr = a(3);
        if (!name_ptr || namelen < 2) return 0x16; // EINVAL
        std::int32_t name[2] = {};
        if (mem_.load(name_ptr, name, sizeof(name)) == MemoryError::ok) {
            if (name[0] == 1 && name[1] == 37) { // CTL_KERN, KERN_ARANDOM
                if (old_ptr && oldlen_ptr) {
                    std::uint64_t len = 0;
                    if (mem_.load(oldlen_ptr, &len, sizeof(len)) == MemoryError::ok && len > 0) {
                        std::vector<std::uint8_t> rand_data(len);
                        for (std::size_t i = 0; i < len; ++i) rand_data[i] = static_cast<std::uint8_t>(std::rand() & 0xFF);
                        mem_.store(old_ptr, rand_data.data(), len);
                        return 0;
                    }
                }
            }
        }
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
    if (n=="gmtime_s") {
        uint64_t p0 = a(0), p1 = a(1);
        if (!p0 || !p1) return invalid;
        uint64_t timer_ptr = p0, result_ptr = p1;
        std::time_t t0 = 0, t1 = 0;
        mem_.load(p0, &t0, sizeof(t0));
        mem_.load(p1, &t1, sizeof(t1));
        if (t1 > 1000000000LL && t1 < 2500000000LL) {
            timer_ptr = p1;
            result_ptr = p0;
        } else {
            timer_ptr = p0;
            result_ptr = p1;
        }
        std::time_t t = 0;
        mem_.load(timer_ptr, &t, sizeof(t));
        std::tm* tm = std::gmtime(&t);
        if (tm) {
            put<std::int32_t>(mem_, result_ptr + 0, tm->tm_sec);
            put<std::int32_t>(mem_, result_ptr + 4, tm->tm_min);
            put<std::int32_t>(mem_, result_ptr + 8, tm->tm_hour);
            put<std::int32_t>(mem_, result_ptr + 12, tm->tm_mday);
            put<std::int32_t>(mem_, result_ptr + 16, tm->tm_mon);
            put<std::int32_t>(mem_, result_ptr + 20, tm->tm_year);
            put<std::int32_t>(mem_, result_ptr + 24, tm->tm_wday);
            put<std::int32_t>(mem_, result_ptr + 28, tm->tm_yday);
            put<std::int32_t>(mem_, result_ptr + 32, tm->tm_isdst);
        }
        return 0;
    }
    if (n=="sceDiscMapUnknownIoKM" || n=="ioKMruft1ek" || n=="sceDiscMapIsRequestOnHDD") {
        if (n == "sceDiscMapIsRequestOnHDD") {
            std::uint64_t ret_ptr = a(3);
            if (ret_ptr) put<std::int32_t>(mem_, ret_ptr, 1);
        } else {
            std::uint64_t flags_ptr = a(3);
            std::uint64_t ret1_ptr = a(4);
            std::uint64_t ret2_ptr = a(5);
            if (flags_ptr) put<std::int32_t>(mem_, flags_ptr, 0);
            if (ret1_ptr) put<std::int32_t>(mem_, ret1_ptr, 0);
            if (ret2_ptr) put<std::int32_t>(mem_, ret2_ptr, 0);
        }
        return 0;
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
        long pos = std::ftell(fp);
        if (pos >= 0 && a(1)) put<std::int64_t>(mem_, a(1), static_cast<std::int64_t>(pos));
        return pos >= 0 ? 0 : static_cast<std::uint64_t>(-1);
    }
    if (n=="fsetpos") {
        FILE* fp = get_file(a(0));
        if (!fp) return static_cast<std::uint64_t>(-1);
        std::int64_t pos = a(1) ? get<std::int64_t>(mem_, a(1)) : 0;
        return std::fseek(fp, static_cast<long>(pos), SEEK_SET);
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
        int ch = static_cast<int>(a(0));
        FILE* fp = get_file(a(1));
        return fp ? std::ungetc(ch, fp) : EOF;
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
