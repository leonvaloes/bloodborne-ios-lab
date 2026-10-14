#include "../core/orbis_runtime.h"
#include <stdexcept>
#define assert(condition) do { if (!(condition)) throw std::runtime_error("falha: " #condition); } while (false)
#include <cstdint>
#include <stdexcept>
#include <string>

using namespace bb;
int main() {
    assert(orbis_nid("_init_env")=="bzQExy189ZI");
    assert(orbis_nid("sceKernelMapDirectMemory")=="L-Q3LEjIbgA");
    GuestMemory m(256*1024);
    assert(m.map(0x1000,0x4000,read|write)==MemoryError::ok);
    // call rax via mov rax, imm64; call rax; hlt — sem execução nativa.
    const std::uint8_t program[]={0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0,0xf4};
    assert(m.store(0x1000,program,sizeof program)==MemoryError::ok);
    OrbisRuntime rt(m);
    assert(rt.resolve("bzQExy189ZI#q#q")=="_init_env");
    X86Interpreter cpu(m);
    cpu.add_trap_range(0x3000,32);
    auto call=[&](const std::string& name, std::uint64_t x=0, std::uint64_t y=0, std::uint64_t z=0,
                  std::uint64_t w=0, std::uint64_t v=0, std::uint64_t sixth=0) {
        std::uint64_t trap=0x3000;
        assert(m.store(0x1002,&trap,8)==MemoryError::ok);
        cpu.state={}; cpu.state.rip=0x1000; cpu.state.gpr[4]=0x2ff0;
        cpu.state.gpr[7]=x; cpu.state.gpr[6]=y; cpu.state.gpr[2]=z;
        cpu.state.gpr[1]=w; cpu.state.gpr[8]=v; cpu.state.gpr[9]=sixth;
        auto r=cpu.run(10); assert(r.stop==CpuStop::trap && r.rip==0x3000);
        auto result=rt.invoke(name,cpu.state);
        std::uint64_t back=0; assert(m.load(cpu.state.gpr[4],&back,8)==MemoryError::ok);
        cpu.state.gpr[4]+=8; cpu.state.rip=back; cpu.state.gpr[0]=result;
        r=cpu.run(10); assert(r.stop==CpuStop::halt);
        return result;
    };
    assert(call("sceKernelGetDirectMemorySize")==5056ULL*1024*1024);
    assert(call("sceKernelAllocateDirectMemory",0,0x100000,0x4000,0x4000,0,0x2000)==0);
    std::uint64_t phys=~0ULL; assert(m.load(0x2000,&phys,8)==MemoryError::ok && phys==0);
    assert(call("sceKernelMapDirectMemory",0x2008,0x4000,3,0,phys,0)==0);
    std::uint64_t mapped=0; assert(m.load(0x2008,&mapped,8)==MemoryError::ok);
    assert(m.store(mapped,"abc",4)==MemoryError::ok);
    assert(call("strlen",mapped)==3);
    assert(call("strcmp",mapped,mapped)==0);
    assert(call("strchr",mapped,'b')==mapped+1);
    assert(call("strstr",mapped,mapped+1)==mapped+1);
    assert(call("strcpy",0x2100,mapped)==0x2100);
    assert(call("strncmp",mapped,0x2100,3)==0);
    assert(call("memset",mapped,0x58,3)==mapped);
    assert(call("memcmp",mapped,mapped,3)==0);
    assert(call("sceKernelReleaseDirectMemory",phys,0x4000)==0x80020010);
    assert(call("sceKernelMunmap",mapped,0x4000)==0);
    assert(call("sceKernelReleaseDirectMemory",phys,0x4000)==0);
    assert(call("sceKernelAllocateDirectMemory",0,0x100000,3,0,0,0x2000)==0x80020016);
    assert(call("sceKernelAllocateDirectMemory",0,0x100000,0x4000,0,0,0)==0x80020016);
    assert(call("scePthreadMutexattrInit",0x2000)==0);
    assert(call("scePthreadMutexattrSettype",0x2000,2)==0);
    assert(call("scePthreadMutexInit",0x2008,0x2000)==0);
    assert(call("scePthreadMutexLock",0x2008)==0);
    assert(call("scePthreadMutexUnlock",0x2008)==0);
    assert(call("scePthreadMutexDestroy",0x2008)==0);
    assert(call("scePthreadMutexattrDestroy",0x2000)==0);
    assert(call("__cxa_guard_acquire",0x2010)==1);
    assert(call("__cxa_guard_release",0x2010)==0);
    assert(call("__cxa_guard_acquire",0x2010)==0);
    auto ptr=call("malloc",15); assert(ptr);
    assert(m.store(ptr,"hello",6)==MemoryError::ok);
    auto grown=call("realloc",ptr,8192); assert(grown && grown!=ptr);
    char copied[6]{}; assert(m.load(grown,copied,6)==MemoryError::ok && std::string(copied)=="hello");
    assert(call("realloc",grown,0)==0);
    assert(call("calloc",UINT64_MAX,2)==0);
    auto zero=call("calloc",4,8); assert(zero);
    std::uint64_t value=1; assert(m.load(zero,&value,8)==MemoryError::ok && !value);
    assert(call("free",zero)==0 && call("free",0)==0);
    assert(call("scePthreadCondInit",0x2030,0)==0);
    assert(call("scePthreadCondWait",0x2030,0x2008)==0x80020010);
    assert(call("scePthreadCondSignal",0x2030)==0);
    assert(call("scePthreadCondDestroy",0x2030)==0);
    assert(call("scePthreadKeyCreate",0x2038,0)==0);
    std::uint32_t key=0; assert(m.load(0x2038,&key,4)==MemoryError::ok);
    assert(call("scePthreadSetspecific",key,123)==0);
    assert(call("scePthreadGetspecific",key)==123);
    assert(call("scePthreadKeyDelete",key)==0);
    assert(call("scePthreadOnce",0x2040,0x1234)==0x80020010);
    assert(call("sceKernelCreateEventFlag",0x2048,0,0,0)==0);
    std::uint32_t event=0; assert(m.load(0x2048,&event,4)==MemoryError::ok);
    assert(call("sceKernelPollEventFlag",event,1,0,0x2050)==0x80020010);
    assert(call("sceKernelSetEventFlag",event,1)==0);
    assert(call("sceKernelPollEventFlag",event,1,0,0x2050)==0);
    assert(call("sceKernelDeleteEventFlag",event)==0);
    assert(call("sceKernelClockGettime",1,0x2060)==0);
    assert(call("sceKernelClockGettime",1,0)==0x80020016);
    assert(call("sceSysmoduleLoadModule",1)==0x80020002);
    assert(call("sceAppContentInitialize",0)==0x80020002);
    assert(call("atexit",0)==0x80020016);
    assert(call("atexit",0x1234)==0);
    bool failed=false;
    try { call("sceVideoOutOpen"); } catch(const std::runtime_error& e) {
        failed=std::string(e.what()).find("import não implementado")!=std::string::npos;
    }
    assert(failed);
}
