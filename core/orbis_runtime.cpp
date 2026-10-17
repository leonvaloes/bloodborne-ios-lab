#include "orbis_runtime.h"
#include "kernel_threads.h"
#include "dialog_hle.h"
#include "ajm_hle.h"
#include "app_content.h"
#include "gnm_context.h"
#include "vfs.h"
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

FILE* portable_freopen(const std::string& path, const char* mode, FILE* stream) {
#if defined(_WIN32)
    std::filesystem::path p = std::filesystem::u8path(path);
    std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfreopen(p.c_str(), wmode.c_str(), stream);
#else
    return std::freopen(path.c_str(), mode, stream);
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
    : mem_(memory), user_mgr_(), pad_mgr_(nullptr, &user_mgr_), gnm_context_(std::make_unique<GnmContext>(memory)),
      owned_vfs_(std::make_unique<VirtualFileSystem>()), vfs_(owned_vfs_.get()) {
    auto tmp_base = (std::filesystem::temp_directory_path() / "antigravity").lexically_normal();
    std::error_code ec;
    std::filesystem::create_directories(tmp_base, ec);
    vfs_->mount("/temp", tmp_base.string(), false);
    vfs_->mount("/tmp", tmp_base.string(), false);
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
        "snprintf", "vsnprintf", "sprintf", "vsprintf", "fprintf", "vfprintf",
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
std::uint64_t OrbisRuntime::get_static_libc_buf(std::size_t offset, std::size_t size) {
    (void)size;
    if (!static_libc_page_) {
        static_libc_page_ = next_virtual_;
        next_virtual_ += 0x10000;
        auto err = mem_.map(static_libc_page_, 0x10000, read | write);
        if (err != MemoryError::ok) {
            std::printf("[HLE] failed to map static libc page at 0x%llx\n", (unsigned long long)static_libc_page_);
        }
        mappings_[static_libc_page_] = 0x10000;
    }
    return static_libc_page_ + offset;
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

void OrbisRuntime::attach_vfs(VirtualFileSystem* vfs) {
    if (vfs) {
        vfs_ = vfs;
    } else {
        vfs_ = owned_vfs_.get();
    }
    if (vfs_ && !app0_dir_.empty()) {
        vfs_->mount("/app0", app0_dir_.string(), true);
    }
    if (vfs_ && !vfs_->is_mounted("/temp")) {
        auto tmp_base = (std::filesystem::temp_directory_path() / "antigravity").lexically_normal();
        std::error_code ec;
        std::filesystem::create_directories(tmp_base, ec);
        vfs_->mount("/temp", tmp_base.string(), false);
        vfs_->mount("/tmp", tmp_base.string(), false);
    }
}

void OrbisRuntime::set_app0_dir(const std::filesystem::path& dir) {
    app0_dir_ = dir;
    if (vfs_) {
        vfs_->mount("/app0", dir.string(), true);
    }
}

std::string OrbisRuntime::resolve_guest_virtual_path(const std::string& guest_path) const {
    if (guest_path.empty()) return "";
    std::string norm = guest_path;
    for (char& c : norm) {
        if (c == '\\') c = '/';
    }

    // Rejeição estrita de travessias com .. per FR-005
    if (VirtualFileSystem::contains_traversal(norm)) {
        return "";
    }

    // Apenas caminhos virtuais conhecidos podem ser absolutos; caminhos de host
    // (incluindo drive letters Windows) jamais são traduzidos para um mount.
    if (norm.find(':') != std::string::npos &&
        norm.rfind("app0:", 0) != 0 && norm.rfind("savedata:", 0) != 0 &&
        norm.rfind("temp:", 0) != 0) return "";

    if (norm.rfind("/app0/", 0) == 0 || norm == "/app0") {
        return norm;
    } else if (norm.rfind("app0:/", 0) == 0) {
        return "/app0/" + norm.substr(7);
    } else if (norm.rfind("app0:", 0) == 0) {
        return "/app0/" + norm.substr(5);
    } else if (norm == "app0") {
        return "/app0";
    } else if (norm.rfind("/savedata/", 0) == 0 || norm == "/savedata") {
        return norm;
    } else if (norm.rfind("savedata:/", 0) == 0) {
        return "/savedata/" + norm.substr(11);
    } else if (norm.rfind("savedata:", 0) == 0) {
        return "/savedata/" + norm.substr(9);
    } else if (norm.rfind("/temp/", 0) == 0) {
        return norm;
    } else if (norm.rfind("/tmp/", 0) == 0) {
        return "/temp/" + norm.substr(5);
    } else if (norm.rfind("temp:/", 0) == 0) {
        return "/temp/" + norm.substr(6);
    } else if (norm.rfind("/hostapp/", 0) == 0) {
        return norm;
    } else if (norm[0] != '/') {
        // Caminhos relativos de guest residem em /app0
        return "/app0/" + norm;
    } else {
        return norm;
    }
}

std::string OrbisRuntime::resolve_guest_path(const std::string& guest_path, bool* out_readonly) const {
    if (out_readonly) *out_readonly = false;
    if (guest_path.empty()) return "";
    std::string norm = guest_path;
    for (char& c : norm) {
        if (c == '\\') c = '/';
    }

    // Rejeição estrita de travessias com .. per FR-005
    if (VirtualFileSystem::contains_traversal(norm)) {
        return "";
    }

    auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
    if (!active_vfs) return "";

    if (!app0_dir_.empty() && !active_vfs->is_mounted("/app0")) {
        const_cast<VirtualFileSystem*>(active_vfs)->mount("/app0", app0_dir_.string(), true);
    }
    if (!active_vfs->is_mounted("/temp")) {
        auto tmp_base = (std::filesystem::temp_directory_path() / "antigravity").lexically_normal();
        std::error_code ec;
        std::filesystem::create_directories(tmp_base, ec);
        const_cast<VirtualFileSystem*>(active_vfs)->mount("/temp", tmp_base.string(), false);
        const_cast<VirtualFileSystem*>(active_vfs)->mount("/tmp", tmp_base.string(), false);
    }
    if (!active_vfs->is_mounted("/savedata")) {
        auto save_base = app0_dir_.empty() ? (std::filesystem::current_path() / "savedata")
                                           : (app0_dir_.parent_path() / "savedata");
        std::error_code ec;
        std::filesystem::create_directories(save_base, ec);
        const_cast<VirtualFileSystem*>(active_vfs)->mount("/savedata", save_base.string(), false);
    }

    std::string vpath = resolve_guest_virtual_path(norm);
    if (vpath.empty()) return "";

    std::string host_path;
    bool readonly = false;
    VfsError err = active_vfs->resolve(vpath, host_path, readonly);
    if (err == VfsError::ok) {
        if (out_readonly) *out_readonly = readonly;
        return host_path;
    }

    // Tentativas de escape ou caminhos fora dos mounts autorizados são rejeitados
    return "";
}

FILE* OrbisRuntime::get_file(std::uint64_t handle) const {
    if (handle == 1) return stdout;
    if (handle == 2) return stderr;
    auto it = open_files_.find(handle);
    return it != open_files_.end() ? it->second : nullptr;
}

bool OrbisRuntime::format_printf(const std::string& format,
                                const std::function<std::uint64_t()>& get_int_arg,
                                const std::function<double()>& get_float_arg,
                                std::string& out) {
    out.clear();
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
        ++i;
        std::string flags_width_prec;
        while (i < format.size() && (std::strchr("-+ #0123456789.", format[i]) != nullptr)) {
            flags_width_prec.push_back(format[i++]);
        }
        std::string length_mod;
        while (i < format.size() && (format[i] == 'l' || format[i] == 'z' || format[i] == 'h' || format[i] == 'j' || format[i] == 't')) {
            length_mod.push_back(format[i++]);
        }
        if (i >= format.size()) {
            return false;
        }
        char spec = format[i++];
        char piece[512] = {};
        if (spec == 'd' || spec == 'i') {
            std::int64_t val = static_cast<std::int64_t>(get_int_arg());
            if (length_mod == "hh") val = static_cast<signed char>(val);
            else if (length_mod == "h") val = static_cast<short>(val);
            else if (length_mod.empty()) val = static_cast<int>(val);
            std::string subspec = "%" + flags_width_prec + "lld";
            std::snprintf(piece, sizeof(piece), subspec.c_str(), static_cast<long long>(val));
            out.append(piece);
        } else if (spec == 'u') {
            std::uint64_t val = get_int_arg();
            if (length_mod == "hh") val = static_cast<unsigned char>(val);
            else if (length_mod == "h") val = static_cast<unsigned short>(val);
            else if (length_mod.empty()) val = static_cast<unsigned int>(val);
            std::string subspec = "%" + flags_width_prec + "llu";
            std::snprintf(piece, sizeof(piece), subspec.c_str(), static_cast<unsigned long long>(val));
            out.append(piece);
        } else if (spec == 'x' || spec == 'X') {
            std::uint64_t val = get_int_arg();
            if (length_mod == "hh") val = static_cast<unsigned char>(val);
            else if (length_mod == "h") val = static_cast<unsigned short>(val);
            else if (length_mod.empty()) val = static_cast<unsigned int>(val);
            std::string subspec = "%" + flags_width_prec + (spec == 'x' ? "llx" : "llX");
            std::snprintf(piece, sizeof(piece), subspec.c_str(), static_cast<unsigned long long>(val));
            out.append(piece);
        } else if (spec == 'o') {
            std::uint64_t val = get_int_arg();
            if (length_mod == "hh") val = static_cast<unsigned char>(val);
            else if (length_mod == "h") val = static_cast<unsigned short>(val);
            else if (length_mod.empty()) val = static_cast<unsigned int>(val);
            std::string subspec = "%" + flags_width_prec + "llo";
            std::snprintf(piece, sizeof(piece), subspec.c_str(), static_cast<unsigned long long>(val));
            out.append(piece);
        } else if (spec == 's') {
            std::uint64_t s_addr = get_int_arg();
            std::string s_val;
            if (s_addr) {
                for (std::size_t k = 0; k < 4096; ++k) {
                    char ch = 0;
                    if (mem_.load(s_addr + k, &ch, 1) != MemoryError::ok || ch == '\0') break;
                    s_val.push_back(ch);
                }
            } else {
                s_val = "(null)";
            }
            std::string subspec = "%" + flags_width_prec + "s";
            if (flags_width_prec.empty()) {
                out.append(s_val);
            } else {
                std::vector<char> s_piece(s_val.size() + 128);
                std::snprintf(s_piece.data(), s_piece.size(), subspec.c_str(), s_val.c_str());
                out.append(s_piece.data());
            }
        } else if (spec == 'f' || spec == 'F' || spec == 'g' || spec == 'G' || spec == 'e' || spec == 'E') {
            double d = get_float_arg();
            std::string subspec = "%" + flags_width_prec + spec;
            std::snprintf(piece, sizeof(piece), subspec.c_str(), d);
            out.append(piece);
        } else if (spec == 'p') {
            std::uint64_t ptr_val = get_int_arg();
            std::snprintf(piece, sizeof(piece), "%p", reinterpret_cast<void*>(static_cast<uintptr_t>(ptr_val)));
            out.append(piece);
        } else if (spec == 'c') {
            char c_val = static_cast<char>(get_int_arg());
            out.push_back(c_val);
        } else {
            return false;
        }
    }
    return true;
}

OrbisRuntime::InvocationResult OrbisRuntime::invoke_trap(const std::string& import, CpuState& cpu,
                                                           GuestScheduler::Id tid, std::uint64_t return_trap) {
    current_tid_ = tid;
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
            if (!at || at->stack_size < 0x10000 || at->stack_size > (mem_.budget() / 2) || (at->stack_size & 0xFFF) != 0) {
                return result(invalid);
            }
            thread_stack_size = static_cast<std::size_t>(at->stack_size);
        }
        if (!a(0) || !a(2) || a(2) < 0x1000 || !return_trap || thread_stack_next_ > UINT64_MAX - thread_stack_size) {
            return result(invalid);
        }

        // T179: Validar a saída guest antes de alocar pilha, TLS ou thread
        std::uint64_t out_probe = 0;
        if (mem_.load(a(0), &out_probe, sizeof(out_probe)) != MemoryError::ok ||
            mem_.store(a(0), &out_probe, sizeof(out_probe)) != MemoryError::ok) {
            return result(invalid);
        }

        // T180: Validar template TLS, DTV e TCB antes de mapear
        const std::uint64_t thread_tls_region = 0x20000; // 128 KB for TLS + TCB + Module TLS
        if (tls_template_.filesz > tls_template_.memsz) {
            return result(invalid);
        }
        std::uint64_t main_align = tls_template_.align ? tls_template_.align : 16;
        if ((main_align & (main_align - 1)) != 0 || main_align > 4096) {
            return result(invalid);
        }
        std::uint64_t main_tls_size = tls_template_.memsz ? (tls_template_.memsz + main_align - 1) & ~(main_align - 1) : 0x1000;
        if (main_tls_size < 0x1000) main_tls_size = 0x1000;
        if (main_tls_size + 1024 > thread_tls_region) {
            return result(invalid);
        }
        if (tls_template_.filesz > 0) {
            if (!tls_template_.address) return result(invalid);
            std::uint8_t probe_byte = 0;
            if (mem_.load(tls_template_.address, &probe_byte, 1) != MemoryError::ok ||
                mem_.load(tls_template_.address + tls_template_.filesz - 1, &probe_byte, 1) != MemoryError::ok) {
                return result(invalid);
            }
        }

        std::uint64_t simulated_mod_offset = (main_tls_size + 512 + 63) & ~static_cast<std::uint64_t>(63);
        for (const auto& mod : module_tls_templates_) {
            if (mod.filesz > mod.memsz) {
                return result(invalid);
            }
            std::uint64_t m_align = mod.align ? mod.align : 16;
            if ((m_align & (m_align - 1)) != 0 || m_align > 4096) {
                return result(invalid);
            }
            if (mod.filesz > 0) {
                if (!mod.address) return result(invalid);
                std::uint8_t probe_byte = 0;
                if (mem_.load(mod.address, &probe_byte, 1) != MemoryError::ok ||
                    mem_.load(mod.address + mod.filesz - 1, &probe_byte, 1) != MemoryError::ok) {
                    return result(invalid);
                }
            }
            if (!mod.module_id || mod.module_id > 64) continue;
            simulated_mod_offset = (simulated_mod_offset + m_align - 1) & ~(m_align - 1);
            std::uint64_t m_memsz = (mod.memsz + m_align - 1) & ~(m_align - 1);
            if (simulated_mod_offset + m_memsz > thread_tls_region) {
                return result(invalid);
            }
            simulated_mod_offset += m_memsz;
        }

        // T179: Mapeamento da pilha e TLS com reversão integral em falha
        const auto bottom = thread_stack_next_;
        auto err1 = mem_.map(bottom, thread_stack_size, read | write);
        if (err1 != MemoryError::ok) {
            return result(no_memory);
        }

        std::uint64_t thread_tls_base = next_virtual_;
        auto err2 = mem_.map(thread_tls_base, thread_tls_region, read | write);
        if (err2 != MemoryError::ok) {
            mem_.unmap(bottom);
            return result(no_memory);
        }
        next_virtual_ += thread_tls_region;
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

        std::uint64_t max_mod_id = 1;
        for (const auto& mod : module_tls_templates_) {
            if (mod.module_id > max_mod_id) max_mod_id = mod.module_id;
        }
        put<std::uint64_t>(mem_, thread_dtv, max_mod_id + 1);
        put<std::uint64_t>(mem_, thread_dtv + 8, thread_tls_base);

        std::uint64_t next_mod_tls = (thread_tcb + 512 + 63) & ~static_cast<std::uint64_t>(63);
        for (const auto& mod : module_tls_templates_) {
            if (!mod.module_id || mod.module_id > 64) continue;
            if (thread_dtv + 8 * mod.module_id >= thread_tcb + 256) continue;
            std::uint64_t align = mod.align ? mod.align : 16;
            next_mod_tls = (next_mod_tls + align - 1) & ~(align - 1);
            std::uint64_t memsz = (mod.memsz + align - 1) & ~(align - 1);
            if (next_mod_tls + memsz <= thread_tls_base + thread_tls_region) {
                if (mod.filesz && mod.address) {
                    std::vector<std::uint8_t> data(static_cast<std::size_t>(mod.filesz));
                    mem_.load(mod.address, data.data(), data.size());
                    mem_.store(next_mod_tls, data.data(), data.size());
                }
                put<std::uint64_t>(mem_, thread_dtv + 8 * mod.module_id, next_mod_tls);
                next_mod_tls += memsz;
            }
        }

        auto id = scheduler_->create(a(2), bottom, bottom + thread_stack_size, return_trap, a(3), thread_tcb);
        if (!id) {
            mem_.unmap(thread_tls_base);
            mappings_.erase(thread_tls_base);
            mem_.unmap(bottom);
            return result(invalid);
        }
        std::uint64_t thread_handle = id;
        if (mem_.store(a(0), &thread_handle, sizeof(thread_handle)) != MemoryError::ok) {
            scheduler_->abort_created(id);
            mem_.unmap(thread_tls_base);
            mappings_.erase(thread_tls_base);
            mem_.unmap(bottom);
            return result(invalid);
        }
        thread_stack_next_ += thread_stack_size;
        auto thread_name = (n == "pthread_create") ? "pthread" : (a(4) ? read_guest_string(a(4)) : std::string("unnamed"));
        auto start_res = scheduler_->start(id);
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
        if (target == tid) {
            return result(ORBIS_KERNEL_ERROR_EDEADLK);
        }
        if (!scheduler_->has_thread(target)) {
            return result(ORBIS_KERNEL_ERROR_ESRCH);
        }
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
        std::uint64_t dummy = 0;
        if (mem_.store(a(0), &dummy, sizeof(dummy)) != MemoryError::ok) {
            return result(invalid);
        }
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
        std::uint64_t handle_u64 = handle;
        if (mem_.store(a(0), &handle_u64, sizeof(handle_u64)) != MemoryError::ok) {
            scheduler_->mutex_delete(id);
            guest_mutexes_.erase(handle);
            return result(invalid);
        }
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
            if (scheduler_) {
                scheduler_->mutex_delete(id);
            }
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
        std::uint64_t dummy = 0;
        if (mem_.store(a(0), &dummy, sizeof(dummy)) != MemoryError::ok) {
            return result(invalid);
        }
        const auto handle = next_handle_++;
        const auto id = scheduler_->cond_create();
        guest_conds_[handle] = id;
        std::uint64_t handle_u64 = handle;
        if (mem_.store(a(0), &handle_u64, sizeof(handle_u64)) != MemoryError::ok) {
            scheduler_->cond_delete(id);
            guest_conds_.erase(handle);
            return result(invalid);
        }
        return result(0);
    }

    if (n == "scePthreadCondDestroy" || n == "pthread_cond_destroy") {
        if (!a(0)) return result(invalid);
        auto handle = get<std::uint64_t>(mem_, a(0));
        auto it = guest_conds_.find(handle);
        if (it == guest_conds_.end()) return result(invalid);
        if (scheduler_) {
            scheduler_->cond_delete(it->second);
        }
        guest_conds_.erase(it);
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
            std::uint64_t dummy = 0;
            if (mem_.store(a(0), &dummy, sizeof(dummy)) != MemoryError::ok) {
                return result(invalid);
            }
            const auto handle = static_cast<std::uint32_t>(next_handle_++);
            const auto id = scheduler_->event_create(a(3));
            guest_events_[handle] = id;
            std::uint64_t handle_u64 = handle;
            if (mem_.store(a(0), &handle_u64, sizeof(handle_u64)) != MemoryError::ok) {
                scheduler_->event_delete(id);
                guest_events_.erase(handle);
                return result(invalid);
            }
            return result(0);
        }
    }

    if (n == "sceKernelDeleteEventFlag") {
        auto handle = static_cast<std::uint32_t>(a(0));
        auto it = guest_events_.find(handle);
        if (it == guest_events_.end()) return result(0x80020003); // ESRCH
        if (scheduler_) {
            scheduler_->event_delete(it->second);
        }
        guest_events_.erase(it);
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

        if (!is_sdk) {
            std::uint32_t dummy = 0;
            if (mem_.store(out_id_addr, &dummy, sizeof(dummy)) != MemoryError::ok) {
                return result(invalid);
            }
        }

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
            if (mem_.store(out_id_addr, &handle, sizeof(handle)) != MemoryError::ok) {
                scheduler_->sema_delete(sid);
                guest_semas_.erase(handle);
                sema_records_.erase(handle);
                return result(invalid);
            }
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
        std::uint32_t key = static_cast<std::uint32_t>(a(0));
        auto err = scheduler_->tls_delete(key);
        if (err != ORBIS_OK) return result(invalid);
        return result(0);
    }

    if (n == "scePthreadSetspecific" || n == "pthread_setspecific") {
        std::uint32_t key = static_cast<std::uint32_t>(a(0));
        std::uint64_t value = a(1);
        auto status = scheduler_->tls_set(tid, key, value);
        if (status != ORBIS_OK) return result(invalid);
        return result(0);
    }

    if (n == "scePthreadGetspecific" || n == "pthread_getspecific") {
        std::uint32_t key = static_cast<std::uint32_t>(a(0));
        if (!scheduler_->tls_valid(key)) return result(0);
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

    if (n == "sceKernelUsleep" || n == "usleep") {
        std::uint64_t us = a(0);
        if (tid && us > 0) {
            scheduler_->sleep(tid, us);
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
        }
        return result(0);
    }

    if (n == "sceKernelSleep" || n == "sleep") {
        std::uint64_t sec = a(0);
        if (tid && sec > 0) {
            scheduler_->sleep(tid, sec * 1000000ULL);
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
void OrbisRuntime::dispatch_flip_event(std::int32_t video_handle, std::uint32_t buf_idx) {
    (void)buf_idx;
    std::lock_guard<std::mutex> lock(equeues_mutex_);
    for (auto& pair : equeues_) {
        auto& eq = pair.second;
        if (!eq) continue;
        std::lock_guard<std::mutex> eq_lock(eq->mtx);
        if (eq->deleted) continue;
        for (const auto& reg : eq->flip_registrations) {
            if (reg.video_handle == video_handle || reg.video_handle == -1 || video_handle == 0 || reg.video_handle == 0) {
                EqueueEvent ev{};
                ev.ident = static_cast<std::uint64_t>(video_handle != 0 ? video_handle : (reg.video_handle != 0 ? reg.video_handle : 0x6));
                ev.filter = -13; // EVFILT_VIDEO_OUT
                ev.flags = 0;
                ev.fflags = 0;
                ev.data = 1;
                ev.udata = reg.udata;
                eq->pending_events.push_back(ev);
                eq->cv.notify_all();
            }
        }
    }
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
        std::string base = save_data_manager().get_base_directory();
        if (base.empty()) base = SaveDataManager::instance().get_base_directory();
        if (base.empty()) base = "savedata";
        bool ok = save_data_manager().initialize(base, quota, save_data_manager().get_vfs() ? save_data_manager().get_vfs() : vfs_);
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
            if (!std::memchr(title_obj.data, '\0', sizeof(title_obj.data))) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            title_str = title_obj.data;
        }
        SceSaveDataDirName dir_obj{};
        const char* dir_str = nullptr;
        if (dir_ptr) {
            if (mem_.load(dir_ptr, &dir_obj, sizeof(dir_obj)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            if (!std::memchr(dir_obj.data, '\0', sizeof(dir_obj.data))) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            dir_str = dir_obj.data;
        }
        // A saída precisa estar totalmente gravável antes de criar ou ocupar um slot.
        std::uint64_t result_base = 0;
        std::size_t result_size = 0;
        unsigned result_perm = 0;
        if (!mem_.query_region(a(1), result_base, result_size, result_perm) ||
            !(result_perm & Permission::write) || a(1) < result_base ||
            a(1) - result_base >= result_size ||
            sizeof(SceSaveDataMountResult) > result_size - (a(1) - result_base)) {
            return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        }
        std::string mount_pt;
        uint32_t status = 0;
        int32_t res = save_data_manager().mount(user, title_str, dir_str, mode, blocks, mount_pt, status);
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
        if (!std::memchr(pt.data, '\0', sizeof(pt.data))) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        bool commit = (a(1) & SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) != 0;
        return static_cast<std::uint32_t>(save_data_manager().umount(pt.data, commit));
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
            if (!std::memchr(title_obj.data, '\0', sizeof(title_obj.data))) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            title_str = title_obj.data;
        }
        SceSaveDataDirName dir_obj{};
        if (mem_.load(dir_ptr, &dir_obj, sizeof(dir_obj)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        if (!std::memchr(dir_obj.data, '\0', sizeof(dir_obj.data))) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        return static_cast<std::uint32_t>(save_data_manager().delete_save(d.user, title_str, dir_obj.data));
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
            if (!std::memchr(title_obj.data, '\0', sizeof(title_obj.data))) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            title_str = title_obj.data;
        }
        SceSaveDataDirName dir_obj{};
        const char* dir_pattern = nullptr;
        if (dir_ptr) {
            if (mem_.load(dir_ptr, &dir_obj, sizeof(dir_obj)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            if (!std::memchr(dir_obj.data, '\0', sizeof(dir_obj.data))) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
            dir_pattern = dir_obj.data;
        }
        std::vector<SaveSearchEntry> entries;
        int32_t res = save_data_manager().dir_name_search(cond.user, title_str, dir_pattern, cond.key, cond.order, entries);
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
                if (mem_.store(names_addr + i * sizeof(SceSaveDataDirName), &name, sizeof(name)) != MemoryError::ok) {
                    return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
                }
            }
            if (params_addr) {
                if (mem_.store(params_addr + i * sizeof(SceSaveDataParam), &entries[i].param, sizeof(SceSaveDataParam)) != MemoryError::ok) {
                    return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
                }
            }
            if (infos_addr) {
                SceSaveDataSearchInfo info{};
                info.blocks = entries[i].blocks;
                info.free_blocks = entries[i].free_blocks;
                if (mem_.store(infos_addr + i * sizeof(SceSaveDataSearchInfo), &info, sizeof(info)) != MemoryError::ok) {
                    return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
                }
            }
        }
        if (mem_.store(a(1), &result, sizeof(result)) != MemoryError::ok) {
            return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        }
        return SCE_SAVEDATA_OK;
    }
    if (n == "sceSaveDataSetParam") {
        if (!a(0) || !a(2)) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        SceSaveDataMountPoint pt{};
        if (mem_.load(a(0), &pt, sizeof(pt)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        if (!std::memchr(pt.data, '\0', sizeof(pt.data))) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        uint32_t type = static_cast<uint32_t>(a(1));
        std::size_t size = static_cast<std::size_t>(a(3));
        if (type > 4 || size == 0 || size > 65536) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        if (type == 0 && size < sizeof(SceSaveDataParam)) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        if (type == 4 && size < sizeof(uint32_t)) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        std::vector<uint8_t> buf(size);
        if (mem_.load(a(2), buf.data(), size) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        return static_cast<std::uint32_t>(save_data_manager().set_param(pt.data, type, buf.data(), size));
    }
    if (n == "sceSaveDataGetParam") {
        if (!a(0) || !a(2)) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        SceSaveDataMountPoint pt{};
        if (mem_.load(a(0), &pt, sizeof(pt)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        SceSaveDataParam param{};
        if (!std::memchr(pt.data, '\0', sizeof(pt.data))) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        int32_t res = save_data_manager().get_param(pt.data, param);
        if (res == SCE_SAVEDATA_OK) {
            std::size_t copy_size = std::min<std::size_t>(sizeof(param), static_cast<std::size_t>(a(3)));
            if (mem_.store(a(2), &param, copy_size) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        }
        return static_cast<std::uint32_t>(res);
    }
    if (n == "sceSaveDataSaveIcon") {
        if (!a(0) || !a(1)) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        SceSaveDataMountPoint pt{};
        if (mem_.load(a(0), &pt, sizeof(pt)) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        std::size_t size = static_cast<std::size_t>(a(2));
        if (!std::memchr(pt.data, '\0', sizeof(pt.data))) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        constexpr std::size_t kMaxIconSize = 2 * 1024 * 1024;
        if (size == 0 || size > kMaxIconSize) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        std::vector<uint8_t> buf(size);
        if (mem_.load(a(1), buf.data(), size) != MemoryError::ok) return static_cast<std::uint32_t>(SCE_SAVEDATA_ERROR_PARAMETER);
        return static_cast<std::uint32_t>(save_data_manager().save_icon(pt.data, buf.data(), size));
    }
    if (n == "sceSaveDataTerminate") {
        save_data_manager().terminate();
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

    // libSceAvPlayer HLE: ciclo de vida de player com validacao e transicao de intro (T155)
    if (n == "sceAvPlayerInit") {
        auto h = static_cast<std::uint32_t>(next_handle_++);
        av_players_[h] = AvPlayerState{h, "", false, false, false, 0, 3000, 2};
        return h;
    }
    if (n == "sceAvPlayerClose") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        av_players_.erase(it);
        return 0;
    }
    if (n == "sceAvPlayerAddSource") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        auto src_ptr = a(1);
        if (!src_ptr) return 0x80540002;
        std::string src_path;
        for (std::size_t i = 0; i < 1024; ++i) {
            char c = 0;
            if (mem_.load(src_ptr + i, &c, 1) != MemoryError::ok) return 0x80540002;
            if (c == '\0') break;
            src_path.push_back(c);
        }
        if (src_path.empty()) return 0x80540002;

        // Validar se e formato de video suportado ou arquivo existente no VFS
        bool valid_source = false;
        std::string lower = src_path;
        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        auto ends_with = [](const std::string& str, const std::string& suffix) {
            return str.size() >= suffix.size() &&
                   str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        if (ends_with(lower, ".mp4") || ends_with(lower, ".m4v") ||
            ends_with(lower, ".avi") || ends_with(lower, ".mkv") ||
            ends_with(lower, ".bk2") || ends_with(lower, ".webm")) {
            valid_source = true;
        }

        auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
        if (active_vfs) {
            auto vpath = resolve_guest_virtual_path(src_path);
            if (!vpath.empty() && active_vfs->exists(vpath)) {
                valid_source = true;
            } else if (!valid_source) {
                return 0x80540002;
            }
        } else if (!valid_source) {
            return 0x80540002;
        }

        it->second.source_path = src_path;
        it->second.active = true;
        it->second.current_time_ms = 0;
        return 0;
    }
    if (n == "sceAvPlayerStart") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        if (it->second.source_path.empty()) return 0x80540002;
        it->second.active = true;
        it->second.paused = false;
        return 0;
    }
    if (n == "sceAvPlayerStop") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        it->second.active = false;
        it->second.current_time_ms = 0;
        return 0;
    }
    if (n == "sceAvPlayerPause") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        it->second.paused = true;
        return 0;
    }
    if (n == "sceAvPlayerResume") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        it->second.paused = false;
        return 0;
    }
    if (n == "sceAvPlayerIsActive") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0;
        return it->second.active ? 1 : 0;
    }
    if (n == "sceAvPlayerIsPaused") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0;
        return it->second.paused ? 1 : 0;
    }
    if (n == "sceAvPlayerCurrentTime") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0;
        auto& p = it->second;
        if (p.active && !p.paused) {
            if (p.current_time_ms < p.duration_ms) {
                p.current_time_ms += 33;
            } else if (!p.looping) {
                p.active = false;
            } else {
                p.current_time_ms = 0;
            }
        }
        return p.current_time_ms;
    }
    if (n == "sceAvPlayerGetDuration") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0;
        return it->second.duration_ms;
    }
    if (n == "sceAvPlayerStreamCount") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0;
        return it->second.stream_count;
    }
    if (n == "sceAvPlayerSetLooping") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        it->second.looping = (a(1) != 0);
        return 0;
    }
    if (n == "sceAvPlayerReset") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        it->second.current_time_ms = 0;
        it->second.active = true;
        it->second.paused = false;
        return 0;
    }
    if (n == "sceAvPlayerJumpToTime") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        std::uint64_t target_ms = a(1);
        it->second.current_time_ms = std::min(target_ms, it->second.duration_ms);
        if (it->second.current_time_ms >= it->second.duration_ms && !it->second.looping) {
            it->second.active = false;
        }
        return 0;
    }
    if (n == "sceAvPlayerEnableStream") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        auto stream_idx = static_cast<std::uint32_t>(a(2));
        if (stream_idx >= it->second.stream_count) return 0x80540003;
        return 0;
    }
    if (n == "sceAvPlayerPostInit" || n == "sceAvPlayerSetAvSync") {
        auto h = static_cast<std::uint32_t>(a(0));
        if (!av_players_.count(h)) return 0x80540001;
        return 0;
    }
    if (n == "sceAvPlayerGetVideoDataEx" || n == "sceAvPlayerGetVideoData") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        auto out_ptr = a(1);
        if (!out_ptr) return 0x80540002;
        if (!it->second.active || it->second.current_time_ms >= it->second.duration_ms) {
            return 0; // Nenhum quadro disponivel / EOF
        }
        std::uint32_t frame_info[8] = {
            static_cast<std::uint32_t>(it->second.current_time_ms & 0xFFFFFFFF),
            static_cast<std::uint32_t>((it->second.current_time_ms >> 32) & 0xFFFFFFFF),
            0, 0, // pData
            1920, // width
            1080, // height
            1920 * 4, // pitch
            1 // RGBA
        };
        if (mem_.store(out_ptr, frame_info, sizeof(frame_info)) != MemoryError::ok) {
            return 0x80540002;
        }
        return 1;
    }
    if (n == "sceAvPlayerGetAudioData" || n == "sceAvPlayerGetAudioDataEx") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        auto out_ptr = a(1);
        if (!out_ptr) return 0x80540002;
        if (!it->second.active || it->second.current_time_ms >= it->second.duration_ms) {
            return 0;
        }
        std::uint32_t audio_info[6] = {
            static_cast<std::uint32_t>(it->second.current_time_ms & 0xFFFFFFFF),
            static_cast<std::uint32_t>((it->second.current_time_ms >> 32) & 0xFFFFFFFF),
            0, 0, // pData
            2,    // channels
            48000 // sample rate
        };
        if (mem_.store(out_ptr, audio_info, sizeof(audio_info)) != MemoryError::ok) {
            return 0x80540002;
        }
        return 1;
    }
    if (n == "sceAvPlayerGetStreamInfo") {
        auto h = static_cast<std::uint32_t>(a(0));
        auto it = av_players_.find(h);
        if (it == av_players_.end()) return 0x80540001;
        auto stream_idx = static_cast<std::uint32_t>(a(1));
        if (stream_idx >= it->second.stream_count) return 0x80540003;
        auto out_ptr = a(2);
        if (!out_ptr) return 0x80540002;
        std::uint32_t stream_info[6] = {};
        if (stream_idx == 0) {
            stream_info[0] = 1; // Video
            stream_info[1] = 1920;
            stream_info[2] = 1080;
            stream_info[3] = 30; // 30 FPS
            stream_info[4] = static_cast<std::uint32_t>(it->second.duration_ms & 0xFFFFFFFF);
            stream_info[5] = static_cast<std::uint32_t>((it->second.duration_ms >> 32) & 0xFFFFFFFF);
        } else {
            stream_info[0] = 2; // Audio
            stream_info[1] = 2; // Stereo
            stream_info[2] = 48000;
            stream_info[3] = 0;
            stream_info[4] = static_cast<std::uint32_t>(it->second.duration_ms & 0xFFFFFFFF);
            stream_info[5] = static_cast<std::uint32_t>((it->second.duration_ms >> 32) & 0xFFFFFFFF);
        }
        if (mem_.store(out_ptr, stream_info, sizeof(stream_info)) != MemoryError::ok) {
            return 0x80540002;
        }
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
        auto video_handle = static_cast<std::int32_t>(a(1));
        auto udata = a(2);
        std::lock_guard<std::mutex> lock(equeues_mutex_);
        auto it = equeues_.find(eq_id);
        if (it != equeues_.end()) {
            std::lock_guard<std::mutex> eq_lock(it->second->mtx);
            bool found = false;
            for (auto& reg : it->second->flip_registrations) {
                if (reg.video_handle == video_handle) {
                    reg.udata = udata;
                    found = true;
                    break;
                }
            }
            if (!found) {
                it->second->flip_registrations.push_back({video_handle, udata});
            }
        }
        return 0;
    }
    if (n == "sceVideoOutDeleteFlipEvent") {
        auto eq_id = static_cast<std::uint32_t>(a(0));
        auto video_handle = static_cast<std::int32_t>(a(1));
        std::lock_guard<std::mutex> lock(equeues_mutex_);
        auto it = equeues_.find(eq_id);
        if (it != equeues_.end()) {
            std::lock_guard<std::mutex> eq_lock(it->second->mtx);
            auto& regs = it->second->flip_registrations;
            regs.erase(std::remove_if(regs.begin(), regs.end(),
                [video_handle](const FlipEventRegistration& reg) {
                    return reg.video_handle == video_handle;
                }), regs.end());
        }
        return 0;
    }
    if (n == "sceVideoOutSetBufferAttribute") {
        std::uint64_t attr_addr = a(0);
        if (attr_addr) {
            BufferAttribute attr{};
            attr.pixel_format = static_cast<std::uint32_t>(a(1));
            attr.tiling_mode = static_cast<std::uint32_t>(a(2));
            attr.aspect_ratio = static_cast<std::int32_t>(a(3));
            attr.width = static_cast<std::uint32_t>(a(4));
            attr.height = static_cast<std::uint32_t>(a(5));
            attr.pitch_in_pixel = static_cast<std::uint32_t>(a(6));
            attr.option = 0;
            attr.reserved0 = 0;
            attr.reserved1 = 0;
            mem_.store(attr_addr, &attr, sizeof(attr));
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
        BufferAttribute attr{};
        if (mem_.load(attr_ptr, &attr, sizeof(attr)) != MemoryError::ok) {
            return static_cast<std::uint64_t>(-1);
        }
        if (attr.tiling_mode > 1) {
            return static_cast<std::uint64_t>(-1);
        }
        std::uint32_t width = attr.width;
        std::uint32_t height = attr.height;
        std::uint32_t pitch = attr.pitch_in_pixel ? attr.pitch_in_pixel : width;
        if (pitch < width) {
            return static_cast<std::uint64_t>(-1);
        }
        std::uint32_t bpp = 4;
        if (attr.pixel_format == 0xC1060000) {
            bpp = 8;
        }
        for (int i = 0; i < buf_count; ++i) {
            std::uint64_t buf_addr = 0;
            if (mem_.load(addrs_ptr + static_cast<std::uint64_t>(i) * 8, &buf_addr, 8) != MemoryError::ok) {
                return static_cast<std::uint64_t>(-1);
            }
            VideoBuffer vb{buf_addr, width, height, pitch, bpp, attr.pixel_format, attr.tiling_mode};
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
        dispatch_flip_event(static_cast<std::int32_t>(a(0)), static_cast<std::uint32_t>(buf_idx));
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
        if (!out_eq) return 0x80020016;
        std::string name = name_ptr ? read_guest_string(name_ptr) : "equeue";
        std::lock_guard<std::mutex> lock(equeues_mutex_);
        auto id = static_cast<std::uint32_t>(next_equeue_id_++);
        auto eq = std::make_shared<EqueueRecord>();
        eq->name = std::move(name);
        equeues_[id] = eq;
        put<std::int32_t>(mem_, out_eq, static_cast<std::int32_t>(id));
        return 0;
    }
    if (n == "sceKernelDeleteEqueue") {
        auto id = static_cast<std::uint32_t>(a(0));
        std::shared_ptr<EqueueRecord> eq;
        {
            std::lock_guard<std::mutex> lock(equeues_mutex_);
            auto it = equeues_.find(id);
            if (it == equeues_.end()) {
                return 0x80020014;
            }
            eq = it->second;
            equeues_.erase(it);
        }
        if (eq) {
            std::lock_guard<std::mutex> eq_lock(eq->mtx);
            eq->deleted = true;
            eq->pending_events.clear();
            eq->cv.notify_all();
        }
        return 0;
    }
    if (n == "sceKernelWaitEqueue") {
        auto id = static_cast<std::uint32_t>(a(0));
        auto ev_ptr = a(1);
        auto num = static_cast<int>(a(2));
        auto out_ptr = a(3);
        auto micros_ptr = a(4);

        if (out_ptr) put<std::int32_t>(mem_, out_ptr, 0);

        std::shared_ptr<EqueueRecord> eq;
        {
            std::lock_guard<std::mutex> lock(equeues_mutex_);
            auto it = equeues_.find(id);
            if (it == equeues_.end()) {
                return 0x80020014;
            }
            eq = it->second;
        }

        if (num <= 0 || !ev_ptr) {
            return 0x80020016;
        }

        std::unique_lock<std::mutex> lock(eq->mtx);
        if (eq->deleted) {
            return 0x80020014;
        }

        bool has_timeout = (micros_ptr != 0);
        std::uint32_t timeout_us = 0;
        if (has_timeout) {
            if (mem_.load(micros_ptr, &timeout_us, sizeof(timeout_us)) != MemoryError::ok) {
                return 0x80020016;
            }
        }

        if (eq->pending_events.empty()) {
            if (has_timeout && timeout_us == 0) {
                return 0x8002001a;
            }
            if (has_timeout) {
                bool ok = eq->cv.wait_for(lock, std::chrono::microseconds(timeout_us), [&]() {
                    return !eq->pending_events.empty() || eq->deleted;
                });
                if (eq->deleted) {
                    return 0x80020014;
                }
                if (!ok && eq->pending_events.empty()) {
                    return 0x8002001a;
                }
            } else {
                eq->cv.wait(lock, [&]() {
                    return !eq->pending_events.empty() || eq->deleted;
                });
                if (eq->deleted) {
                    return 0x80020014;
                }
            }
        }

        int count = 0;
        while (count < num && !eq->pending_events.empty()) {
            auto ev = eq->pending_events.front();
            eq->pending_events.erase(eq->pending_events.begin());
            if (mem_.store(ev_ptr + count * sizeof(EqueueEvent), &ev, sizeof(EqueueEvent)) != MemoryError::ok) {
                break;
            }
            count++;
        }
        if (out_ptr) put<std::int32_t>(mem_, out_ptr, count);
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
        // Preparar um contexto temporário: um IB inválido não pode deixar
        // registradores ou draws parcialmente aplicados ao contexto persistente.
        // As chamadas da submissão anterior não são reaproveitadas.
        auto pending = std::make_unique<GnmContext>(*gnm_context_);
        pending->clear_calls();
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t dcb_addr = 0;
            uint32_t dcb_size = 0;
            if (mem_.load(dcb_addrs_ptr + i * 8, &dcb_addr, 8) != MemoryError::ok ||
                mem_.load(dcb_sizes_ptr + i * 4, &dcb_size, 4) != MemoryError::ok ||
                dcb_addr == 0 || dcb_size < 4 || dcb_size % 4 != 0 ||
                pending->submit(dcb_addr, dcb_size / 4) != Pm4Status::ok) {
                return static_cast<std::uint64_t>(-1);
            }
        }
        last_rasterizer_status_ = RasterizerStatus::Ok;
        if (!pending->draw_calls().empty()) {
            last_rasterizer_status_ = rasterizer_.execute_draw_calls(
                mem_, pending->draw_calls(), &rasterizer_stats_);
            if (last_rasterizer_status_ != RasterizerStatus::Ok) {
                ++rasterizer_failure_count_;
                // T154: Propagar erro de desenho e impedir flip de quadro invalido sem poluir contexto
                return static_cast<std::uint64_t>(-1);
            }
        }
        // T157: Conectar a submissão de draws e dispatches guest ao bridge Metal iOS
        if (gnm_listener_) {
            if (!pending->draw_calls().empty() && !gnm_listener_->on_draw_calls(pending->draw_calls())) {
                ++rasterizer_failure_count_;
                last_rasterizer_status_ = RasterizerStatus::Error;
                return static_cast<std::uint64_t>(-1);
            }
            if (!pending->dispatch_calls().empty() && !gnm_listener_->on_dispatch_calls(pending->dispatch_calls())) {
                ++rasterizer_failure_count_;
                last_rasterizer_status_ = RasterizerStatus::Error;
                return static_cast<std::uint64_t>(-1);
            }
        }
        gnm_context_.swap(pending);
        submit_count_ += count;
        if (n == "sceGnmSubmitAndFlipCommandBuffers") {
            auto h = static_cast<std::int32_t>(a(5));
            auto buf_idx = static_cast<std::uint32_t>(a(6));
            auto flip_arg = a(8);
            if (!video_out_) return static_cast<std::uint64_t>(-1);
            VideoStatus flip_status = VideoStatus::invalid;
            std::uint32_t actual_buf = buf_idx;
            if (buf_idx < 16) {
                flip_status = video_out_->submit_flip(buf_idx, flip_arg);
            } else if (h < 16) {
                flip_status = video_out_->submit_flip(static_cast<std::uint32_t>(h), a(7));
                actual_buf = static_cast<std::uint32_t>(h);
            }
            if (flip_status != VideoStatus::ok) return static_cast<std::uint64_t>(-1);
            dispatch_flip_event(h, actual_buf);
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

    // Formatting functions (snprintf, vsnprintf, sprintf, vsprintf, fprintf, vfprintf)
    auto make_variadic_args = [&](unsigned start_int_arg) {
        struct VariadicGetters {
            std::function<std::uint64_t()> get_int;
            std::function<double()> get_float;
        };
        auto int_idx = std::make_shared<unsigned>(start_int_arg);
        auto float_idx = std::make_shared<unsigned>(0);
        VariadicGetters g;
        g.get_int = [&c, this, int_idx]() -> std::uint64_t {
            return arg(c, (*int_idx)++);
        };
        g.get_float = [&c, this, int_idx, float_idx]() -> double {
            double d = 0.0;
            if (*float_idx < 8 && (c.xmm[*float_idx][0] != 0 || (c.gpr[0] > 0 && c.gpr[0] <= 8))) {
                std::memcpy(&d, &c.xmm[(*float_idx)++][0], sizeof(d));
            } else {
                std::uint64_t gpr_val = arg(c, (*int_idx)++);
                std::memcpy(&d, &gpr_val, sizeof(d));
            }
            return d;
        };
        return g;
    };

    auto make_valist_args = [&](std::uint64_t va_list_addr) {
        struct VaListGetters {
            std::function<std::uint64_t()> get_int;
            std::function<double()> get_float;
        };
        struct SysVVaList {
            std::uint32_t gp_offset;
            std::uint32_t fp_offset;
            std::uint64_t overflow_arg_area;
            std::uint64_t reg_save_area;
        };
        auto vl = std::make_shared<SysVVaList>();
        auto is_sysv = std::make_shared<bool>(false);
        auto raw_offset = std::make_shared<std::uint64_t>(0);

        if (va_list_addr != 0 && mem_.load(va_list_addr, vl.get(), sizeof(SysVVaList)) == MemoryError::ok) {
            if (vl->gp_offset <= 48 && vl->fp_offset >= 48 && vl->fp_offset <= 176 && vl->reg_save_area != 0) {
                *is_sysv = true;
            }
        }

        VaListGetters g;
        g.get_int = [this, va_list_addr, vl, is_sysv, raw_offset]() -> std::uint64_t {
            if (!va_list_addr) return 0;
            std::uint64_t val = 0;
            if (*is_sysv) {
                if (vl->gp_offset < 48) {
                    mem_.load(vl->reg_save_area + vl->gp_offset, &val, 8);
                    vl->gp_offset += 8;
                } else if (vl->overflow_arg_area) {
                    mem_.load(vl->overflow_arg_area, &val, 8);
                    vl->overflow_arg_area += 8;
                }
                mem_.store(va_list_addr, vl.get(), sizeof(SysVVaList));
            } else {
                mem_.load(va_list_addr + *raw_offset, &val, 8);
                *raw_offset += 8;
            }
            return val;
        };
        g.get_float = [this, va_list_addr, vl, is_sysv, raw_offset]() -> double {
            if (!va_list_addr) return 0.0;
            double val = 0.0;
            if (*is_sysv) {
                if (vl->fp_offset < 176) {
                    mem_.load(vl->reg_save_area + vl->fp_offset, &val, 8);
                    vl->fp_offset += 16;
                } else if (vl->overflow_arg_area) {
                    mem_.load(vl->overflow_arg_area, &val, 8);
                    vl->overflow_arg_area += 8;
                }
                mem_.store(va_list_addr, vl.get(), sizeof(SysVVaList));
            } else {
                mem_.load(va_list_addr + *raw_offset, &val, 8);
                *raw_offset += 8;
            }
            return val;
        };
        return g;
    };

    if (n == "snprintf") {
        std::uint64_t dst = a(0);
        std::uint64_t max_size = a(1);
        std::uint64_t fmt_addr = a(2);
        if (!fmt_addr) return static_cast<std::uint64_t>(-1);
        if (!dst && max_size > 0) return static_cast<std::uint64_t>(-1);

        std::string format = read_guest_string(fmt_addr, 4096);
        auto getters = make_variadic_args(3);
        std::string out;
        if (!format_printf(format, getters.get_int, getters.get_float, out)) {
            return static_cast<std::uint64_t>(-1);
        }

        if (dst && max_size > 0) {
            std::size_t to_write = std::min<std::size_t>(out.size(), static_cast<std::size_t>(max_size - 1));
            if (to_write > 0 && mem_.store(dst, out.data(), to_write) != MemoryError::ok) {
                return static_cast<std::uint64_t>(-1);
            }
            char null_byte = '\0';
            if (mem_.store(dst + to_write, &null_byte, 1) != MemoryError::ok) {
                return static_cast<std::uint64_t>(-1);
            }
        }
        return static_cast<std::uint64_t>(out.size());
    }

    if (n == "vsnprintf") {
        std::uint64_t dst = a(0);
        std::uint64_t max_size = a(1);
        std::uint64_t fmt_addr = a(2);
        std::uint64_t va_list_addr = a(3);
        if (!fmt_addr) return static_cast<std::uint64_t>(-1);
        if (!dst && max_size > 0) return static_cast<std::uint64_t>(-1);

        std::string format = read_guest_string(fmt_addr, 4096);
        auto getters = make_valist_args(va_list_addr);
        std::string out;
        if (!format_printf(format, getters.get_int, getters.get_float, out)) {
            return static_cast<std::uint64_t>(-1);
        }

        if (dst && max_size > 0) {
            std::size_t to_write = std::min<std::size_t>(out.size(), static_cast<std::size_t>(max_size - 1));
            if (to_write > 0 && mem_.store(dst, out.data(), to_write) != MemoryError::ok) {
                return static_cast<std::uint64_t>(-1);
            }
            char null_byte = '\0';
            if (mem_.store(dst + to_write, &null_byte, 1) != MemoryError::ok) {
                return static_cast<std::uint64_t>(-1);
            }
        }
        return static_cast<std::uint64_t>(out.size());
    }

    if (n == "sprintf" || n == "vsprintf") {
        std::uint64_t dst = a(0);
        std::uint64_t fmt_addr = a(1);
        if (!dst || !fmt_addr) return static_cast<std::uint64_t>(-1);

        std::string format = read_guest_string(fmt_addr, 4096);
        std::string out;
        bool ok = false;
        if (n == "sprintf") {
            auto getters = make_variadic_args(2);
            ok = format_printf(format, getters.get_int, getters.get_float, out);
        } else {
            auto getters = make_valist_args(a(2));
            ok = format_printf(format, getters.get_int, getters.get_float, out);
        }
        if (!ok) return static_cast<std::uint64_t>(-1);

        if (mem_.store(dst, out.data(), out.size()) != MemoryError::ok) {
            return static_cast<std::uint64_t>(-1);
        }
        char null_byte = '\0';
        if (mem_.store(dst + out.size(), &null_byte, 1) != MemoryError::ok) {
            return static_cast<std::uint64_t>(-1);
        }
        return static_cast<std::uint64_t>(out.size());
    }

    if (n == "fprintf" || n == "vfprintf") {
        FILE* fp = get_file(a(0));
        if (!fp) return static_cast<std::uint64_t>(-1);
        std::uint64_t fmt_addr = a(1);
        if (!fmt_addr) return static_cast<std::uint64_t>(-1);

        std::string format = read_guest_string(fmt_addr, 4096);
        std::string out;
        bool ok = false;
        if (n == "fprintf") {
            auto getters = make_variadic_args(2);
            ok = format_printf(format, getters.get_int, getters.get_float, out);
        } else {
            auto getters = make_valist_args(a(2));
            ok = format_printf(format, getters.get_int, getters.get_float, out);
        }
        if (!ok) return static_cast<std::uint64_t>(-1);

        std::size_t written = std::fwrite(out.data(), 1, out.size(), fp);
        std::fflush(fp);
        if (written < out.size()) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(written);
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
    if (n=="sceKernelGetDirectMemorySize") return direct_memory_pool_;
    if (n=="sceKernelAllocateDirectMemory") {
        auto low=a(0), high=std::min(a(1),direct_memory_pool_), size=a(2), alignment=a(3) ? a(3) : page;
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
        unsigned permissions = ((prot & 0x11) ? static_cast<unsigned>(read) : 0u) | ((prot & 0x22) ? static_cast<unsigned>(write) : 0u);
        if (mem_.map(at,static_cast<std::size_t>(size),permissions)!=MemoryError::ok) return no_memory;
        try { put(mem_,a(0),at); } catch (...) { mem_.unmap(at); throw; }
        mappings_[at]=size; if (direct) direct_mappings_[at]=phys;
        next_virtual_=std::max(next_virtual_,at+size); return 0;
    }
    if (n=="sceKernelMunmap" || n=="munmap" || n=="sceKernelReleaseFlexibleMemory") {
        auto addr = a(0);
        auto size = a(1);
        if (!addr || !size || addr % page || size % page || size > UINT64_MAX - addr ||
            size > std::numeric_limits<std::size_t>::max()) return invalid;
        // Não confirmar liberação de buracos: validar toda a faixa antes de modificar o mapa.
        const std::uint64_t u_start = addr;
        const std::uint64_t u_end = addr + size;
        for (auto cursor = u_start; cursor < u_end;) {
            std::uint64_t base = 0;
            std::size_t length = 0;
            unsigned permissions = 0;
            if (!mem_.query_region(cursor, base, length, permissions)) return 0x8002000e; // EFAULT
            cursor = std::min(u_end, base + length);
        }
        const auto unmap_result = mem_.unmap(addr, static_cast<std::size_t>(size));
        if (unmap_result != MemoryError::ok)
            return unmap_result == MemoryError::unmapped ? 0x8002000e : invalid;
        std::vector<std::uint64_t> to_del;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> to_ins;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> direct_to_ins;
        for (auto& m : mappings_) {
            std::uint64_t m_start = m.first;
            std::uint64_t m_size = m.second;
            std::uint64_t m_end = m_start + m_size;
            if (m_start >= u_end || m_end <= u_start) continue;
            to_del.push_back(m_start);
            const auto direct = direct_mappings_.find(m_start);
            if (u_start > m_start) {
                to_ins.emplace_back(m_start, u_start - m_start);
                if (direct != direct_mappings_.end()) direct_to_ins.emplace_back(m_start, direct->second);
            }
            if (u_end < m_end) {
                to_ins.emplace_back(u_end, m_end - u_end);
                if (direct != direct_mappings_.end())
                    direct_to_ins.emplace_back(u_end, direct->second + (u_end - m_start));
            }
        }
        for (auto k : to_del) {
            mappings_.erase(k);
            direct_mappings_.erase(k);
        }
        for (auto& p : to_ins) {
            mappings_.insert(p);
        }
        for (auto& p : direct_to_ins) direct_mappings_.insert(p);
        return 0;
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
    if (n=="sceKernelMprotect" || n=="mprotect") {
        auto addr = a(0);
        auto len = a(1);
        auto prot = a(2);
        if (!addr || !len) return invalid;
        if ((addr % page) != 0 || (len % page) != 0) return invalid;
        if ((prot & ~7) != 0) return invalid;

        std::uint64_t cursor = addr;
        const std::uint64_t p_end = addr + len;
        if (p_end < addr) return 0x8002000e; // EFAULT / overflow
        while (cursor < p_end) {
            std::uint64_t base = 0;
            std::size_t size = 0;
            unsigned current_perm = 0;
            if (!mem_.query_region(cursor, base, size, current_perm)) {
                return 0x8002000e; // EFAULT
            }
            cursor = std::min(p_end, base + size);
        }

        unsigned new_perm = 0;
        if (prot & 1) new_perm |= read;
        if (prot & 2) new_perm |= write;

        MemoryError merr = mem_.protect(addr, static_cast<std::size_t>(len), new_perm);
        if (merr != MemoryError::ok) {
            if (merr == MemoryError::invalid) return invalid;
            if (merr == MemoryError::unmapped) return 0x8002000e;
            return invalid;
        }

        std::vector<std::uint64_t> to_del;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> to_ins;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> direct_to_ins;
        for (auto& m : mappings_) {
            std::uint64_t m_start = m.first;
            std::uint64_t m_size = m.second;
            std::uint64_t m_end = m_start + m_size;
            if (m_start >= p_end || m_end <= addr) continue;
            to_del.push_back(m_start);
            const auto direct = direct_mappings_.find(m_start);
            if (addr > m_start) {
                to_ins.emplace_back(m_start, addr - m_start);
                if (direct != direct_mappings_.end()) direct_to_ins.emplace_back(m_start, direct->second);
            }
            const std::uint64_t mid_start = std::max(addr, m_start);
            const std::uint64_t mid_end = std::min(p_end, m_end);
            to_ins.emplace_back(mid_start, mid_end - mid_start);
            if (direct != direct_mappings_.end()) {
                direct_to_ins.emplace_back(mid_start, direct->second + (mid_start - m_start));
            }
            if (p_end < m_end) {
                to_ins.emplace_back(p_end, m_end - p_end);
                if (direct != direct_mappings_.end())
                    direct_to_ins.emplace_back(p_end, direct->second + (p_end - m_start));
            }
        }
        for (auto k : to_del) {
            mappings_.erase(k);
            direct_mappings_.erase(k);
        }
        for (auto& p : to_ins) mappings_.insert(p);
        for (auto& p : direct_to_ins) direct_mappings_.insert(p);

        return 0;
    }
    if (n=="sceKernelQueryMemoryProtection") {
        auto addr = a(0);
        auto out_start = a(1);
        auto out_end = a(2);
        auto out_prot = a(3);
        std::uint64_t base = 0;
        std::size_t size = 0;
        unsigned perm = 0;
        if (!mem_.query_region(addr, base, size, perm)) return 0x8002000e; // EFAULT
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
        unsigned perm = 0;
        if (!mem_.query_region(addr, base, size, perm)) return 0x8002000e; // EFAULT
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
                if (!length || length % page) break;
                if (!(flags & 0x10) || !start_addr) {
                    start_addr = align(next_virtual_, page);
                }
                if ((static_cast<unsigned char>(prot) & ~0x37) != 0) break;
                const unsigned permissions = ((prot & 0x11) ? static_cast<unsigned>(read) : 0u) | ((prot & 0x22) ? static_cast<unsigned>(write) : 0u);
                if (mem_.map(start_addr, static_cast<std::size_t>(length), permissions) != MemoryError::ok) break;
                put(mem_, entry_ptr, start_addr);
                mappings_[start_addr] = length;
                next_virtual_ = std::max(next_virtual_, start_addr + length);
            } else if (operation == 1) { // unmap
                if (!length || (length % page) != 0 || (start_addr % page) != 0) break;
                std::uint64_t cursor = start_addr;
                const std::uint64_t u_end = start_addr + length;
                if (u_end < start_addr) break;
                bool all_mapped = true;
                while (cursor < u_end) {
                    std::uint64_t base = 0;
                    std::size_t rsize = 0;
                    unsigned perm = 0;
                    if (!mem_.query_region(cursor, base, rsize, perm)) {
                        all_mapped = false;
                        break;
                    }
                    cursor = std::min(u_end, base + rsize);
                }
                if (!all_mapped) break;

                const auto unmap_res = mem_.unmap(start_addr, static_cast<std::size_t>(length));
                if (unmap_res != MemoryError::ok) break;

                std::vector<std::uint64_t> to_del;
                std::vector<std::pair<std::uint64_t, std::uint64_t>> to_ins;
                std::vector<std::pair<std::uint64_t, std::uint64_t>> direct_to_ins;
                for (auto& m : mappings_) {
                    std::uint64_t m_start = m.first;
                    std::uint64_t m_size = m.second;
                    std::uint64_t m_end = m_start + m_size;
                    if (m_start >= u_end || m_end <= start_addr) continue;
                    to_del.push_back(m_start);
                    const auto direct = direct_mappings_.find(m_start);
                    if (start_addr > m_start) {
                        to_ins.emplace_back(m_start, start_addr - m_start);
                        if (direct != direct_mappings_.end()) direct_to_ins.emplace_back(m_start, direct->second);
                    }
                    if (u_end < m_end) {
                        to_ins.emplace_back(u_end, m_end - u_end);
                        if (direct != direct_mappings_.end())
                            direct_to_ins.emplace_back(u_end, direct->second + (u_end - m_start));
                    }
                }
                for (auto k : to_del) {
                    mappings_.erase(k);
                    direct_mappings_.erase(k);
                }
                for (auto& p : to_ins) mappings_.insert(p);
                for (auto& p : direct_to_ins) direct_mappings_.insert(p);
            } else if (operation == 2 || operation == 4) { // protect
                if ((static_cast<unsigned char>(prot) & ~0x37) != 0 || !length || (length % page) != 0 || (start_addr % page) != 0) break;
                std::uint64_t cursor = start_addr;
                const std::uint64_t p_end = start_addr + length;
                if (p_end < start_addr) break;
                bool all_mapped = true;
                while (cursor < p_end) {
                    std::uint64_t base = 0;
                    std::size_t rsize = 0;
                    unsigned perm = 0;
                    if (!mem_.query_region(cursor, base, rsize, perm)) {
                        all_mapped = false;
                        break;
                    }
                    cursor = std::min(p_end, base + rsize);
                }
                if (!all_mapped) break;
                const unsigned permissions = ((prot & 0x11) ? static_cast<unsigned>(read) : 0u) | ((prot & 0x22) ? static_cast<unsigned>(write) : 0u);
                if (mem_.protect(start_addr, static_cast<std::size_t>(length), permissions) != MemoryError::ok) break;

                std::vector<std::uint64_t> to_del;
                std::vector<std::pair<std::uint64_t, std::uint64_t>> to_ins;
                std::vector<std::pair<std::uint64_t, std::uint64_t>> direct_to_ins;
                for (auto& m : mappings_) {
                    std::uint64_t m_start = m.first;
                    std::uint64_t m_size = m.second;
                    std::uint64_t m_end = m_start + m_size;
                    if (m_start >= p_end || m_end <= start_addr) continue;
                    to_del.push_back(m_start);
                    const auto direct = direct_mappings_.find(m_start);
                    if (start_addr > m_start) {
                        to_ins.emplace_back(m_start, start_addr - m_start);
                        if (direct != direct_mappings_.end()) direct_to_ins.emplace_back(m_start, direct->second);
                    }
                    const std::uint64_t mid_start = std::max(start_addr, m_start);
                    const std::uint64_t mid_end = std::min(p_end, m_end);
                    to_ins.emplace_back(mid_start, mid_end - mid_start);
                    if (direct != direct_mappings_.end()) {
                        direct_to_ins.emplace_back(mid_start, direct->second + (mid_start - m_start));
                    }
                    if (p_end < m_end) {
                        to_ins.emplace_back(p_end, m_end - p_end);
                        if (direct != direct_mappings_.end())
                            direct_to_ins.emplace_back(p_end, direct->second + (p_end - m_start));
                    }
                }
                for (auto k : to_del) {
                    mappings_.erase(k);
                    direct_mappings_.erase(k);
                }
                for (auto& p : to_ins) mappings_.insert(p);
                for (auto& p : direct_to_ins) direct_mappings_.insert(p);
            } else {
                break;
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
        if (!a(0)) return invalid;
        std::uint64_t dummy = 0;
        if (mem_.store(a(0), &dummy, sizeof(dummy)) != MemoryError::ok) return invalid;
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

        if (!is_sdk) {
            std::uint32_t dummy = 0;
            if (mem_.store(out_id_addr, &dummy, sizeof(dummy)) != MemoryError::ok) {
                return invalid;
            }
        }

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
            if (mem_.store(out_id_addr, &id, sizeof(id)) != MemoryError::ok) {
                KernelSyncManager::instance().delete_sema(id);
                sema_records_.erase(id);
                return invalid;
            }
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
        return current_tid_ ? current_tid_ : 1;
    }
    if (n=="scePthreadCreate" || n=="pthread_create" || n=="pthread_create_name_np") {
        return invalid;
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
        std::uint64_t dummy = 0;
        if (mem_.store(a(0), &dummy, sizeof(dummy)) != MemoryError::ok) return invalid;
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
            std::uint32_t dummy = 0;
            if (mem_.store(a(0), &dummy, sizeof(dummy)) != MemoryError::ok) return invalid;
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
        if (scheduler_) return scheduler_->now();
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    if (n=="sceKernelClockGettime" || n=="clock_gettime") {
        if (!a(1) || a(0)>4) return invalid;
        std::uint64_t ns = 0;
        if (a(0) == 0) {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        } else if (scheduler_) {
            ns = scheduler_->now() * 1000ULL;
        } else {
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        }
        const std::int64_t sec = static_cast<std::int64_t>(ns / 1000000000ULL);
        const std::int64_t nsec = static_cast<std::int64_t>(ns % 1000000000ULL);
        if (mem_.store(a(1), &sec, sizeof(sec)) != MemoryError::ok ||
            mem_.store(a(1) + sizeof(sec), &nsec, sizeof(nsec)) != MemoryError::ok) {
            return 0x8002000e; // EFAULT
        }
        return 0;
    }
    if (n=="sceSysmoduleLoadModule") {
        return static_cast<std::uint32_t>(default_sysmodule().sceSysmoduleLoadModule(static_cast<std::int32_t>(a(0))));
    }
    if (n=="sceSysmoduleUnloadModule") {
        return static_cast<std::uint32_t>(default_sysmodule().sceSysmoduleUnloadModule(static_cast<std::int32_t>(a(0))));
    }
    if (n=="sceSysmoduleIsLoaded") {
        return static_cast<std::uint32_t>(default_sysmodule().sceSysmoduleIsLoaded(static_cast<std::int32_t>(a(0))));
    }
    if (n=="sceAppContentInitialize") {
        return static_cast<std::uint32_t>(default_app_content().sceAppContentInitialize(mem_, a(0), a(1)));
    }
    if (n=="sceAppContentAppParamGetInt") {
        return static_cast<std::uint32_t>(default_app_content().sceAppContentAppParamGetInt(mem_, static_cast<std::uint32_t>(a(0)), a(1)));
    }
    if (n=="sceAppContentGetAddcontInfoList") {
        return static_cast<std::uint32_t>(default_app_content().sceAppContentGetAddcontInfoList(mem_, static_cast<std::uint32_t>(a(0)), a(1), static_cast<std::uint32_t>(a(2)), a(3)));
    }
    if (n=="sceAppContentGetPaddedAddcontInfo") {
        return static_cast<std::uint32_t>(default_app_content().sceAppContentGetPaddedAddcontInfo(mem_, static_cast<std::uint32_t>(a(0)), a(1), a(2)));
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
    if (n=="sceSaveDataDialogGetStatus") return 3; // FINISHED
    if (n=="sceSaveDataDialogGetResult") {
        if (a(0)) put<std::uint32_t>(mem_, a(0), 0); // SCE_OK
        return 0;
    }
    if (n=="sceSaveDataDialogIsReadyToDisplay") return 1;
    if (n=="sceSaveDataDialogClose") return 0;
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
        if (mem_.store(out, &val, sizeof(val)) != MemoryError::ok) {
            return 0x80A10003;
        }
        return 0;
    }
    if (n == "sceSystemServiceGetStatus") {
        auto out = a(0);
        if (!out) return 0x80A10003;
        std::uint8_t status[12] = {}; // event_num=0, no overlay, foreground, normal CPU mode
        if (mem_.store(out, status, sizeof(status)) != MemoryError::ok) {
            return 0x80A10003;
        }
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
        std::int32_t ctx_id = 0;
        auto res = default_ajm_manager().initialize(static_cast<std::int64_t>(a(0)), &ctx_id);
        if (a(1)) put<std::int32_t>(mem_, a(1), ctx_id);
        else if (a(0)) put<std::int32_t>(mem_, a(0), ctx_id);
        return static_cast<std::uint32_t>(res);
    }
    if (n == "sceAjmFinalize") {
        return static_cast<std::uint32_t>(default_ajm_manager().finalize(static_cast<std::int32_t>(a(0))));
    }
    if (n == "sceAjmModuleRegister") {
        return static_cast<std::uint32_t>(default_ajm_manager().module_register(static_cast<std::int32_t>(a(0)), static_cast<std::uint32_t>(a(1))));
    }
    if (n == "sceAjmModuleUnregister") {
        return static_cast<std::uint32_t>(default_ajm_manager().module_unregister(static_cast<std::int32_t>(a(0)), static_cast<std::uint32_t>(a(1))));
    }
    if (n == "sceAjmInstanceCreate") {
        std::int32_t inst_id = 0;
        auto ctx_id = static_cast<std::int32_t>(a(0));
        auto codec = static_cast<std::uint32_t>(a(1));
        auto param_addr = a(2);
        auto out_id_addr = a(3);
        if (!out_id_addr && param_addr) {
            out_id_addr = param_addr;
            param_addr = 0;
        }
        auto res = default_ajm_manager().instance_create(ctx_id, codec, mem_, param_addr, &inst_id);
        if (out_id_addr) put<std::int32_t>(mem_, out_id_addr, inst_id);
        return static_cast<std::uint32_t>(res);
    }
    if (n == "sceAjmInstanceDestroy") {
        return static_cast<std::uint32_t>(default_ajm_manager().instance_destroy(static_cast<std::int32_t>(a(0)), static_cast<std::int32_t>(a(1))));
    }
    if (n == "sceAjmBatchStartBuffer") {
        std::uint64_t batch_id = 0;
        auto res = default_ajm_manager().batch_start_buffer(static_cast<std::int32_t>(a(0)), &batch_id);
        if (a(3)) put<std::uint64_t>(mem_, a(3), batch_id);
        else if (a(1)) put<std::uint64_t>(mem_, a(1), batch_id);
        return static_cast<std::uint32_t>(res);
    }
    if (n == "sceAjmBatchJobRunBufferRa") {
        std::uint32_t written = 0;
        auto res = default_ajm_manager().batch_job_run_buffer(
            static_cast<std::int32_t>(a(0)), a(1), mem_, a(2), static_cast<std::uint32_t>(a(3)),
            a(4), static_cast<std::uint32_t>(a(5)), static_cast<std::int32_t>(a(6)), &written);
        return static_cast<std::uint32_t>(res);
    }
    if (n == "sceAjmBatchWait") {
        return static_cast<std::uint32_t>(default_ajm_manager().batch_wait(static_cast<std::int32_t>(a(0)), a(1), static_cast<std::uint32_t>(a(2))));
    }
    if (n == "sceAjmBatchCancel") {
        return static_cast<std::uint32_t>(default_ajm_manager().batch_cancel(static_cast<std::int32_t>(a(0)), a(1)));
    }
    if (n == "sceAjmBatchJobControlBufferRa" || n == "sceAjmBatchErrorDump") {
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
        std::uint64_t fmt_addr = a(0);
        if (!fmt_addr) return static_cast<std::uint64_t>(-1);
        std::string format = read_guest_string(fmt_addr, 4096);
        std::string out;
        bool ok = false;
        if (n == "printf") {
            auto getters = make_variadic_args(1);
            ok = format_printf(format, getters.get_int, getters.get_float, out);
        } else {
            auto getters = make_valist_args(a(1));
            ok = format_printf(format, getters.get_int, getters.get_float, out);
        }
        if (!ok) return static_cast<std::uint64_t>(-1);
        std::size_t written = std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
        if (written < out.size()) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(written);
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
    if (n == "sceKernelStat" || n == "stat" || n == "lstat") {
        if (!a(0) || !a(1)) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 14); // EFAULT
            return (n == "sceKernelStat") ? 0x80020016 : static_cast<std::uint64_t>(-1);
        }
        auto path = read_guest_string(a(0));
        if (path.empty() || VirtualFileSystem::contains_traversal(path)) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return (n == "sceKernelStat") ? 0x80020002 : static_cast<std::uint64_t>(-1);
        }
        auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
        auto vpath = resolve_guest_virtual_path(path);
        SceKernelStat sb{};
        bool stat_ok = false;
        if (active_vfs && !vpath.empty()) {
            VfsStat vstat{};
            if (active_vfs->stat(vpath, vstat) == VfsError::ok) {
                sb.st_mode = vstat.is_directory ? 0040777 : 0100777;
                sb.st_size = static_cast<std::int64_t>(vstat.size);
                sb.st_blksize = vstat.is_directory ? 65536 : 4096;
                sb.st_blocks = (sb.st_size + 511) / 512;
                sb.st_nlink = 1;
                auto host_path = resolve_guest_path(path);
                if (!host_path.empty()) {
                    SceKernelStat host_sb{};
                    if (fill_stat(host_path, host_sb)) {
                        sb.st_mtime_sec = host_sb.st_mtime_sec;
                        sb.st_mtime_nsec = host_sb.st_mtime_nsec;
                        sb.st_atime_sec = host_sb.st_atime_sec;
                        sb.st_ctime_sec = host_sb.st_ctime_sec;
                        sb.st_birthtime_sec = host_sb.st_birthtime_sec;
                    }
                }
                stat_ok = true;
            }
        }
        if (!stat_ok) {
            auto host_path = resolve_guest_path(path);
            if (host_path.empty() || !fill_stat(host_path, sb)) {
                auto err_addr = get_errno_addr(1);
                if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
                return (n == "sceKernelStat") ? 0x80020002 : static_cast<std::uint64_t>(-1);
            }
        }
        if (mem_.store(a(1), &sb, sizeof(sb)) != MemoryError::ok) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 14); // EFAULT
            return (n == "sceKernelStat") ? 0x80020016 : static_cast<std::uint64_t>(-1);
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
        bool stat_ok = false;
        if (it->second.is_vfs) {
            auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
            if (active_vfs) {
                VfsStat vstat{};
                if (active_vfs->stat(it->second.vfs_path, vstat) == VfsError::ok) {
                    sb.st_mode = vstat.is_directory ? 0040777 : 0100777;
                    sb.st_size = static_cast<std::int64_t>(vstat.size);
                    sb.st_blksize = 4096;
                    sb.st_blocks = (sb.st_size + 511) / 512;
                    sb.st_nlink = 1;
                    stat_ok = true;
                }
            }
        }
        if (!stat_ok && !fill_stat(it->second.path, sb)) {
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

    if (n == "sceKernelOpen" || n == "open" || n == "_open") {
        if (!a(0)) return (n == "open" || n == "_open") ? static_cast<std::uint64_t>(-1) : 0x80020016;
        auto path = read_guest_string(a(0));
        auto host_path = resolve_guest_path(path);
        auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
        auto vpath = resolve_guest_virtual_path(path);
        int flags = static_cast<int>(a(1));
        std::printf("[FILE] %s: '%s' -> '%s' (vpath='%s' flags=0x%x)\n",
                    n.c_str(), path.c_str(), host_path.c_str(), vpath.c_str(), flags);

        // Se o caminho viola o sandbox ou travessia, rejeita
        if (host_path.empty() && (!active_vfs || vpath.empty())) {
            return (n == "open" || n == "_open") ? static_cast<std::uint64_t>(-1) : 0x80020002;
        }

        bool is_write = ((flags & 0x0003) == 0x0001 || (flags & 0x0003) == 0x0002 || (flags & 0x0200));
        if (is_write && active_vfs) {
            std::string hp;
            bool ro = false;
            VfsStat vs{};
            if (!vpath.empty() && active_vfs->stat(vpath, vs) == VfsError::ok && vs.is_readonly) {
                return (n == "open" || n == "_open") ? static_cast<std::uint64_t>(-1) : 0x8002001e; // EROFS
            }
            if (!vpath.empty() && active_vfs->resolve(vpath, hp, ro) == VfsError::ok && ro) {
                return (n == "open" || n == "_open") ? static_cast<std::uint64_t>(-1) : 0x8002001e; // EROFS
            }
        }
        
        // Precedência de arquivos virtuais montados em BHD5/BND4 sobre o host (T119)
        std::string open_vpath;
        VfsStat vstat{};
        if (active_vfs) {
            if (!vpath.empty() && active_vfs->has_archive_file(vpath) && active_vfs->stat(vpath, vstat) == VfsError::ok && !vstat.is_directory) {
                open_vpath = vpath;
            } else if (active_vfs->has_archive_file(path) && active_vfs->stat(path, vstat) == VfsError::ok && !vstat.is_directory) {
                open_vpath = path;
            }
        }
        if (!open_vpath.empty()) {
            if (is_write) {
                return (n == "open" || n == "_open") ? static_cast<std::uint64_t>(-1) : 0x8002001e; // EROFS
            }
            int fd = next_kernel_fd_++;
            std::uint64_t fsz = vstat.size;
            kernel_files_[fd] = KernelFileEntry{nullptr, path, open_vpath, flags, fsz, 0, true, true};
            std::printf("[FILE] %s: '%s' -> (VFS read_at) fd=%d size=%llu\n",
                        n.c_str(), path.c_str(), fd, (unsigned long long)fsz);
            return static_cast<std::uint64_t>(fd);
        }

        // Modos de abertura host com preservação byte a byte e conformidade com O_TRUNC/O_CREAT (T116)
        bool has_trunc = (flags & 0x0400) != 0;
        bool has_append = (flags & 0x0008) != 0;
        bool has_creat = (flags & 0x0200) != 0;
        bool is_rdwr = ((flags & 0x0003) == 0x0002);
        bool is_wronly = ((flags & 0x0003) == 0x0001);

        const char* fmode = "rb";
        std::error_code ec_ex;
        bool file_exists = !host_path.empty() && std::filesystem::exists(host_path, ec_ex) && !std::filesystem::is_directory(host_path, ec_ex);

        if (has_trunc) {
            fmode = is_rdwr ? "w+b" : "wb";
        } else if (has_append) {
            fmode = is_rdwr ? "a+b" : "ab";
        } else if (is_rdwr) {
            fmode = "r+b";
        } else if (is_wronly) {
            if (file_exists) {
                fmode = "r+b"; // Preserva conteudo preexistente byte a byte; escrita sem truncar
            } else if (has_creat) {
                fmode = "wb";  // Cria novo arquivo vazio
            } else {
                fmode = "r+b"; // Nao existe: tentara r+b e falhara com ENOENT
            }
        } else if (has_creat) {
            if (file_exists) {
                fmode = is_rdwr ? "r+b" : "rb";
            } else {
                fmode = is_rdwr ? "w+b" : "wb";
            }
        } else {
            fmode = "rb";
        }

        FILE* fp = host_path.empty() ? nullptr : portable_fopen(host_path, fmode);
        if (!fp && has_creat && !file_exists && !host_path.empty()) {
            fp = portable_fopen(host_path, is_rdwr ? "w+b" : "wb");
        }
        if (!fp && active_vfs) {
            VfsStat fallback_vstat{};
            std::string fallback_vpath;
            if (!vpath.empty() && active_vfs->stat(vpath, fallback_vstat) == VfsError::ok && !fallback_vstat.is_directory) {
                fallback_vpath = vpath;
            } else if (active_vfs->stat(path, fallback_vstat) == VfsError::ok && !fallback_vstat.is_directory) {
                fallback_vpath = path;
            } else if (!host_path.empty() && active_vfs->stat(host_path, fallback_vstat) == VfsError::ok && !fallback_vstat.is_directory) {
                fallback_vpath = host_path;
            }
            if (!fallback_vpath.empty()) {
                // Rejeita tentativas de escrita em arquivos virtuais/read-only VFS
                if (is_write) {
                    return (n == "open" || n == "_open") ? static_cast<std::uint64_t>(-1) : 0x8002001e; // EROFS
                }
                int fd = next_kernel_fd_++;
                std::uint64_t fsz = fallback_vstat.size;
                kernel_files_[fd] = KernelFileEntry{nullptr, path, fallback_vpath, flags, fsz, 0, true, true};
                std::printf("[FILE] %s: '%s' -> (VFS read_at) fd=%d size=%llu\n",
                            n.c_str(), path.c_str(), fd, (unsigned long long)fsz);
                return static_cast<std::uint64_t>(fd);
            }
        }
        if (!fp) {
            return (n == "open" || n == "_open") ? static_cast<std::uint64_t>(-1) : 0x80020002; // ENOENT
        }

        std::uint64_t fsz = 0;
        std::error_code ec;
        if (std::filesystem::exists(host_path, ec) && !std::filesystem::is_directory(host_path, ec)) {
            fsz = std::filesystem::file_size(host_path, ec);
        }

        int fd = next_kernel_fd_++;
        kernel_files_[fd] = KernelFileEntry{fp, host_path, vpath, flags, fsz, 0, false, false};
        std::printf("[FILE] %s: '%s' -> fd=%d size=%llu\n", n.c_str(), path.c_str(), fd, (unsigned long long)fsz);
        return static_cast<std::uint64_t>(fd);
    }

    if (n == "sceKernelClose" || n == "close" || n == "_close") {
        int fd = static_cast<int>(a(0));
        auto it = kernel_files_.find(fd);
        if (it == kernel_files_.end()) {
            std::printf("[FILE] %s: fd=%d EBADF\n", n.c_str(), fd);
            return (n == "close" || n == "_close") ? static_cast<std::uint64_t>(-1) : 0x80020009; // EBADF
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
        if (!buf && nbyte > 0) return (n == "sceKernelRead") ? 0x80020016 : static_cast<std::uint64_t>(-1);
        if (it == kernel_files_.end()) {
            return (n == "sceKernelRead") ? 0x80020009 : static_cast<std::uint64_t>(-1);
        }
        if (nbyte == 0) return 0;

        if (it->second.is_vfs) {
            auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
            if (!active_vfs) {
                return (n == "sceKernelRead") ? 0x80020009 : static_cast<std::uint64_t>(-1);
            }
            if (it->second.seek_pos >= it->second.size) {
                return 0; // EOF
            }
            std::uint64_t avail = it->second.size - it->second.seek_pos;
            std::size_t to_transfer = static_cast<std::size_t>(std::min<std::uint64_t>(nbyte, avail));
            constexpr std::size_t kChunkSize = 64 * 1024;
            std::vector<std::uint8_t> chunk(kChunkSize);
            std::size_t total_read = 0;
            VfsError last_err = VfsError::ok;
            while (total_read < to_transfer) {
                std::size_t step = std::min(to_transfer - total_read, kChunkSize);
                std::size_t out_bytes = 0;
                last_err = active_vfs->read_at(it->second.vfs_path,
                                               it->second.seek_pos + total_read,
                                               chunk.data(),
                                               step,
                                               out_bytes);
                if (last_err != VfsError::ok || out_bytes == 0) break;
                if (mem_.store(buf + total_read, chunk.data(), out_bytes) != MemoryError::ok) {
                    return (n == "sceKernelRead") ? 0x80020016 : static_cast<std::uint64_t>(-1);
                }
                total_read += out_bytes;
                if (out_bytes < step) break;
            }
            if (total_read > 0) {
                it->second.seek_pos += total_read;
                return static_cast<std::uint64_t>(total_read);
            }
            if (last_err != VfsError::ok) {
                if (n == "sceKernelRead") {
                    if (last_err == VfsError::access_denied) return 0x8002000d;
                    if (last_err == VfsError::not_found) return 0x80020002;
                    return 0x80020005; // EIO
                }
                return static_cast<std::uint64_t>(-1);
            }
            return 0; // EOF genuíno
        }

        if (!it->second.fp) {
            return (n == "sceKernelRead") ? 0x80020009 : static_cast<std::uint64_t>(-1);
        }
        constexpr std::size_t kChunkSize = 64 * 1024;
        std::vector<std::uint8_t> tmp(kChunkSize);
        std::size_t total_read = 0;
        while (total_read < nbyte) {
            std::size_t step = std::min(nbyte - total_read, kChunkSize);
            std::size_t bytes_read = std::fread(tmp.data(), 1, step, it->second.fp);
            if (bytes_read == 0) break;
            if (mem_.store(buf + total_read, tmp.data(), bytes_read) != MemoryError::ok) {
                return (n == "sceKernelRead") ? 0x80020016 : static_cast<std::uint64_t>(-1);
            }
            total_read += bytes_read;
            it->second.seek_pos += bytes_read;
            if (bytes_read < step) break;
        }
        return static_cast<std::uint64_t>(total_read);
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
        if (!buf && nbyte > 0) return 0x80020016;
        if (it == kernel_files_.end()) return 0x80020009;
        if (offset < 0) return 0x80020016;
        if (nbyte == 0) return 0;

        if (it->second.is_vfs) {
            if (static_cast<std::uint64_t>(offset) >= it->second.size) {
                return 0; // EOF per POSIX pread
            }
            auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
            if (!active_vfs) return 0x80020009;

            std::uint64_t avail = it->second.size - static_cast<std::uint64_t>(offset);
            std::size_t to_transfer = static_cast<std::size_t>(std::min<std::uint64_t>(nbyte, avail));
            constexpr std::size_t kChunkSize = 64 * 1024;
            std::vector<std::uint8_t> chunk(kChunkSize);
            std::size_t total_read = 0;
            VfsError last_err = VfsError::ok;
            while (total_read < to_transfer) {
                std::size_t step = std::min(to_transfer - total_read, kChunkSize);
                std::size_t out_bytes = 0;
                last_err = active_vfs->read_at(it->second.vfs_path,
                                               static_cast<std::uint64_t>(offset) + total_read,
                                               chunk.data(),
                                               step,
                                               out_bytes);
                if (last_err != VfsError::ok || out_bytes == 0) break;
                if (mem_.store(buf + total_read, chunk.data(), out_bytes) != MemoryError::ok) {
                    return 0x80020016;
                }
                total_read += out_bytes;
                if (out_bytes < step) break;
            }
            if (total_read > 0) {
                return static_cast<std::uint64_t>(total_read);
            }
            if (last_err != VfsError::ok) {
                if (last_err == VfsError::access_denied) return 0x8002000d;
                if (last_err == VfsError::not_found) return 0x80020002;
                return 0x80020005; // EIO
            }
            return 0;
        }

        if (!it->second.fp) {
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
        constexpr std::size_t kChunkSize = 64 * 1024;
        std::vector<std::uint8_t> tmp(kChunkSize);
        std::size_t total_read = 0;
        bool store_err = false;
        while (total_read < nbyte) {
            std::size_t step = std::min(nbyte - total_read, kChunkSize);
            std::size_t bytes_read = std::fread(tmp.data(), 1, step, fp);
            if (bytes_read == 0) break;
            if (mem_.store(buf + total_read, tmp.data(), bytes_read) != MemoryError::ok) {
                store_err = true;
                break;
            }
            total_read += bytes_read;
            if (bytes_read < step) break;
        }
#if defined(_WIN32)
        _fseeki64(fp, cur, SEEK_SET);
#else
        fseeko(fp, cur, SEEK_SET);
#endif
        if (store_err) return 0x80020016;
        return static_cast<std::uint64_t>(total_read);
    }

    if (n == "sceKernelWrite" || n == "write" || n == "_write") {
        int fd = static_cast<int>(a(0));
        std::uint64_t buf = a(1);
        std::size_t nbyte = static_cast<std::size_t>(a(2));
        if (!buf && nbyte > 0) return (n == "sceKernelWrite") ? 0x80020016 : static_cast<std::uint64_t>(-1);
        if (fd == 1 || fd == 2) {
            if (nbyte == 0) return 0;
            FILE* out_fp = (fd == 2) ? stderr : stdout;
            constexpr std::size_t kChunkSize = 64 * 1024;
            std::vector<char> tmp(kChunkSize);
            std::size_t total_written = 0;
            while (total_written < nbyte) {
                std::size_t step = std::min(nbyte - total_written, kChunkSize);
                if (mem_.load(buf + total_written, tmp.data(), step) != MemoryError::ok) {
                    if (total_written > 0) break;
                    return (n == "sceKernelWrite") ? 0x80020016 : static_cast<std::uint64_t>(-1);
                }
                std::size_t bytes_written = std::fwrite(tmp.data(), 1, step, out_fp);
                total_written += bytes_written;
                if (bytes_written < step) break;
            }
            std::fflush(out_fp);
            return static_cast<std::uint64_t>(total_written);
        }
        auto it = kernel_files_.find(fd);
        if (it == kernel_files_.end() || !it->second.fp) {
            return (n == "sceKernelWrite") ? 0x80020009 : static_cast<std::uint64_t>(-1);
        }
        constexpr std::size_t kChunkSize = 64 * 1024;
        std::vector<std::uint8_t> tmp(kChunkSize);
        std::size_t total_written = 0;
        while (total_written < nbyte) {
            std::size_t step = std::min(nbyte - total_written, kChunkSize);
            if (mem_.load(buf + total_written, tmp.data(), step) != MemoryError::ok) {
                return (n == "sceKernelWrite") ? 0x80020016 : static_cast<std::uint64_t>(-1);
            }
            std::size_t bytes_written = std::fwrite(tmp.data(), 1, step, it->second.fp);
            total_written += bytes_written;
            it->second.seek_pos += bytes_written;
            if (bytes_written < step) break;
        }
        return static_cast<std::uint64_t>(total_written);
    }

    if (n == "sceKernelPwrite") {
        int fd = static_cast<int>(a(0));
        std::uint64_t buf = a(1);
        std::size_t nbyte = static_cast<std::size_t>(a(2));
        std::int64_t offset = static_cast<std::int64_t>(a(3));
        if (!buf && nbyte > 0) return 0x80020016;
        if (offset < 0) return 0x80020016;
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
        constexpr std::size_t kChunkSize = 64 * 1024;
        std::vector<std::uint8_t> tmp(kChunkSize);
        std::size_t total_written = 0;
        bool load_err = false;
        while (total_written < nbyte) {
            std::size_t step = std::min(nbyte - total_written, kChunkSize);
            if (mem_.load(buf + total_written, tmp.data(), step) != MemoryError::ok) {
                load_err = true;
                break;
            }
            std::size_t bytes_written = std::fwrite(tmp.data(), 1, step, fp);
            total_written += bytes_written;
            if (bytes_written < step) break;
        }
#if defined(_WIN32)
        _fseeki64(fp, cur, SEEK_SET);
#else
        fseeko(fp, cur, SEEK_SET);
#endif
        if (load_err) return 0x80020016;
        return static_cast<std::uint64_t>(total_written);
    }

    if (n == "sceKernelLseek" || n == "lseek" || n == "_lseek") {
        int fd = static_cast<int>(a(0));
        std::int64_t offset = static_cast<std::int64_t>(a(1));
        int whence = static_cast<int>(a(2));
        auto it = kernel_files_.find(fd);
        if (it == kernel_files_.end()) {
            return (n == "lseek" || n == "_lseek") ? static_cast<std::uint64_t>(-1) : 0x80020009;
        }
        if (it->second.is_vfs) {
            std::int64_t new_pos = 0;
            if (whence == 0) { // SEEK_SET
                new_pos = offset;
            } else if (whence == 1) { // SEEK_CUR
                new_pos = static_cast<std::int64_t>(it->second.seek_pos) + offset;
            } else if (whence == 2) { // SEEK_END
                new_pos = static_cast<std::int64_t>(it->second.size) + offset;
            } else {
                return (n == "lseek" || n == "_lseek") ? static_cast<std::uint64_t>(-1) : 0x80020016;
            }
            if (new_pos < 0) return (n == "lseek" || n == "_lseek") ? static_cast<std::uint64_t>(-1) : 0x80020016;
            it->second.seek_pos = static_cast<std::uint64_t>(new_pos);
            return static_cast<std::uint64_t>(it->second.seek_pos);
        }
        if (!it->second.fp) {
            return (n == "lseek" || n == "_lseek") ? static_cast<std::uint64_t>(-1) : 0x80020009;
        }
#if defined(_WIN32)
        if (_fseeki64(it->second.fp, offset, whence) != 0) {
            return (n == "lseek") ? static_cast<std::uint64_t>(-1) : 0x80020016;
        }
        auto cur = static_cast<std::uint64_t>(_ftelli64(it->second.fp));
        it->second.seek_pos = cur;
        return cur;
#else
        if (fseeko(it->second.fp, offset, whence) != 0) {
            return (n == "lseek") ? static_cast<std::uint64_t>(-1) : 0x80020016;
        }
        auto cur = static_cast<std::uint64_t>(ftello(it->second.fp));
        it->second.seek_pos = cur;
        return cur;
#endif
    }

    if (n == "sceKernelFtruncate" || n == "ftruncate") {
        int fd = static_cast<int>(a(0));
        std::int64_t length = static_cast<std::int64_t>(a(1));
        auto it = kernel_files_.find(fd);
        if (it == kernel_files_.end()) {
            if (n == "ftruncate") {
                auto err_addr = get_errno_addr(1);
                if (err_addr) put<std::int32_t>(mem_, err_addr, 9); // EBADF
                return static_cast<std::uint64_t>(-1);
            }
            return 0x80020009; // EBADF
        }
        if (length < 0) {
            if (n == "ftruncate") {
                auto err_addr = get_errno_addr(1);
                if (err_addr) put<std::int32_t>(mem_, err_addr, 22); // EINVAL
                return static_cast<std::uint64_t>(-1);
            }
            return 0x80020016; // EINVAL
        }
        if (it->second.is_vfs || (it->second.flags & 0x0003) == 0 || !it->second.fp) {
            if (n == "ftruncate") {
                auto err_addr = get_errno_addr(1);
                if (err_addr) put<std::int32_t>(mem_, err_addr, 22); // EINVAL
                return static_cast<std::uint64_t>(-1);
            }
            return 0x80020016; // EINVAL
        }
#if defined(_WIN32)
        int win_fd = _fileno(it->second.fp);
        if (win_fd < 0) {
            if (n == "ftruncate") {
                auto err_addr = get_errno_addr(1);
                if (err_addr) put<std::int32_t>(mem_, err_addr, 9); // EBADF
                return static_cast<std::uint64_t>(-1);
            }
            return 0x80020009;
        }
        errno_t err = _chsize_s(win_fd, length);
        if (err != 0) {
            if (n == "ftruncate") {
                auto err_addr = get_errno_addr(1);
                if (err_addr) put<std::int32_t>(mem_, err_addr, static_cast<std::int32_t>(err));
                return static_cast<std::uint64_t>(-1);
            }
            return 0x80020000 | static_cast<uint32_t>(err);
        }
#else
        int posix_fd = fileno(it->second.fp);
        if (posix_fd < 0) {
            if (n == "ftruncate") {
                auto err_addr = get_errno_addr(1);
                if (err_addr) put<std::int32_t>(mem_, err_addr, 9);
                return static_cast<std::uint64_t>(-1);
            }
            return 0x80020009;
        }
        if (ftruncate(posix_fd, length) != 0) {
            int err = errno;
            if (n == "ftruncate") {
                auto err_addr = get_errno_addr(1);
                if (err_addr) put<std::int32_t>(mem_, err_addr, err);
                return static_cast<std::uint64_t>(-1);
            }
            return 0x80020000 | static_cast<uint32_t>(err);
        }
#endif
        it->second.size = static_cast<std::uint64_t>(length);
        return 0;
    }

    if (n == "sceKernelCheckReachability") {
        if (!a(0)) return 0x80020016;
        auto path = read_guest_string(a(0));
        auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
        auto vpath = resolve_guest_virtual_path(path);
        if (active_vfs && !vpath.empty() && active_vfs->exists(vpath)) {
            return 0;
        }
        auto host_path = resolve_guest_path(path);
        std::error_code ec;
        if (!std::filesystem::exists(host_path, ec)) {
            return 0x80020002; // ENOENT
        }
        return 0;
    }

    if (n == "mkdir") {
        if (!a(0)) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 14); // EFAULT
            return static_cast<std::uint64_t>(-1);
        }
        auto path = read_guest_string(a(0));
        if (path.empty() || VirtualFileSystem::contains_traversal(path)) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return static_cast<std::uint64_t>(-1);
        }
        bool readonly = false;
        auto host_path = resolve_guest_path(path, &readonly);
        auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
        auto vpath = resolve_guest_virtual_path(path);
        if (active_vfs && !vpath.empty()) {
            VfsStat vs{};
            if (active_vfs->stat(vpath, vs) == VfsError::ok && vs.is_readonly) {
                readonly = true;
            }
        }
        if (readonly) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 30); // EROFS
            return static_cast<std::uint64_t>(-1);
        }
        if (host_path.empty()) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return static_cast<std::uint64_t>(-1);
        }
        std::error_code ec;
        std::filesystem::create_directories(host_path, ec);
        if (ec) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 13); // EACCES
            return static_cast<std::uint64_t>(-1);
        }
        return 0;
    }

    if (n == "rename") {
        if (!a(0) || !a(1)) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 14); // EFAULT
            return static_cast<std::uint64_t>(-1);
        }
        auto old_path = read_guest_string(a(0));
        auto new_path = read_guest_string(a(1));
        if (old_path.empty() || new_path.empty() ||
            VirtualFileSystem::contains_traversal(old_path) ||
            VirtualFileSystem::contains_traversal(new_path)) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return static_cast<std::uint64_t>(-1);
        }
        bool old_ro = false, new_ro = false;
        auto old_p = resolve_guest_path(old_path, &old_ro);
        auto new_p = resolve_guest_path(new_path, &new_ro);
        auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
        auto old_vp = resolve_guest_virtual_path(old_path);
        auto new_vp = resolve_guest_virtual_path(new_path);
        if (active_vfs) {
            VfsStat vs{};
            if (!old_vp.empty() && active_vfs->stat(old_vp, vs) == VfsError::ok && vs.is_readonly) old_ro = true;
            if (!new_vp.empty() && active_vfs->stat(new_vp, vs) == VfsError::ok && vs.is_readonly) new_ro = true;
        }
        if (old_ro || new_ro) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 30); // EROFS
            return static_cast<std::uint64_t>(-1);
        }
        if (old_p.empty() || new_p.empty()) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return static_cast<std::uint64_t>(-1);
        }
        std::error_code ec;
        std::filesystem::rename(old_p, new_p, ec);
        if (ec) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 13); // EACCES
            return static_cast<std::uint64_t>(-1);
        }
        return 0;
    }

    if (n=="fopen") {
        if (!a(0) || !a(1)) return 0;
        std::string path = read_guest_string(a(0));
        std::string mode = read_guest_string(a(1));
        if (path.empty() || mode.empty() || VirtualFileSystem::contains_traversal(path)) return 0;
        bool readonly = false;
        auto host_path = resolve_guest_path(path, &readonly);
        if (host_path.empty()) return 0;

        auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
        auto vpath = resolve_guest_virtual_path(path);
        if (active_vfs && !vpath.empty()) {
            VfsStat vs{};
            if (active_vfs->stat(vpath, vs) == VfsError::ok && vs.is_readonly) {
                readonly = true;
            }
        }

        bool is_write = (mode.find('w') != std::string::npos ||
                         mode.find('a') != std::string::npos ||
                         mode.find('+') != std::string::npos);
        if (readonly && is_write) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 30); // EROFS
            return 0;
        }
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

    // pthreads (T150: identidade, espera e saida com sincronizacao no scheduler)
    if (n=="pthread_self" || n=="scePthreadSelf" || n=="pthread_getthreadid_np") {
        return current_tid_ ? current_tid_ : 1;
    }
    if (n=="pthread_join" || n=="scePthreadJoin") {
        if (!scheduler_) return invalid;
        if (a(1)) {
            std::uint64_t probe = 0;
            if (mem_.load(a(1), &probe, 8) != MemoryError::ok || mem_.store(a(1), &probe, 8) != MemoryError::ok)
                return invalid;
        }
        const auto target = static_cast<GuestScheduler::Id>(a(0));
        if (target == current_tid_) return ORBIS_KERNEL_ERROR_EDEADLK;
        if (!scheduler_->has_thread(target)) return ORBIS_KERNEL_ERROR_ESRCH;
        std::uint64_t val = 0;
        auto status = scheduler_->join(current_tid_, target, &val);
        if (status) return static_cast<std::uint32_t>(status);
        if (a(1)) put(mem_, a(1), val);
        return 0;
    }
    if (n=="pthread_exit" || n=="scePthreadExit") {
        if (!scheduler_) return invalid;
        scheduler_->exit(current_tid_, a(0));
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

    // signals (T151: isolamento de mascaras, validacao atomica de ponteiros e semanticas BSD/Orbis)
    if (n=="signal") {
        int signo = static_cast<int>(a(0));
        if (signo < 1 || signo > 128 || signo == 9 || signo == 17 || signo == 19) {
            return ~0ULL; // SIG_ERR (SIGKILL 9 e SIGSTOP 17/19 nao podem ser capturados/ignorados)
        }
        std::uint64_t new_handler = a(1);
        std::uint64_t old_handler = signal_actions_[signo].sa_handler;
        signal_actions_[signo].sa_handler = new_handler;
        return old_handler;
    }
    if (n=="raise") {
        int signo = static_cast<int>(a(0));
        if (signo < 1 || signo > 128) return 0x16; // EINVAL
        return 0;
    }
    if (n=="sigaction") {
        int signo = static_cast<int>(a(0));
        if (signo < 1 || signo > 128 || signo == 9 || signo == 17 || signo == 19) {
            return 0x16; // EINVAL
        }
        auto act = a(1);
        auto oact = a(2);
        struct GuestSigActionRaw {
            std::uint64_t handler;
            std::uint32_t flags;
            std::uint32_t pad;
            std::uint32_t mask[4];
        };
        // Validacao atomica prévia dos ponteiros antes de alterar o estado
        if (oact) {
            GuestSigActionRaw dummy{};
            if (mem_.store(oact, &dummy, sizeof(dummy)) != MemoryError::ok) {
                return 0x0E; // EFAULT
            }
        }
        GuestSigActionRaw act_raw{};
        if (act) {
            if (mem_.load(act, &act_raw, sizeof(act_raw)) != MemoryError::ok) {
                return 0x0E; // EFAULT
            }
        }
        if (oact) {
            GuestSigAction cur = signal_actions_[signo];
            GuestSigActionRaw raw{};
            raw.handler = cur.sa_handler;
            raw.flags = cur.sa_flags;
            raw.pad = 0;
            std::memcpy(raw.mask, cur.sa_mask.data(), sizeof(raw.mask));
            mem_.store(oact, &raw, sizeof(raw));
        }
        if (act) {
            GuestSigAction& entry = signal_actions_[signo];
            entry.sa_handler = act_raw.handler;
            entry.sa_flags = act_raw.flags;
            std::memcpy(entry.sa_mask.data(), act_raw.mask, sizeof(entry.sa_mask));
        }
        return 0;
    }
    if (n=="sigemptyset") {
        auto set = a(0);
        if (!set) return 0x0E; // EFAULT
        std::uint32_t zeros[4] = {0, 0, 0, 0};
        if (mem_.store(set, zeros, sizeof(zeros)) != MemoryError::ok) {
            return 0x0E; // EFAULT
        }
        return 0;
    }
    if (n=="sigfillset") {
        auto set = a(0);
        if (!set) return 0x0E; // EFAULT
        std::uint32_t ones[4] = {~0U, ~0U, ~0U, ~0U};
        if (mem_.store(set, ones, sizeof(ones)) != MemoryError::ok) {
            return 0x0E; // EFAULT
        }
        return 0;
    }
    if (n=="sigaddset" || n=="sigdelset" || n=="sigismember") {
        auto set = a(0);
        if (!set) return 0x0E; // EFAULT
        int signo = static_cast<int>(a(1));
        if (signo < 1 || signo > 128) return 0x16; // EINVAL
        std::uint32_t mask[4] = {};
        if (mem_.load(set, mask, sizeof(mask)) != MemoryError::ok) {
            return 0x0E; // EFAULT
        }
        size_t idx = static_cast<size_t>((signo - 1) / 32);
        std::uint32_t bit = 1U << ((signo - 1) % 32);
        if (n == "sigismember") {
            return (mask[idx] & bit) ? 1 : 0;
        }
        if (n == "sigaddset") {
            mask[idx] |= bit;
        } else {
            mask[idx] &= ~bit;
        }
        if (mem_.store(set, mask, sizeof(mask)) != MemoryError::ok) {
            return 0x0E; // EFAULT
        }
        return 0;
    }
    if (n=="sigprocmask" || n=="_sigprocmask" || n=="pthread_sigmask") {
        auto how = a(0);
        auto set = a(1);
        auto old = a(2);
        // Validacao prévia dos ponteiros
        if (old) {
            std::uint32_t probe[4] = {};
            if (mem_.store(old, probe, sizeof(probe)) != MemoryError::ok) {
                return 0x0E; // EFAULT
            }
        }
        std::uint32_t new_mask[4] = {};
        if (set) {
            if (how < 1 || how > 3) return 0x16; // EINVAL
            if (mem_.load(set, new_mask, sizeof(new_mask)) != MemoryError::ok) {
                return 0x0E; // EFAULT
            }
        }
        auto& current_mask = get_signal_mask(current_tid_);
        if (old) {
            mem_.store(old, current_mask.data(), sizeof(std::uint32_t) * 4);
        }
        if (set) {
            for (int i = 0; i < 4; ++i) {
                if (how == 1) current_mask[i] |= new_mask[i]; // SIG_BLOCK
                else if (how == 2) current_mask[i] &= ~new_mask[i]; // SIG_UNBLOCK
                else if (how == 3) current_mask[i] = new_mask[i]; // SIG_SETMASK
            }
            // SIGKILL (9) e SIGSTOP (17/19) nao podem ser bloqueados
            current_mask[0] &= ~((1U << (9 - 1)) | (1U << (17 - 1)) | (1U << (19 - 1)));
        }
        return 0;
    }
    if (n=="sigpending") {
        auto set = a(0);
        if (!set) return 0x0E; // EFAULT
        std::uint32_t zeros[4] = {0, 0, 0, 0};
        if (mem_.store(set, zeros, sizeof(zeros)) != MemoryError::ok) {
            return 0x0E; // EFAULT
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
        if (mem_.load(name_ptr, name, sizeof(name)) != MemoryError::ok) {
            return 0x0E; // EFAULT
        }
        if (name[0] == 1 && name[1] == 37) { // CTL_KERN, KERN_ARANDOM
            if (!old_ptr || !oldlen_ptr) return 0x0E; // EFAULT
            std::uint64_t len = 0;
            if (mem_.load(oldlen_ptr, &len, sizeof(len)) != MemoryError::ok) {
                return 0x0E; // EFAULT
            }
            if (len == 0) return 0;
            constexpr std::size_t MAX_ARANDOM_LEN = 256;
            std::size_t to_write = std::min<std::size_t>(static_cast<std::size_t>(len), MAX_ARANDOM_LEN);
            std::array<std::uint8_t, MAX_ARANDOM_LEN> rand_data{};
            for (std::size_t i = 0; i < to_write; ++i) {
                rand_data[i] = static_cast<std::uint8_t>(std::rand() & 0xFF);
            }
            if (mem_.store(old_ptr, rand_data.data(), to_write) != MemoryError::ok) {
                return 0x0E; // EFAULT
            }
            std::uint64_t written_len = to_write;
            if (mem_.store(oldlen_ptr, &written_len, sizeof(written_len)) != MemoryError::ok) {
                return 0x0E; // EFAULT
            }
            return 0;
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
        if (!a(0)) return 0;
        std::int64_t raw_t = 0;
        if (mem_.load(a(0), &raw_t, sizeof(raw_t)) != MemoryError::ok) {
            if (a(0) > 1000000000ULL && a(0) < 3000000000ULL) {
                raw_t = static_cast<std::int64_t>(a(0));
            } else {
                return 0;
            }
        }
        std::time_t t = static_cast<std::time_t>(raw_t);
        std::tm* tm = std::localtime(&t);
        if (!tm) return 0;
        std::uint64_t buf = get_static_libc_buf(0, 64);
        put<std::int32_t>(mem_, buf + 0, tm->tm_sec);
        put<std::int32_t>(mem_, buf + 4, tm->tm_min);
        put<std::int32_t>(mem_, buf + 8, tm->tm_hour);
        put<std::int32_t>(mem_, buf + 12, tm->tm_mday);
        put<std::int32_t>(mem_, buf + 16, tm->tm_mon);
        put<std::int32_t>(mem_, buf + 20, tm->tm_year);
        put<std::int32_t>(mem_, buf + 24, tm->tm_wday);
        put<std::int32_t>(mem_, buf + 28, tm->tm_yday);
        put<std::int32_t>(mem_, buf + 32, tm->tm_isdst);
        return buf;
    }
    if (n=="gmtime") {
        if (!a(0)) return 0;
        std::int64_t raw_t = 0;
        if (mem_.load(a(0), &raw_t, sizeof(raw_t)) != MemoryError::ok) {
            if (a(0) > 1000000000ULL && a(0) < 3000000000ULL) {
                raw_t = static_cast<std::int64_t>(a(0));
            } else {
                return 0;
            }
        }
        std::time_t t = static_cast<std::time_t>(raw_t);
        std::tm* tm = std::gmtime(&t);
        if (!tm) return 0;
        std::uint64_t buf = get_static_libc_buf(0, 64);
        put<std::int32_t>(mem_, buf + 0, tm->tm_sec);
        put<std::int32_t>(mem_, buf + 4, tm->tm_min);
        put<std::int32_t>(mem_, buf + 8, tm->tm_hour);
        put<std::int32_t>(mem_, buf + 12, tm->tm_mday);
        put<std::int32_t>(mem_, buf + 16, tm->tm_mon);
        put<std::int32_t>(mem_, buf + 20, tm->tm_year);
        put<std::int32_t>(mem_, buf + 24, tm->tm_wday);
        put<std::int32_t>(mem_, buf + 28, tm->tm_yday);
        put<std::int32_t>(mem_, buf + 32, tm->tm_isdst);
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
        std::uint64_t buf = get_static_libc_buf(64, 64);
        char out[64];
        std::snprintf(out, sizeof(out), "%.3s %.3s %2d %.2d:%.2d:%.2d %d\n",
            wday_name[0], mon_name[mon % 12], mday, hour, min, sec, 1900 + year);
        mem_.store(buf, out, std::strlen(out) + 1);
        return buf;
    }
    if (n=="ctime") {
        if (!a(0)) return 0;
        std::int64_t raw_t = 0;
        if (mem_.load(a(0), &raw_t, sizeof(raw_t)) != MemoryError::ok) {
            if (a(0) > 1000000000ULL && a(0) < 3000000000ULL) {
                raw_t = static_cast<std::int64_t>(a(0));
            } else {
                return 0;
            }
        }
        std::time_t t = static_cast<std::time_t>(raw_t);
        const char* s = std::ctime(&t);
        if (!s) return 0;
        std::size_t slen = std::strlen(s);
        std::uint64_t buf = get_static_libc_buf(128, 64);
        mem_.store(buf, s, slen + 1);
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
    if (n=="remove" || n=="rmdir") {
        if (!a(0)) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 14); // EFAULT
            return static_cast<std::uint64_t>(-1);
        }
        std::string path = read_guest_string(a(0));
        if (path.empty() || VirtualFileSystem::contains_traversal(path)) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return static_cast<std::uint64_t>(-1);
        }
        bool readonly = false;
        auto host_path = resolve_guest_path(path, &readonly);
        auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
        auto vpath = resolve_guest_virtual_path(path);
        if (active_vfs && !vpath.empty()) {
            VfsStat vs{};
            if (active_vfs->stat(vpath, vs) == VfsError::ok && vs.is_readonly) {
                readonly = true;
            }
        }
        if (readonly) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 30); // EROFS
            return static_cast<std::uint64_t>(-1);
        }
        if (host_path.empty()) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return static_cast<std::uint64_t>(-1);
        }
        std::error_code ec;
        std::filesystem::remove(host_path, ec);
        if (ec) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return static_cast<std::uint64_t>(-1);
        }
        return 0;
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
        if (!a(0) || !a(1)) return 0;
        std::string path = read_guest_string(a(0));
        std::string mode = read_guest_string(a(1));
        auto it = open_files_.find(a(2));
        if (it == open_files_.end() || it->second == nullptr || path.empty() || mode.empty() || VirtualFileSystem::contains_traversal(path)) return 0;

        bool readonly = false;
        auto host_path = resolve_guest_path(path, &readonly);
        if (host_path.empty()) return 0;

        auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
        auto vpath = resolve_guest_virtual_path(path);
        if (active_vfs && !vpath.empty()) {
            VfsStat vs{};
            if (active_vfs->stat(vpath, vs) == VfsError::ok && vs.is_readonly) {
                readonly = true;
            }
        }

        bool is_write = (mode.find('w') != std::string::npos ||
                         mode.find('a') != std::string::npos ||
                         mode.find('+') != std::string::npos);
        if (readonly && is_write) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 30); // EROFS
            return 0;
        }

        FILE* res = portable_freopen(host_path, mode.c_str(), it->second);
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
        const char* msg = std::strerror(errnum);
        if (!msg) msg = "Unknown error";
        std::size_t len = std::strlen(msg);
        std::uint64_t buf = get_static_libc_buf(256, 256);
        mem_.store(buf, msg, len + 1);
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
    if (n=="access") {
        if (!a(0)) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 14); // EFAULT
            return static_cast<std::uint64_t>(-1);
        }
        auto path = read_guest_string(a(0));
        if (path.empty() || VirtualFileSystem::contains_traversal(path)) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return static_cast<std::uint64_t>(-1);
        }
        int amode = static_cast<int>(a(1));
        if ((amode & ~7) != 0) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 22); // EINVAL
            return static_cast<std::uint64_t>(-1);
        }

        auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
        auto vpath = resolve_guest_virtual_path(path);
        bool file_exists = false;
        bool is_ro = false;

        if (active_vfs && !vpath.empty()) {
            VfsStat vstat{};
            if (active_vfs->stat(vpath, vstat) == VfsError::ok) {
                file_exists = true;
                is_ro = vstat.is_readonly;
            }
        }

        if (!file_exists) {
            bool ro = false;
            auto host_path = resolve_guest_path(path, &ro);
            if (!host_path.empty()) {
                std::error_code ec;
                if (std::filesystem::exists(host_path, ec)) {
                    file_exists = true;
                    is_ro = ro;
                }
            }
        }

        if (!file_exists) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return static_cast<std::uint64_t>(-1);
        }

        // Testar permissões solicitadas: R_OK = 4, W_OK = 2, X_OK = 1, F_OK = 0
        if ((amode & 2) != 0 && is_ro) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 13); // EACCES
            return static_cast<std::uint64_t>(-1);
        }

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
    if (n=="sceKernelUnlink" || n=="unlink") {
        if (!a(0)) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 14); // EFAULT
            return (n == "unlink") ? static_cast<std::uint64_t>(-1) : 0x80020016;
        }
        std::string path = read_guest_string(a(0));
        if (path.empty() || VirtualFileSystem::contains_traversal(path)) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return (n == "unlink") ? static_cast<std::uint64_t>(-1) : 0x80020002;
        }
        bool readonly = false;
        auto host_path = resolve_guest_path(path, &readonly);
        auto* active_vfs = vfs_ ? vfs_ : owned_vfs_.get();
        auto vpath = resolve_guest_virtual_path(path);
        if (active_vfs && !vpath.empty()) {
            VfsStat vs{};
            if (active_vfs->stat(vpath, vs) == VfsError::ok && vs.is_readonly) {
                readonly = true;
            }
        }
        if (readonly) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 30); // EROFS
            return (n == "unlink") ? static_cast<std::uint64_t>(-1) : 0x8002001e; // EROFS
        }
        if (host_path.empty()) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return (n == "unlink") ? static_cast<std::uint64_t>(-1) : 0x80020002;
        }
        std::error_code ec;
        std::filesystem::remove(host_path, ec);
        if (ec) {
            auto err_addr = get_errno_addr(1);
            if (err_addr) put<std::int32_t>(mem_, err_addr, 2); // ENOENT
            return (n == "unlink") ? static_cast<std::uint64_t>(-1) : 0x80020002;
        }
        return 0;
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
