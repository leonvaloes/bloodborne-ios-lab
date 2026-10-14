#include "orbis_runtime.h"
#include "kernel_threads.h"
#include <algorithm>
#include <array>
#include <chrono>
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
OrbisRuntime::OrbisRuntime(GuestMemory& memory) : mem_(memory) {
    static const char* const symbols[] = {
#include "orbis_import_names.h"
        "scePthreadMutexattrInit", "scePthreadMutexattrSettype", "scePthreadMutexattrDestroy",
        "scePthreadMutexInit", "scePthreadMutexLock", "scePthreadMutexUnlock", "scePthreadMutexDestroy",
        "scePthreadMutexTrylock", "sceKernelMmap", "sceKernelMunmap", "sceKernelVirtualQuery"
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
    return handle(resolve(import),cpu);
}
std::uint64_t OrbisRuntime::handle(const std::string& n, const CpuState& c) {
    auto a=[&](unsigned i){return arg(c,i);};
    if (n=="_init_env") return 0;
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
