#include "../core/orbis_runtime.h"
#include "../core/vfs.h"
#include "../core/binder.h"
#include "../core/gnm_context.h"
#include "../core/gnm_metal_bridge.h"
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#define assert(condition) do { if (!(condition)) throw std::runtime_error("falha: " #condition); } while (false)

namespace {
std::string unique_tag() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(now) + "_" + std::to_string(++counter);
}
} // namespace

using namespace bb;
int main() {
    try {
    assert(orbis_nid("_init_env")=="bzQExy189ZI");
    assert(orbis_nid("sceKernelMapDirectMemory")=="L-Q3LEjIbgA");
    GuestMemory m(256*1024);
    assert(m.map(0x1000,0x4000,bb::read|bb::write)==MemoryError::ok);
    // call rax via mov rax, imm64; call rax; hlt — sem execução nativa.
    const std::uint8_t program[]={0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0,0xf4};
    assert(m.store(0x1000,program,sizeof program)==MemoryError::ok);
    OrbisRuntime rt(m);
    assert(rt.resolve("bzQExy189ZI#q#q")=="_init_env");
    X86Interpreter cpu(m);
    cpu.add_trap_range(0x3000,32);
    auto call=[&](const std::string& name, std::uint64_t x=0, std::uint64_t y=0, std::uint64_t z=0,
                  std::uint64_t w=0, std::uint64_t v=0, std::uint64_t sixth=0,
                  std::uint64_t seventh=0, std::uint64_t eighth=0) {
        std::uint64_t trap=0x3000;
        assert(m.store(0x1002,&trap,8)==MemoryError::ok);
        cpu.state={}; cpu.state.rip=0x1000; cpu.state.gpr[4]=0x2ff0;
        assert(m.store(0x2ff0, &seventh, 8) == MemoryError::ok);
        assert(m.store(0x2ff8, &eighth, 8) == MemoryError::ok);
        cpu.state.gpr[7]=x; cpu.state.gpr[6]=y; cpu.state.gpr[2]=z;
        cpu.state.gpr[1]=w; cpu.state.gpr[8]=v; cpu.state.gpr[9]=sixth;
        auto r=cpu.run(10); assert(r.stop==CpuStop::trap && r.rip==0x3000);
        auto result=rt.invoke(name,cpu.state);
        std::uint64_t back=0; assert(m.load(cpu.state.gpr[4],&back,8)==MemoryError::ok);
        cpu.state.gpr[4]+=8; cpu.state.rip=back; cpu.state.gpr[0]=result;
        r=cpu.run(10); assert(r.stop==CpuStop::halt);
        return result;
    };
    assert(call("sceKernelGetProcParam")==0);
    rt.set_procparam(0x12345000);
    assert(call("sceKernelGetProcParam")==0x12345000);
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

    // Validação de sceKernelMprotect (T066 / FR-002)
    assert(m.store(mapped, "test", 4) == MemoryError::ok);
    assert(call("sceKernelMprotect", mapped, 0x4000, 1) == 0);
    char mprot_buf[4]{};
    assert(m.load(mapped, mprot_buf, 4) == MemoryError::ok && std::memcmp(mprot_buf, "test", 4) == 0);
    assert(m.store(mapped, "fail", 4) == MemoryError::denied);
    assert(call("sceKernelMprotect", mapped, 0x4000, 3) == 0);
    assert(m.store(mapped, "pass", 4) == MemoryError::ok);
    assert(call("sceKernelMprotect", 0, 0x4000, 1) == 0x80020016);
    assert(call("sceKernelMprotect", mapped, 0, 1) == 0x80020016);
    assert(call("sceKernelMprotect", mapped, 0x4000, 0x10) == 0x80020016);
    assert(call("sceKernelMprotect", 0x99900000, 0x4000, 1) == 0x8002000e);

    // T083: consultas não devem inventar mapeamentos em endereços livres.
    constexpr std::uint64_t unmapped = 0x99900000;
    constexpr std::uint64_t sentinel = 0xdecafbadULL;
    assert(m.store(0x2100, &sentinel, 8) == MemoryError::ok);
    assert(m.store(0x2108, &sentinel, 8) == MemoryError::ok);
    assert(m.store(0x2110, &sentinel, 8) == MemoryError::ok);
    assert(call("sceKernelQueryMemoryProtection", unmapped, 0x2100, 0x2108, 0x2110) == 0x8002000e);
    std::uint64_t unchanged = 0;
    assert(m.load(0x2100, &unchanged, 8) == MemoryError::ok && unchanged == sentinel);
    assert(m.load(0x2108, &unchanged, 8) == MemoryError::ok && unchanged == sentinel);
    assert(m.load(0x2110, &unchanged, 8) == MemoryError::ok && unchanged == sentinel);
    std::uint8_t info_sentinel[72];
    std::memset(info_sentinel, 0xa5, sizeof info_sentinel);
    assert(m.store(0x2200, info_sentinel, sizeof info_sentinel) == MemoryError::ok);
    assert(call("sceKernelVirtualQuery", unmapped, 0, 0x2200, sizeof info_sentinel) == 0x8002000e);
    std::uint8_t info_after[72]{};
    assert(m.load(0x2200, info_after, sizeof info_after) == MemoryError::ok);
    assert(std::memcmp(info_after, info_sentinel, sizeof info_after) == 0);
    assert(call("sceKernelQueryMemoryProtection", mapped, 0x2100, 0x2108, 0x2110) == 0);
    std::uint64_t queried_base = 0, queried_end = 0;
    std::uint32_t queried_prot = 0;
    assert(m.load(0x2100, &queried_base, 8) == MemoryError::ok && queried_base == mapped);
    assert(m.load(0x2108, &queried_end, 8) == MemoryError::ok && queried_end == mapped + 0x4000);
    assert(m.load(0x2110, &queried_prot, 4) == MemoryError::ok && queried_prot == 0x33);
    assert(call("sceKernelVirtualQuery", mapped, 0, 0x2200, sizeof info_sentinel) == 0);
    assert(m.load(0x2200, &queried_base, 8) == MemoryError::ok && queried_base == mapped);
    assert(m.load(0x2208, &queried_end, 8) == MemoryError::ok && queried_end == mapped + 0x4000);

    // T082: mapeamentos e protect em lote respeitam as permissões CPU solicitadas.
    assert(call("sceKernelMapDirectMemory",0x2018,0x4000,0x11,0,phys,0)==0);
    std::uint64_t readonly = 0;
    assert(m.load(0x2018, &readonly, 8) == MemoryError::ok);
    assert(m.store(readonly, "x", 1) == MemoryError::denied);
    char byte = 0;
    assert(m.load(readonly, &byte, 1) == MemoryError::ok);
    assert(call("sceKernelMunmap", readonly, 0x4000) == 0);
    assert(call("sceKernelMmap", 0x2018, 0x4000, 0x22, 0) == 0);
    std::uint64_t writeonly = 0;
    assert(m.load(0x2018, &writeonly, 8) == MemoryError::ok);
    assert(m.store(writeonly, "x", 1) == MemoryError::ok);
    assert(m.load(writeonly, &byte, 1) == MemoryError::denied);
    // Entrada de 32 bytes: start, offset, length, prot, type, reserved, operation.
    std::uint8_t batch[32]{};
    std::memcpy(batch, &writeonly, 8);
    const std::uint64_t batch_length = 0x4000;
    std::memcpy(batch + 16, &batch_length, 8);
    batch[24] = 0x11;
    const std::int32_t protect_op = 4;
    std::memcpy(batch + 28, &protect_op, 4);
    assert(m.store(0x2300, batch, sizeof batch) == MemoryError::ok);
    assert(call("sceKernelBatchMap", 0x2300, 1, 0x2340) == 0);
    std::int32_t processed = 0;
    assert(m.load(0x2340, &processed, 4) == MemoryError::ok && processed == 1);
    assert(m.load(writeonly, &byte, 1) == MemoryError::ok && byte == 'x');
    assert(m.store(writeonly, "y", 1) == MemoryError::denied);
    assert(call("sceKernelMunmap", writeonly, 0x4000) == 0);
    // O lote também deve criar uma região somente leitura e rejeitar proteção inválida.
    std::memset(batch, 0, sizeof batch);
    std::memcpy(batch + 16, &batch_length, 8);
    batch[24] = 0x11;
    const std::int32_t map_op = 3;
    std::memcpy(batch + 28, &map_op, 4);
    assert(m.store(0x2300, batch, sizeof batch) == MemoryError::ok);
    assert(call("sceKernelBatchMap", 0x2300, 1, 0x2340) == 0);
    std::uint64_t batch_mapped = 0;
    assert(m.load(0x2300, &batch_mapped, 8) == MemoryError::ok && batch_mapped != 0);
    assert(m.store(batch_mapped, "x", 1) == MemoryError::denied);
    batch[24] = 0x80;
    std::memcpy(batch, &batch_mapped, 8);
    std::memcpy(batch + 28, &protect_op, 4);
    assert(m.store(0x2300, batch, sizeof batch) == MemoryError::ok);
    assert(call("sceKernelBatchMap", 0x2300, 1, 0x2340) != 0);
    assert(m.load(0x2340, &processed, 4) == MemoryError::ok && processed == 0);
    assert(m.store(batch_mapped, "x", 1) == MemoryError::denied);
    assert(call("sceKernelMunmap", batch_mapped, 0x4000) == 0);
    std::memset(batch, 0, sizeof batch);
    std::memcpy(batch + 16, &batch_length, 8);
    batch[24] = 0x22;
    std::memcpy(batch + 28, &map_op, 4);
    assert(m.store(0x2300, batch, sizeof batch) == MemoryError::ok);
    assert(call("sceKernelBatchMap", 0x2300, 1, 0x2340) == 0);
    assert(m.load(0x2300, &batch_mapped, 8) == MemoryError::ok && batch_mapped != 0);
    assert(m.store(batch_mapped, "z", 1) == MemoryError::ok);
    assert(m.load(batch_mapped, &byte, 1) == MemoryError::denied);
    assert(call("sceKernelMunmap", batch_mapped, 0x4000) == 0);
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

    // =========================================================================
    // Testes de Sincronização Expandidos: Semaphores, Mutex, Cond, Event, TLS
    // =========================================================================
    // 1. Semaphores - SDK Convention e Lifecycle
    assert(m.store(0x2100, "TestSema", 9) == MemoryError::ok);
    auto sid = call("sceKernelCreateSema", 0x2100, 0, 1, 5, 0);
    assert(sid > 0);
    assert(call("sceKernelGetSemaInfo", sid, 0x2150) == 0);
    SceKernelSemaInfo sema_info{};
    assert(m.load(0x2150, &sema_info, sizeof(sema_info)) == MemoryError::ok);
    assert(std::string(sema_info.name) == "TestSema");
    assert(sema_info.init_count == 1);
    assert(sema_info.max_count == 5);
    assert(sema_info.current_count == 1);

    assert(call("sceKernelWaitSema", sid, 1, 0) == 0);
    assert(call("sceKernelGetSemaInfo", sid, 0x2150) == 0);
    assert(m.load(0x2150, &sema_info, sizeof(sema_info)) == MemoryError::ok);
    assert(sema_info.current_count == 0);

    assert(call("sceKernelPollSema", sid, 1) == 0x80020010); // EBUSY
    assert(call("sceKernelSignalSema", sid, 2) == 0);
    assert(call("sceKernelGetSemaInfo", sid, 0x2150) == 0);
    assert(m.load(0x2150, &sema_info, sizeof(sema_info)) == MemoryError::ok);
    assert(sema_info.current_count == 2);

    assert(call("sceKernelDeleteSema", sid) == 0);
    assert(call("sceKernelGetSemaInfo", sid, 0x2150) == 0x80020003); // ESRCH

    // 2. Semaphores - Legacy Out-pointer Convention
    assert(call("sceKernelCreateSema", 0x2180, 0, 0x00050002, 0) == 0);
    std::uint32_t leg_sid = 0;
    assert(m.load(0x2180, &leg_sid, 4) == MemoryError::ok && leg_sid > 0);
    assert(call("sceKernelDeleteSema", leg_sid) == 0);

    // 3. Pthread Mutex - Attributes, Protocol & Prioceiling Stubs
    assert(call("scePthreadMutexattrInit", 0x2200) == 0);
    assert(call("scePthreadMutexattrSettype", 0x2200, 2) == 0); // recursive
    assert(call("scePthreadMutexattrGettype", 0x2200, 0x2210) == 0);
    std::int32_t mtype = 0;
    assert(m.load(0x2210, &mtype, 4) == MemoryError::ok && mtype == 2);
    assert(call("scePthreadMutexattrGetprotocol", 0x2200, 0x2210) == 0);
    assert(call("scePthreadMutexattrSetprotocol", 0x2200, 0) == 0);
    assert(call("scePthreadMutexattrGetprioceiling", 0x2200, 0x2210) == 0);
    assert(call("scePthreadMutexattrSetprioceiling", 0x2200, 0) == 0);

    // 4. Pthread Mutex - Recursive Mutex Lock/Trylock
    assert(call("scePthreadMutexInit", 0x2220, 0x2200) == 0);
    assert(call("scePthreadMutexGetprioceiling", 0x2220, 0x2210) == 0);
    assert(call("scePthreadMutexSetprioceiling", 0x2220, 0, 0x2210) == 0);
    assert(call("scePthreadMutexLock", 0x2220) == 0);
    assert(call("scePthreadMutexTrylock", 0x2220) == 0); // recursive allowed
    assert(call("scePthreadMutexUnlock", 0x2220) == 0);
    assert(call("scePthreadMutexUnlock", 0x2220) == 0);
    assert(call("scePthreadMutexDestroy", 0x2220) == 0);
    assert(call("scePthreadMutexattrDestroy", 0x2200) == 0);

    // 5. Pthread Mutex - Normal Mutex Trylock returns EBUSY
    assert(call("scePthreadMutexInit", 0x2230, 0) == 0);
    assert(call("scePthreadMutexLock", 0x2230) == 0);
    assert(call("scePthreadMutexTrylock", 0x2230) == 0x80020010); // EBUSY
    assert(call("scePthreadMutexUnlock", 0x2230) == 0);
    assert(call("scePthreadMutexDestroy", 0x2230) == 0);

    // 6. Event Flag - SDK Convention
    assert(m.store(0x2250, "EventSDK", 9) == MemoryError::ok);
    auto efid = call("sceKernelCreateEventFlag", 0x2250, 0, 7, 0);
    assert(efid > 0);
    assert(call("sceKernelPollEventFlag", efid, 1, 0, 0x2260) == 0);
    assert(call("sceKernelClearEventFlag", efid, 7) == 0);
    assert(call("sceKernelPollEventFlag", efid, 1, 0, 0x2260) == 0x80020010);
    assert(call("sceKernelDeleteEventFlag", efid) == 0);

    // 7. Pthread Cond - Timedwait abstime
    assert(call("scePthreadCondInit", 0x2270, 0) == 0);
    assert(call("scePthreadMutexInit", 0x2280, 0) == 0);
    assert(call("scePthreadCondTimedwait", 0x2270, 0x2280, 100) == 0x80020010);
    assert(call("scePthreadCondDestroy", 0x2270) == 0);
    assert(call("scePthreadMutexDestroy", 0x2280) == 0);

    // 8. Pthread Once & TLS aliases
    assert(call("pthread_once", 0x2040, 0x1234) == 0x80020010);
    assert(call("pthread_key_create", 0x2290, 0) == 0);
    std::uint32_t pkey = 0;
    assert(m.load(0x2290, &pkey, 4) == MemoryError::ok);
    assert(call("pthread_setspecific", pkey, 456) == 0);
    assert(call("pthread_getspecific", pkey) == 456);
    assert(call("pthread_key_delete", pkey) == 0);
    // Validação de chave inválida (T068 / FR-002)
    assert(call("pthread_key_delete", 0x99999) == 0x80020016);
    assert(call("pthread_setspecific", 0x99999, 123) == 0x80020016);
    assert(call("pthread_getspecific", 0x99999) == 0);
    assert(call("scePthreadKeyDelete", 0x99999) == 0x80020016);
    assert(call("scePthreadSetspecific", 0x99999, 123) == 0x80020016);
    assert(call("scePthreadGetspecific", 0x99999) == 0);
    assert(call("sceKernelClockGettime",1,0x2060)==0);
    assert(call("sceKernelClockGettime",1,0)==0x80020016);
    // T126: Validação de falha de escrita guest e propagação de erro para sceKernelClockGettime, sceSystemServiceParamGetInt e sceSystemServiceGetStatus
    const std::uint64_t unmapped_addr = 0xdeadbeef000ULL;
    assert(call("sceKernelClockGettime", 1, unmapped_addr) == 0x8002000e);
    assert(call("sceSystemServiceParamGetInt", 1, 0x2060) == 0);
    std::int32_t sys_lang = 0;
    assert(m.load(0x2060, &sys_lang, sizeof(sys_lang)) == MemoryError::ok && sys_lang == 1);
    assert(call("sceSystemServiceParamGetInt", 1, 0) == 0x80A10003);
    assert(call("sceSystemServiceParamGetInt", 1, unmapped_addr) == 0x80A10003);
    assert(call("sceSystemServiceGetStatus", 0x2060) == 0);
    assert(call("sceSystemServiceGetStatus", 0) == 0x80A10003);
    assert(call("sceSystemServiceGetStatus", unmapped_addr) == 0x80A10003);
    const std::uint64_t ro_addr = 0x30000ULL;
    assert(m.map(ro_addr, 0x4000, bb::read) == MemoryError::ok);
    assert(call("sceKernelClockGettime", 1, ro_addr) == 0x8002000e);
    assert(call("sceSystemServiceParamGetInt", 1, ro_addr) == 0x80A10003);
    assert(call("sceSystemServiceGetStatus", ro_addr) == 0x80A10003);
    assert(m.unmap(ro_addr, 0x4000) == MemoryError::ok);
    // T123: Integração de SysmoduleManager ao despacho de sceSysmoduleLoadModule, sceSysmoduleUnloadModule e sceSysmoduleIsLoaded
    assert(call("sceSysmoduleIsLoaded", 1) == 0x805A1001); // SCE_SYSMODULE_ERROR_UNLOADED
    assert(call("sceSysmoduleLoadModule", 0) == 0x805A1000); // SCE_SYSMODULE_ERROR_INVALID_VALUE
    assert(call("sceSysmoduleLoadModule", 0x9999) == 0x805A1000); // SCE_SYSMODULE_ERROR_INVALID_VALUE
    assert(call("sceSysmoduleLoadModule", 1) == 0); // SCE_SYSMODULE_OK
    assert(call("sceSysmoduleIsLoaded", 1) == 0); // SCE_SYSMODULE_LOADED
    assert(call("sceSysmoduleLoadModule", 1) == 0x805A1002); // SCE_SYSMODULE_ERROR_ALREADY_LOADED
    assert(call("sceSysmoduleUnloadModule", 1) == 0); // SCE_SYSMODULE_OK
    assert(call("sceSysmoduleIsLoaded", 1) == 0x805A1001); // SCE_SYSMODULE_ERROR_UNLOADED
    assert(call("sceSysmoduleUnloadModule", 1) == 0x805A1001); // SCE_SYSMODULE_ERROR_UNLOADED
    assert(call("sceSysmoduleUnloadModule", 0) == 0x805A1000); // SCE_SYSMODULE_ERROR_INVALID_VALUE

    // T124: Integração de AppContentManager ao despacho de inicialização e consulta de parâmetros/conteúdo
    assert(call("sceAppContentAppParamGetInt", 0, 0x2060) == 0x80D90003); // SCE_APP_CONTENT_ERROR_NOT_INITIALIZED
    assert(call("sceAppContentInitialize", 0, 0) == 0); // SCE_APP_CONTENT_OK
    assert(call("sceAppContentInitialize", 0, 0) == 0x80D90001); // SCE_APP_CONTENT_ERROR_BUSY
    assert(call("sceAppContentAppParamGetInt", 0, 0x2060) == 0);
    std::int32_t sku_flag = 0;
    assert(m.load(0x2060, &sku_flag, sizeof(sku_flag)) == MemoryError::ok && sku_flag == 3);
    assert(call("sceAppContentAppParamGetInt", 0, 0) == 0x80D90002); // SCE_APP_CONTENT_ERROR_PARAMETER
    assert(call("sceAppContentAppParamGetInt", 0, unmapped_addr) == 0x80D90002); // SCE_APP_CONTENT_ERROR_PARAMETER
    assert(call("sceAppContentAppParamGetInt", 9999, 0x2060) == 0x80D90002); // SCE_APP_CONTENT_ERROR_PARAMETER
    assert(call("sceAppContentGetAddcontInfoList", 0, 0, 0, 0x2060) == 0);
    std::uint32_t dlc_hits = 0;
    assert(m.load(0x2060, &dlc_hits, sizeof(dlc_hits)) == MemoryError::ok && dlc_hits == 2);
    assert(call("sceAppContentGetAddcontInfoList", 0, 0, 0, unmapped_addr) == 0x80D90002);
    assert(call("atexit",0)==0x80020016);
    assert(call("atexit",0x1234)==0);
    bool failed=false;
    try { call("unknown_dummy_symbol"); } catch(const std::runtime_error& e) {
        failed=std::string(e.what()).find("import não implementado")!=std::string::npos;
    }
    assert(failed);

    // =========================================================================
    // Testes de subsistemas HLE: libSceUserService, libScePad, libSceSaveData,
    // libSceAudioOut, Math, Conversões, Ordenação e snprintf
    // =========================================================================
    assert(m.map(0x5000, 0xb000, bb::read | bb::write) == MemoryError::ok);

    // 1. libSceUserService & libScePad
    assert(call("sceUserServiceInitialize", 0) == 0);
    assert(call("sceUserServiceGetInitialUser", 0x5000) == 0);
    SceUserServiceUserId uid = -1;
    assert(m.load(0x5000, &uid, sizeof(uid)) == MemoryError::ok && uid == SCE_USER_SERVICE_INITIAL_USER_ID);

    assert(call("sceUserServiceGetUserName", uid, 0x5020, 16) == 0);
    char uname[16]{};
    assert(m.load(0x5020, uname, sizeof(uname)) == MemoryError::ok && std::string(uname) == "Hunter");

    auto pad_h = call("scePadOpen", uid, 0, 0, 0);
    assert(static_cast<std::int32_t>(pad_h) > 0);

    assert(call("scePadReadState", pad_h, 0x5100) == 0);
    ScePadData pdata{};
    assert(m.load(0x5100, &pdata, sizeof(pdata)) == MemoryError::ok);
    assert(pdata.connected == 1);
    assert(pdata.left_stick_x == 128 && pdata.left_stick_y == 128);
    assert(pdata.right_stick_x == 128 && pdata.right_stick_y == 128);

    ScePadVibrationParam vib{80, 120};
    assert(m.store(0x5200, &vib, sizeof(vib)) == MemoryError::ok);
    assert(call("scePadSetVibration", pad_h, 0x5200) == 0);
    assert(rt.pad_manager().get_vibration(static_cast<std::int32_t>(pad_h)).large_motor == 80);

    ScePadColor col{0, 255, 0, 0};
    assert(m.store(0x5220, &col, sizeof(col)) == MemoryError::ok);
    assert(call("scePadSetLightBar", pad_h, 0x5220) == 0);
    assert(rt.pad_manager().get_lightbar(static_cast<std::int32_t>(pad_h)).g == 255);
    assert(call("scePadClose", pad_h) == 0);

    // 2. libSceSaveData & libSceAudioOut
    // Set up VFS and initialize SaveDataManager with it before HLE calls
    VirtualFileSystem vfs;
    const std::string tag = unique_tag();
    std::string temp_dir = std::filesystem::temp_directory_path().string() + "/save_test_" + tag;
    std::string save_base_dir = std::filesystem::temp_directory_path().string() + "/save_data_test_" + tag;
    std::error_code ec_clean;
    std::filesystem::remove_all(temp_dir, ec_clean);
    std::filesystem::remove_all(save_base_dir, ec_clean);

    assert(vfs.mount("/save", temp_dir));
    rt.save_data_manager().initialize(save_base_dir, 32 * 1024 * 1024, &vfs);

    SceSaveDataInitParams save_params{};
    save_params.max_quota_per_slot = 32768;
    assert(m.store(0x5300, &save_params, sizeof(save_params)) == MemoryError::ok);
    assert(call("sceSaveDataInitialize3", 0x5300) == 0);
    assert(rt.save_data_manager().is_initialized());
    assert(rt.save_data_manager().get_max_quota_per_slot() == 32768);

    SceSaveDataDirName sdir{};
    std::strncpy(sdir.data, "SAVE_TEST", sizeof(sdir.data));
    assert(m.store(0x5400, &sdir, sizeof(sdir)) == MemoryError::ok);
    SceSaveDataMount smount{};
    smount.user = 0;
    smount.dir = reinterpret_cast<const SceSaveDataDirName*>(0x5400);
    smount.mode = SCE_SAVEDATA_MOUNT_MODE_CREATE;
    smount.blocks = 64;
    assert(m.store(0x5440, &smount, sizeof(smount)) == MemoryError::ok);
    // T109: campos de largura fixa do guest precisam terminar dentro do próprio array.
    SceSaveDataTitleId bad_title{};
    std::memset(bad_title.data, 'T', sizeof(bad_title.data));
    SceSaveDataDirName bad_dir{};
    std::memset(bad_dir.data, 'D', sizeof(bad_dir.data));
    SceSaveDataMountPoint bad_point{};
    std::memset(bad_point.data, 'P', sizeof(bad_point.data));
    assert(m.store(0x5800, &bad_title, sizeof(bad_title)) == MemoryError::ok);
    assert(m.store(0x5900, &bad_dir, sizeof(bad_dir)) == MemoryError::ok);
    assert(m.store(0x5A00, &bad_point, sizeof(bad_point)) == MemoryError::ok);
    smount.title = reinterpret_cast<const SceSaveDataTitleId*>(0x5800);
    assert(m.store(0x5440, &smount, sizeof(smount)) == MemoryError::ok);
    assert(static_cast<std::int32_t>(call("sceSaveDataMount", 0x5440, 0x5500)) == SCE_SAVEDATA_ERROR_PARAMETER);
    smount.title = nullptr;
    smount.dir = reinterpret_cast<const SceSaveDataDirName*>(0x5900);
    assert(m.store(0x5440, &smount, sizeof(smount)) == MemoryError::ok);
    assert(static_cast<std::int32_t>(call("sceSaveDataMount", 0x5440, 0x5500)) == SCE_SAVEDATA_ERROR_PARAMETER);
    smount.dir = reinterpret_cast<const SceSaveDataDirName*>(0x5400);
    assert(m.store(0x5440, &smount, sizeof(smount)) == MemoryError::ok);
    SceSaveDataDelete bad_delete{};
    bad_delete.dir = reinterpret_cast<const SceSaveDataDirName*>(0x5900);
    assert(m.store(0x5B00, &bad_delete, sizeof(bad_delete)) == MemoryError::ok);
    assert(static_cast<std::int32_t>(call("sceSaveDataDelete", 0x5B00)) == SCE_SAVEDATA_ERROR_PARAMETER);
    assert(static_cast<std::int32_t>(call("sceSaveDataUmount", 0x5A00, 1)) == SCE_SAVEDATA_ERROR_PARAMETER);
    assert(static_cast<std::int32_t>(call("sceSaveDataGetParam", 0x5A00, 0, 0x5700, 64)) == SCE_SAVEDATA_ERROR_PARAMETER);
    assert(!std::filesystem::exists(std::filesystem::path(save_base_dir) / "savedata" / "0"));
    // Saída inválida não pode consumir o slot nem criar o save antes do retry.
    assert(static_cast<std::int32_t>(call("sceSaveDataMount", 0x5440, 0xFFFFFFFFULL)) == SCE_SAVEDATA_ERROR_PARAMETER);
    assert(!std::filesystem::exists(std::filesystem::path(save_base_dir) / "savedata" / "0"));
    assert(call("sceSaveDataMount", 0x5440, 0x5500) == 0);
    SceSaveDataMountResult sres{};
    assert(m.load(0x5500, &sres, sizeof(sres)) == MemoryError::ok);
    assert(std::string(sres.point.data).find("/savedata") != std::string::npos);
    assert(m.store(0x5600, &sres.point, sizeof(sres.point)) == MemoryError::ok);

    // T088: sceSaveDataSetParam e sceSaveDataSaveIcon limites e validacoes
    // sceSaveDataSetParam: rejeita tamanho excessivo (>64 KiB), tamanho zero, tipo invalido, buffer invalido
    assert(static_cast<std::int32_t>(call("sceSaveDataSetParam", 0x5600, 1, 0x5700, 70000)) == SCE_SAVEDATA_ERROR_PARAMETER);
    assert(static_cast<std::int32_t>(call("sceSaveDataSetParam", 0x5600, 1, 0x5700, 0)) == SCE_SAVEDATA_ERROR_PARAMETER);
    assert(static_cast<std::int32_t>(call("sceSaveDataSetParam", 0x5600, 99, 0x5700, 32)) == SCE_SAVEDATA_ERROR_PARAMETER);
    assert(static_cast<std::int32_t>(call("sceSaveDataSetParam", 0x5600, 0, 0x5700, 10)) == SCE_SAVEDATA_ERROR_PARAMETER);
    assert(static_cast<std::int32_t>(call("sceSaveDataSetParam", 0x5600, 1, 0xFFFFFFFF0000ULL, 32)) == SCE_SAVEDATA_ERROR_PARAMETER);
    std::vector<std::uint8_t> valid_param(64, 0x5A);
    assert(m.store(0x5700, valid_param.data(), valid_param.size()) == MemoryError::ok);
    assert(call("sceSaveDataSetParam", 0x5600, 1, 0x5700, valid_param.size()) == 0);

    // sceSaveDataSaveIcon: rejeita tamanho zero, tamanho > 2 MiB, buffer invalido
    assert(static_cast<std::int32_t>(call("sceSaveDataSaveIcon", 0x5600, 0x5700, 0)) == SCE_SAVEDATA_ERROR_PARAMETER);
    assert(static_cast<std::int32_t>(call("sceSaveDataSaveIcon", 0x5600, 0x5700, 3 * 1024 * 1024)) == SCE_SAVEDATA_ERROR_PARAMETER);
    assert(static_cast<std::int32_t>(call("sceSaveDataSaveIcon", 0x5600, 0xFFFFFFFF0000ULL, 64)) == SCE_SAVEDATA_ERROR_PARAMETER);
    assert(call("sceSaveDataSaveIcon", 0x5600, 0x5700, 64) == 0);

    // Desmonta e commita o save no disco para que possa ser localizado pela busca
    assert(call("sceSaveDataUmount", 0x5600, 1) == 0);

    // T089: sceSaveDataDirNameSearch propagando falhas de escrita
    SceSaveDataSearchCond s_cond{};
    s_cond.user = 0;
    s_cond.key = SCE_SAVEDATA_SORT_KEY_DIRNAME;
    s_cond.order = SCE_SAVEDATA_SORT_ORDER_ASC;
    assert(m.store(0x5480, &s_cond, sizeof(s_cond)) == MemoryError::ok);

    SceSaveDataSearchResult s_res{};
    s_res.names_capacity = 1;
    s_res.names = reinterpret_cast<SceSaveDataDirName*>(0xFFFFFFFF0000ULL);
    assert(m.store(0x5550, &s_res, sizeof(s_res)) == MemoryError::ok);
    assert(static_cast<std::int32_t>(call("sceSaveDataDirNameSearch", 0x5480, 0x5550)) == SCE_SAVEDATA_ERROR_PARAMETER);

    s_res.names = reinterpret_cast<SceSaveDataDirName*>(0x8000);
    s_res.params = reinterpret_cast<SceSaveDataParam*>(0xFFFFFFFF0000ULL);
    assert(m.store(0x5550, &s_res, sizeof(s_res)) == MemoryError::ok);
    assert(static_cast<std::int32_t>(call("sceSaveDataDirNameSearch", 0x5480, 0x5550)) == SCE_SAVEDATA_ERROR_PARAMETER);

    s_res.params = reinterpret_cast<SceSaveDataParam*>(0x8100);
    s_res.infos = reinterpret_cast<SceSaveDataSearchInfo*>(0xFFFFFFFF0000ULL);
    assert(m.store(0x5550, &s_res, sizeof(s_res)) == MemoryError::ok);
    assert(static_cast<std::int32_t>(call("sceSaveDataDirNameSearch", 0x5480, 0x5550)) == SCE_SAVEDATA_ERROR_PARAMETER);

    assert(static_cast<std::int32_t>(call("sceSaveDataDirNameSearch", 0x5480, 0xFFFFFFFF0000ULL)) == SCE_SAVEDATA_ERROR_PARAMETER);

    s_res.infos = reinterpret_cast<SceSaveDataSearchInfo*>(0x8800);
    assert(m.store(0x5550, &s_res, sizeof(s_res)) == MemoryError::ok);
    assert(call("sceSaveDataDirNameSearch", 0x5480, 0x5550) == 0);
    SceSaveDataSearchResult ok_sres{};
    assert(m.load(0x5550, &ok_sres, sizeof(ok_sres)) == MemoryError::ok);
    assert(ok_sres.hits >= 1);
    assert(ok_sres.set_count >= 1);
    SceSaveDataDirName ok_dirname{};
    assert(m.load(0x8000, &ok_dirname, sizeof(ok_dirname)) == MemoryError::ok);
    assert(std::string(ok_dirname.data) == "SAVE_TEST");

    std::filesystem::remove_all(temp_dir, ec_clean);
    std::filesystem::remove_all(save_base_dir, ec_clean);

    auto audio_h = call("sceAudioOutOpen", uid, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 512, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    assert(static_cast<std::int32_t>(audio_h) > 0);
    std::vector<std::int16_t> pcm_data(512 * 2, 500);
    assert(m.store(0x5800, pcm_data.data(), pcm_data.size() * sizeof(std::int16_t)) == MemoryError::ok);
    assert(call("sceAudioOutOutput", audio_h, 0x5800) == 0);
    std::int32_t vol_data[2] = {16384, 16384};
    assert(m.store(0x5900, vol_data, sizeof(vol_data)) == MemoryError::ok);
    assert(call("sceAudioOutSetVolume", audio_h, SCE_AUDIO_OUT_VOLUME_FLAG_STEREO, 0x5900) == 0);
    assert(call("sceAudioOutGetPortInfo", audio_h, 0x5700) == 0);
    SceAudioOutPortInfo ainfo{};
    assert(m.load(0x5700, &ainfo, sizeof(ainfo)) == MemoryError::ok);
    assert(ainfo.status == 1 && ainfo.samples_per_block == 512 && ainfo.volume_left == 16384);
    assert(call("sceAudioOutClose", audio_h) == 0);

    // 3. Funções matemáticas (SysV / registradores XMM)
    auto call_math = [&](const std::string& name, double x, double y = 0.0) -> double {
        cpu.state = {};
        std::memcpy(&cpu.state.xmm[0][0], &x, sizeof(x));
        std::memcpy(&cpu.state.xmm[1][0], &y, sizeof(y));
        rt.invoke(name, cpu.state);
        double res = 0.0;
        std::memcpy(&res, &cpu.state.xmm[0][0], sizeof(res));
        return res;
    };
    auto call_math_f = [&](const std::string& name, float x, float y = 0.0f) -> float {
        cpu.state = {};
        std::uint32_t bx = 0, by = 0;
        std::memcpy(&bx, &x, sizeof(bx));
        std::memcpy(&by, &y, sizeof(by));
        cpu.state.xmm[0][0] = bx;
        cpu.state.xmm[1][0] = by;
        rt.invoke(name, cpu.state);
        std::uint32_t bres = static_cast<std::uint32_t>(cpu.state.xmm[0][0]);
        float res = 0.0f;
        std::memcpy(&res, &bres, sizeof(res));
        return res;
    };
    assert(std::abs(call_math("sin", 0.0) - 0.0) < 1e-6);
    assert(std::abs(call_math("sin", 1.5707963267948966) - 1.0) < 1e-6);
    assert(std::abs(call_math("cos", 0.0) - 1.0) < 1e-6);
    assert(std::abs(call_math("cos", 3.141592653589793) - (-1.0)) < 1e-6);
    assert(std::abs(call_math("tan", 0.0) - 0.0) < 1e-6);
    assert(std::abs(call_math("atan2", 1.0, 1.0) - 0.7853981633974483) < 1e-6);
    assert(std::abs(call_math("pow", 2.0, 10.0) - 1024.0) < 1e-6);
    assert(std::abs(call_math("exp", 1.0) - 2.718281828459045) < 1e-6);
    assert(std::abs(call_math("log", 2.718281828459045) - 1.0) < 1e-6);
    assert(std::abs(call_math("sqrt", 144.0) - 12.0) < 1e-6);
    assert(std::abs(call_math("floor", 5.9) - 5.0) < 1e-6);
    assert(std::abs(call_math("ceil", 5.1) - 6.0) < 1e-6);
    assert(std::abs(call_math("fmod", 7.0, 3.0) - 1.0) < 1e-6);
    assert(std::abs(call_math("round", 4.6) - 5.0) < 1e-6);
    assert(std::abs(call_math("fabs", -123.456) - 123.456) < 1e-6);

    assert(std::abs(call_math_f("sinf", 0.0f) - 0.0f) < 1e-5f);
    assert(std::abs(call_math_f("cosf", 0.0f) - 1.0f) < 1e-5f);
    assert(std::abs(call_math_f("sqrtf", 25.0f) - 5.0f) < 1e-5f);
    assert(std::abs(call_math_f("fabsf", -99.5f) - 99.5f) < 1e-5f);

    // SysV: GPRs não são fallback para parâmetros FP, nem quando XMM contém +0 ou -0.
    auto double_bits = [](double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };
    auto float_bits = [](float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };
    auto check_double_abi = [&](const char* name, double x, double expected) {
        cpu.state = {};
        cpu.state.xmm[0][0] = double_bits(x);
        cpu.state.gpr[7] = double_bits(1.0); // RDI conflitante com XMM0
        rt.invoke(name, cpu.state);
        assert(cpu.state.xmm[0][0] == double_bits(expected));
    };
    auto check_float_abi = [&](const char* name, float x, float expected) {
        cpu.state = {};
        cpu.state.xmm[0][0] = 0xDEADBEEF00000000ULL | float_bits(x);
        cpu.state.gpr[7] = float_bits(1.0f); // RDI conflitante com XMM0
        rt.invoke(name, cpu.state);
        assert(static_cast<std::uint32_t>(cpu.state.xmm[0][0]) == float_bits(expected));
    };
    for (double zero_arg : {0.0, -0.0}) {
        check_double_abi("cos", zero_arg, 1.0);
        check_double_abi("exp", zero_arg, 1.0);
        check_double_abi("sqrt", zero_arg, zero_arg); // preserva sinal de -0
    }
    for (float zero_arg : {0.0f, -0.0f}) {
        check_float_abi("cosf", zero_arg, 1.0f);
        check_float_abi("sqrtf", zero_arg, zero_arg);
    }
    // XMM1 também é independente de RSI em funções binárias.
    cpu.state = {};
    cpu.state.xmm[0][0] = double_bits(2.0);
    cpu.state.xmm[1][0] = double_bits(0.0);
    cpu.state.gpr[6] = double_bits(3.0);
    rt.invoke("pow", cpu.state);
    assert(cpu.state.xmm[0][0] == double_bits(1.0));

    // 4. Conversões & Ordenação (strtol, strtoul, strtod, strtof, qsort, bsearch)
    assert(m.store(0x5a00, " -98765xyz", 11) == MemoryError::ok);
    auto st_res = call("strtol", 0x5a00, 0x5b00, 10);
    assert(static_cast<std::int64_t>(st_res) == -98765);
    std::uint64_t end_p = 0;
    assert(m.load(0x5b00, &end_p, 8) == MemoryError::ok && end_p == 0x5a07);

    assert(m.store(0x5a20, "0xDEADBEEF trailing", 20) == MemoryError::ok);
    auto stu_res = call("strtoul", 0x5a20, 0x5b10, 16);
    assert(stu_res == 0xDEADBEEFULL);

    assert(m.store(0x5a40, "3.14159rest", 12) == MemoryError::ok);
    call("strtod", 0x5a40, 0x5b20);
    double dval = 0.0;
    std::memcpy(&dval, &cpu.state.xmm[0][0], sizeof(dval));
    assert(std::abs(dval - 3.14159) < 1e-5);

    // qsort e bsearch com comparador em código de máquina guest
    // mov eax, [rdi] (8b 07)
    // sub eax, [rsi] (2b 06)
    // ret (c3)
    const std::uint8_t cmp_code[] = {0x8B, 0x07, 0x2B, 0x06, 0xC3};
    assert(m.store(0x1500, cmp_code, sizeof(cmp_code)) == MemoryError::ok);
    int unsorted_arr[5] = {50, 10, 40, 20, 30};
    assert(m.store(0x5c00, unsorted_arr, sizeof(unsorted_arr)) == MemoryError::ok);
    assert(call("qsort", 0x5c00, 5, sizeof(int), 0x1500) == 0);
    int sorted_arr[5]{};
    assert(m.load(0x5c00, sorted_arr, sizeof(sorted_arr)) == MemoryError::ok);
    assert(sorted_arr[0] == 10 && sorted_arr[1] == 20 && sorted_arr[2] == 30 && sorted_arr[3] == 40 && sorted_arr[4] == 50);

    int bkey = 40;
    assert(m.store(0x5c40, &bkey, sizeof(bkey)) == MemoryError::ok);
    auto bfound = call("bsearch", 0x5c40, 0x5c00, 5, sizeof(int), 0x1500);
    assert(bfound == 0x5c00 + 3 * sizeof(int));

    int bkey_none = 999;
    assert(m.store(0x5c40, &bkey_none, sizeof(bkey_none)) == MemoryError::ok);
    assert(call("bsearch", 0x5c40, 0x5c00, 5, sizeof(int), 0x1500) == 0);

    // 5. snprintf
    assert(m.store(0x5e00, "val=%d u=%u hex=%x s=%s", sizeof("val=%d u=%u hex=%x s=%s")) == MemoryError::ok);
    assert(m.store(0x5e40, "Hunter", 7) == MemoryError::ok);
    auto sn_len = call("snprintf", 0x5d00, 64, 0x5e00, static_cast<std::uint64_t>(-42), 100, 0xFE, 0x5e40);
    char sn_out[64]{};
    assert(m.load(0x5d00, sn_out, sizeof(sn_out)) == MemoryError::ok);
    assert(std::string(sn_out) == "val=-42 u=100 hex=fe s=Hunter");
    assert(sn_len == std::string("val=-42 u=100 hex=fe s=Hunter").size());

    // snprintf com float e ponteiro
    assert(m.store(0x5e60, "pi=%.2f ptr=%p", 15) == MemoryError::ok);
    cpu.state = {};
    double pi_val = 3.14159;
    std::memcpy(&cpu.state.xmm[0][0], &pi_val, sizeof(pi_val));
    cpu.state.gpr[7] = 0x5d00;
    cpu.state.gpr[6] = 64;
    cpu.state.gpr[2] = 0x5e60;
    cpu.state.gpr[1] = 0x1234;
    rt.invoke("snprintf", cpu.state);
    char sn_out2[64]{};
    assert(m.load(0x5d00, sn_out2, sizeof(sn_out2)) == MemoryError::ok);
    assert(std::string(sn_out2).find("pi=3.14") != std::string::npos);
    assert(std::string(sn_out2).find("1234") != std::string::npos);

    // Limite estrito de tamanho para evitar buffer overflows
    call("snprintf", 0x5d00, 6, 0x5e00, static_cast<std::uint64_t>(-42), 100, 0xFE, 0x5e40);
    char sn_trunc[16]{};
    assert(m.load(0x5d00, sn_trunc, sizeof(sn_trunc)) == MemoryError::ok);
    assert(std::string(sn_trunc) == "val=-");
    assert(sn_trunc[5] == '\0');

    // T092: sprintf, vsprintf e vsnprintf com semantica estrita e rejeicao explicita
    struct SysVVaListTest {
        std::uint32_t gp_offset;
        std::uint32_t fp_offset;
        std::uint64_t overflow_arg_area;
        std::uint64_t reg_save_area;
    };

    // sprintf
    assert(m.store(0x5e00, "num=%d str=%s hex=0x%X", 23) == MemoryError::ok);
    assert(m.store(0x5e40, "Yharnam", 8) == MemoryError::ok);
    auto sp_len = call("sprintf", 0x5d00, 0x5e00, 12345, 0x5e40, 0xABCD);
    char sp_out[64]{};
    assert(m.load(0x5d00, sp_out, sizeof(sp_out)) == MemoryError::ok);
    assert(std::string(sp_out) == "num=12345 str=Yharnam hex=0xABCD");
    assert(sp_len == std::string("num=12345 str=Yharnam hex=0xABCD").size());

    // sprintf com formato rejeitado/invalido nao reporta sucesso falso
    assert(m.store(0x5e20, "bad=%~", 7) == MemoryError::ok);
    assert(call("sprintf", 0x5d00, 0x5e20, 1) == static_cast<std::uint64_t>(-1));
    assert(call("sprintf", 0, 0x5e00, 1) == static_cast<std::uint64_t>(-1));

    // vsprintf com SysV va_list
    std::uint64_t reg_save_vals[6] = {
        static_cast<std::uint64_t>(-77), // arg 0 (%d)
        0x5e40                           // arg 1 (%s)
    };
    constexpr std::uint64_t reg_save_addr = 0x5b40;
    constexpr std::uint64_t va_list_addr = 0x5b80;
    assert(m.store(reg_save_addr, reg_save_vals, sizeof(reg_save_vals)) == MemoryError::ok);
    SysVVaListTest vl_test{0, 48, 0, reg_save_addr};
    assert(m.store(va_list_addr, &vl_test, sizeof(vl_test)) == MemoryError::ok);
    assert(m.store(0x5e70, "v=%d s=%s", 10) == MemoryError::ok);

    auto vsp_len = call("vsprintf", 0x5d00, 0x5e70, va_list_addr);
    char vsp_out[64]{};
    assert(m.load(0x5d00, vsp_out, sizeof(vsp_out)) == MemoryError::ok);
    assert(std::string(vsp_out) == "v=-77 s=Yharnam");
    assert(vsp_len == std::string("v=-77 s=Yharnam").size());

    // vsnprintf com truncamento e terminador nulo estrito
    vl_test = {0, 48, 0, reg_save_addr};
    assert(m.store(va_list_addr, &vl_test, sizeof(vl_test)) == MemoryError::ok);
    auto vsn_len = call("vsnprintf", 0x5d00, 6, 0x5e70, va_list_addr);
    char vsn_out[16]{};
    assert(m.load(0x5d00, vsn_out, sizeof(vsn_out)) == MemoryError::ok);
    assert(std::string(vsn_out) == "v=-77");
    assert(vsn_out[5] == '\0');
    assert(vsn_len == std::string("v=-77 s=Yharnam").size());

    // 6. _sceKernelRtldSetApplicationHeapAPI
    assert(call("_sceKernelRtldSetApplicationHeapAPI", 0x9000) == 0);

    // 7. libSceVideoOut HLE
    {
        struct TestVideoSink : public VideoOutSink {
            void on_flip(const FlipEvent&) override {}
        } test_sink;
        VideoOutManager vom(16);
        vom.attach_guest_memory(&m, &test_sink);
        rt.attach_video_out(&vom);

        auto vh = call("sceVideoOutOpen", 0, 0, 0, 0);
        assert(vh > 0);
        assert(call("sceVideoOutSetFlipRate", vh, 60) == 0);
        assert(call("sceVideoOutAddFlipEvent", 0, vh, 0) == 0);

        constexpr std::uint64_t attr_addr = 0x6000;
        assert(call("sceVideoOutSetBufferAttribute", attr_addr, 0x80000000, 1, 0, 2, 2, 2) == 0);
        std::uint32_t read_attr[6] = {};
        assert(m.load(attr_addr, read_attr, sizeof(read_attr)) == MemoryError::ok);
        assert(read_attr[0] == 0x80000000 && read_attr[1] == 1);
        assert(read_attr[3] == 2 && read_attr[4] == 2 && read_attr[5] == 2);
        BufferAttribute full_attr{};
        assert(m.load(attr_addr, &full_attr, sizeof(full_attr)) == MemoryError::ok);
        assert(full_attr.pixel_format == 0x80000000 && full_attr.tiling_mode == 1);
        assert(full_attr.option == 0 && full_attr.reserved0 == 0 && full_attr.reserved1 == 0);

        // Rejeição de tiling_mode inválido (> 1) e pitch < width
        constexpr std::uint64_t bad_tile_addr = 0x6050;
        assert(call("sceVideoOutSetBufferAttribute", bad_tile_addr, 0, 2, 0, 2, 2, 2) == 0);
        constexpr std::uint64_t bad_pitch_addr = 0x6080;
        assert(call("sceVideoOutSetBufferAttribute", bad_pitch_addr, 0, 1, 0, 4, 2, 2) == 0);

        constexpr std::uint64_t addrs_ptr = 0x6100;
        constexpr std::uint64_t buf0_addr = 0x7000;
        assert(m.store(addrs_ptr, &buf0_addr, 8) == MemoryError::ok);
        assert(call("sceVideoOutRegisterBuffers", vh, 0, addrs_ptr, 1, bad_tile_addr) == UINT64_MAX);
        assert(call("sceVideoOutRegisterBuffers", vh, 0, addrs_ptr, 1, bad_pitch_addr) == UINT64_MAX);
        assert(call("sceVideoOutRegisterBuffers", vh, 0, addrs_ptr, 1, attr_addr) == 0);
        assert(call("sceVideoOutRegisterBuffers", vh, 1, addrs_ptr, 1, attr_addr) == 0);
        assert(call("sceVideoOutRegisterBuffers", vh, 1, addrs_ptr, 1, attr_addr) == UINT64_MAX);
        assert(call("sceVideoOutRegisterBuffers", vh, 16, addrs_ptr, 1, attr_addr) == UINT64_MAX);
        assert(call("sceVideoOutRegisterBuffers", vh, 2, 0, 1, attr_addr) == UINT64_MAX);

        // Um flip rejeitado não avança a sequência nem cria imagem falsa.
        assert(call("sceVideoOutSubmitFlip", vh, 2, 0, 42) == UINT64_MAX);
        assert(!vom.has_new_frame() && vom.flip_count() == 0);
        assert(call("sceGnmSubmitCommandBuffers", 1, 0, 0) == UINT64_MAX);
        assert(call("sceVideoOutSubmitFlip", vh, 0, 0, 42) == 0);
        assert(vom.has_new_frame());
        assert(vom.flip_count() == 1);

        constexpr std::uint64_t status_addr = 0x6200;
        assert(call("sceVideoOutGetFlipStatus", vh, status_addr) == 0);
        std::uint64_t flip_seq = 0;
        assert(m.load(status_addr, &flip_seq, 8) == MemoryError::ok && flip_seq == 1);

        // Submissão PM4 com flip via sceGnmSubmitAndFlipCommandBuffers
        std::uint32_t pm4_cmds[] = {
            0xC0001000, 0x00000000 // NOP
        };
        constexpr std::uint64_t dcb_addr = 0x6500;
        constexpr std::uint64_t dcb_ptr_table = 0x6600;
        constexpr std::uint64_t dcb_size_table = 0x6650;
        assert(m.store(dcb_addr, pm4_cmds, sizeof(pm4_cmds)) == MemoryError::ok);
        assert(m.store(dcb_ptr_table, &dcb_addr, 8) == MemoryError::ok);
        std::uint32_t dcb_size_bytes = sizeof(pm4_cmds);
        assert(m.store(dcb_size_table, &dcb_size_bytes, 4) == MemoryError::ok);

        // T154: Submissão de draw com render target inválido deve falhar e não realizar flip
        const std::uint32_t draw_cmds[] = {0xC0012D00, 3, 0};
        assert(m.store(dcb_addr, draw_cmds, sizeof(draw_cmds)) == MemoryError::ok);
        dcb_size_bytes = sizeof(draw_cmds);
        assert(m.store(dcb_size_table, &dcb_size_bytes, 4) == MemoryError::ok);
        assert(call("sceGnmSubmitCommandBuffers", 1, dcb_ptr_table, dcb_size_table) == UINT64_MAX);
        assert(rt.last_rasterizer_status() == RasterizerStatus::InvalidRenderTarget);
        assert(rt.rasterizer_failure_count() == 1);
        assert(rt.submit_count() == 0);

        // T154: sceGnmSubmitAndFlipCommandBuffers com comando inválido também falha e NÃO executa flip
        assert(call("sceGnmSubmitAndFlipCommandBuffers", 1, dcb_ptr_table, dcb_size_table, 0, 0, 0, 99, 42) == UINT64_MAX);
        assert(rt.rasterizer_failure_count() == 2);
        assert(vom.flip_count() == 1); // Permanece 1, sem flip espúrio

        // Se o segundo IB falhar, o primeiro não pode apagar ou duplicar o draw
        // aceito anteriormente. Nenhum contador deve avançar.
        const std::uint64_t ibs[] = {dcb_addr, 0};
        const std::uint32_t sizes[] = {sizeof(draw_cmds), sizeof(draw_cmds)};
        assert(m.store(dcb_ptr_table, ibs, sizeof(ibs)) == MemoryError::ok);
        assert(m.store(dcb_size_table, sizes, sizeof(sizes)) == MemoryError::ok);
        assert(call("sceGnmSubmitCommandBuffers", 2, dcb_ptr_table, dcb_size_table) == UINT64_MAX);
        assert(rt.submit_count() == 0);

        // Mesmo um registro aplicado pelo primeiro IB deve sofrer rollback.
        const std::uint32_t register_cmds[] = {0xC0016900, 0x200, 0x12345678};
        const auto old_reg = rt.gnm_context()->context_reg(0x200);
        assert(m.store(dcb_addr, register_cmds, sizeof(register_cmds)) == MemoryError::ok);
        const std::uint32_t register_sizes[] = {sizeof(register_cmds), sizeof(register_cmds)};
        assert(m.store(dcb_size_table, register_sizes, sizeof(register_sizes)) == MemoryError::ok);
        assert(call("sceGnmSubmitCommandBuffers", 2, dcb_ptr_table, dcb_size_table) == UINT64_MAX);
        assert(rt.gnm_context()->context_reg(0x200) == old_reg);
        assert(rt.submit_count() == 0);

        // Caminho válido: comandos NOP com flip bem-sucedido
        assert(m.store(dcb_ptr_table, &dcb_addr, 8) == MemoryError::ok);
        assert(m.store(dcb_addr, pm4_cmds, sizeof(pm4_cmds)) == MemoryError::ok);
        dcb_size_bytes = sizeof(pm4_cmds);
        assert(m.store(dcb_size_table, &dcb_size_bytes, 4) == MemoryError::ok);

        assert(call("sceGnmSubmitAndFlipCommandBuffers", 1, dcb_ptr_table, dcb_size_table, 0, 0, 0, 99, 42) == 0);
        assert(rt.gnm_context()->draw_calls().empty());
        assert(rt.last_rasterizer_status() == RasterizerStatus::Ok);
        assert(rt.rasterizer_failure_count() == 2);
        assert(rt.submit_count() == 1);
        assert(vom.has_new_frame());
        assert(vom.flip_count() == 2);

        assert(call("sceVideoOutClose", vh) == 0);
    }

    // 8. stdio HLE memory safety
    {
        std::string filename = "hle_stdio_test_" + unique_tag() + ".tmp";
        std::string tmp_file = (std::filesystem::temp_directory_path() / "antigravity" / filename).string();
        std::string guest_file = "/temp/" + filename;
        assert(m.store(0x6300, guest_file.c_str(), guest_file.size() + 1) == MemoryError::ok);
        assert(m.store(0x6400, "wb+", 4) == MemoryError::ok);

        auto fh = call("fopen", 0x6300, 0x6400);
        assert(fh > 0);

        // Escape path traversal must be rejected
        assert(m.store(0x6350, "../escape.txt", 14) == MemoryError::ok);
        assert(call("fopen", 0x6350, 0x6400) == 0);

        // Safe fwrite via guest memory
        assert(m.store(0x6500, "Hello Orbis Stdio!\n", 20) == MemoryError::ok);
        assert(call("fwrite", 0x6500, 1, 20, fh) == 20);

        // fseek & ftell
        assert(call("fseek", fh, 0, 0) == 0);
        assert(call("ftell", fh) == 0);

        // Safe fread via guest memory
        assert(call("fread", 0x6600, 1, 5, fh) == 5);
        char read_buf[16] = {};
        assert(m.load(0x6600, read_buf, 5) == MemoryError::ok);
        assert(std::string(read_buf, 5) == "Hello");

        // Safe fgets via guest memory
        assert(call("fseek", fh, 0, 0) == 0);
        assert(call("fgets", 0x6700, 32, fh) == 0x6700);
        char line_buf[32] = {};
        assert(m.load(0x6700, line_buf, sizeof(line_buf)) == MemoryError::ok);
        assert(std::string(line_buf) == "Hello Orbis Stdio!\n");

        // Seek para fim do arquivo antes de alternar de leitura para escrita (conforme regra stdio C)
        assert(call("fseek", fh, 0, 2 /* SEEK_END */) == 0);

        // T092: fprintf e vfprintf
        assert(m.store(0x6800, "val=%d msg=%s\n", 15) == MemoryError::ok);
        assert(m.store(0x6850, "Bloodborne", 11) == MemoryError::ok);
        auto fprint_len = call("fprintf", fh, 0x6800, 999, 0x6850);
        assert(fprint_len == 23);

        // vfprintf com va_list
        std::uint64_t vfp_reg_vals[2] = {static_cast<std::uint64_t>(42), 0x6850};
        assert(m.store(0x68a0, vfp_reg_vals, sizeof(vfp_reg_vals)) == MemoryError::ok);
        SysVVaListTest vfp_vl{0, 48, 0, 0x68a0};
        assert(m.store(0x68c0, &vfp_vl, sizeof(vfp_vl)) == MemoryError::ok);
        auto vfprint_len = call("vfprintf", fh, 0x6800, 0x68c0);
        assert(vfprint_len == 22);

        // Rejeicao explicita para handle invalido ou formato invalido
        assert(call("fprintf", 99999, 0x6800, 1) == static_cast<std::uint64_t>(-1));
        assert(call("fprintf", fh, 0) == static_cast<std::uint64_t>(-1));
        assert(call("vfprintf", 99999, 0x6800, 0x68c0) == static_cast<std::uint64_t>(-1));

        // fprintf para stdout/stderr
        assert(call("fprintf", 1, 0x6800, 1, 0x6850) == 21);
        assert(call("fprintf", 2, 0x6800, 2, 0x6850) == 21);

        assert(call("fclose", fh) == 0);

        // Operation on closed handle returns error safely without crashing
        assert(call("fread", 0x6600, 1, 4, fh) == 0);
        assert(call("fclose", fh) == static_cast<std::uint64_t>(-1));

        std::error_code rm_ec;
        std::filesystem::remove(tmp_file, rm_ec);
    }

    // =========================================================================
    // T090: sceKernelWrite e write com chunking (64 KiB), validacao e stdout/stderr
    // =========================================================================
    {
        // 1. nbyte == 0 retorna 0
        assert(call("sceKernelWrite", 1, 0x5d00, 0) == 0);
        assert(call("write", 1, 0x5d00, 0) == 0);

        // 2. Ponteiro nulo com nbyte > 0 retorna erro explícito
        assert(call("sceKernelWrite", 1, 0, 100) == 0x80020016);
        assert(call("write", 1, 0, 100) == static_cast<std::uint64_t>(-1));

        // 3. Ponteiro para memória guest não mapeada retorna erro
        assert(call("sceKernelWrite", 1, 0xFFFFFFFF0000ULL, 100) == 0x80020016);
        assert(call("write", 2, 0xFFFFFFFF0000ULL, 100) == static_cast<std::uint64_t>(-1));

        // 4. FD inválido retorna erro de descritor inexistente
        assert(call("sceKernelWrite", 9999, 0x5d00, 10) == 0x80020009);
        assert(call("write", 9999, 0x5d00, 10) == static_cast<std::uint64_t>(-1));

        // 5. Escrita válida para stdout (1) e stderr (2)
        const char hello_msg[] = "[test] T090 stdout chunk test\n";
        assert(m.store(0x5d00, hello_msg, sizeof(hello_msg) - 1) == MemoryError::ok);
        assert(call("sceKernelWrite", 1, 0x5d00, sizeof(hello_msg) - 1) == sizeof(hello_msg) - 1);
        assert(call("write", 2, 0x5d00, sizeof(hello_msg) - 1) == sizeof(hello_msg) - 1);
    }

    // =========================================================================
    // Phase 20 Convergence: T093 (freopen), T094 (fopen), T095 (lstat & access)
    // =========================================================================
    {
        VirtualFileSystem vfs_p20;
        const std::string tag = unique_tag();
        std::filesystem::path test_base = std::filesystem::temp_directory_path() / ("vfs_phase20_" + tag);
        std::filesystem::path app0_dir = test_base / "app0";
        std::filesystem::path temp_dir = test_base / "temp";
        std::error_code ec;
        std::filesystem::create_directories(app0_dir, ec);
        std::filesystem::create_directories(temp_dir, ec);

        // Cria arquivos de teste em app0 (read-only) e temp (read-write)
        const std::string app0_content = "ORIGINAL_APP0_RO_CONTENT_T094";
        std::filesystem::path app0_file = app0_dir / "readonly_file.txt";
        {
            std::ofstream ofs(app0_file, std::ios::binary);
            ofs.write(app0_content.data(), app0_content.size());
        }

        const std::string temp_content = "INITIAL_TEMP_RW_CONTENT_T093";
        std::filesystem::path temp_file = temp_dir / "writable_file.txt";
        {
            std::ofstream ofs(temp_file, std::ios::binary);
            ofs.write(temp_content.data(), temp_content.size());
        }

        // Monta /app0 como somente-leitura (true) e /temp como leitura-escrita (false)
        assert(vfs_p20.mount("/app0", app0_dir.string(), true));
        assert(vfs_p20.mount("/temp", temp_dir.string(), false));

        // Stub sintético para verificar resolução de lstat e access em arquivos puramente virtuais
        std::vector<std::uint8_t> synth_data = {'S', 'Y', 'N', 'T', 'H', '0', '9', '5'};
        assert(vfs_p20.set_synthetic_file("/app0/synth_test.bin", synth_data));

        rt.attach_vfs(&vfs_p20);

        // T106: caminhos absolutos do host não podem contornar mounts, nem mesmo
        // dentro da pasta temporária ou num irmão com prefixo idêntico.
        std::filesystem::path sibling_dir = test_base.string() + "_sibling";
        std::filesystem::create_directories(sibling_dir, ec);
        std::filesystem::path sibling_file = sibling_dir / "secret.txt";
        {
            std::ofstream ofs(sibling_file);
            ofs << "preserve";
        }
        for (const auto& host_file : {temp_file, sibling_file}) {
            const auto absolute = host_file.string();
            assert(m.store(0x7100, absolute.c_str(), absolute.size() + 1) == MemoryError::ok);
            assert(m.store(0x7300, "w", 2) == MemoryError::ok);
            assert(call("fopen", 0x7100, 0x7300) == 0);
            assert(m.store(0x7300, "r", 2) == MemoryError::ok);
            assert(call("fopen", 0x7100, 0x7300) == 0);
        }
        {
            std::ifstream ifs(sibling_file);
            std::string contents;
            ifs >> contents;
            assert(contents == "preserve");
        }
        std::filesystem::remove_all(sibling_dir, ec);

        // Endereços de memória guest para buffers de teste
        constexpr std::uint64_t g_path1 = 0x7100;
        constexpr std::uint64_t g_path2 = 0x7200;
        constexpr std::uint64_t g_mode  = 0x7300;
        constexpr std::uint64_t g_stat  = 0x7400;
        constexpr std::uint64_t g_buf   = 0x7500;

        // -----------------------------------------------------------------
        // T094: fopen em mounts somente-leitura vs graváveis
        // -----------------------------------------------------------------
        // 1. Modos de escrita em /app0 devem ser rejeitados: "w", "w+", "wb", "a", "ab", "r+", "w+b"
        const char* write_modes[] = {"w", "w+", "wb", "a", "ab", "r+", "w+b"};
        for (const char* wmode : write_modes) {
            assert(m.store(g_path1, "/app0/readonly_file.txt", 25) == MemoryError::ok);
            assert(m.store(g_mode, wmode, std::strlen(wmode) + 1) == MemoryError::ok);
            auto h = call("fopen", g_path1, g_mode);
            assert(h == 0); // Rejeitado!
        }

        // Confirma que o conteúdo original de readonly_file.txt permanece 100% intacto
        {
            std::ifstream ifs(app0_file, std::ios::binary);
            std::string current((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            assert(current == app0_content);
        }

        // 2. Modos de leitura em /app0 devem ter sucesso
        assert(m.store(g_path1, "/app0/readonly_file.txt", 25) == MemoryError::ok);
        assert(m.store(g_mode, "r", 2) == MemoryError::ok);
        auto fh_ro = call("fopen", g_path1, g_mode);
        assert(fh_ro > 0);
        char ro_buf[64] = {};
        assert(call("fread", g_buf, 1, app0_content.size(), fh_ro) == app0_content.size());
        assert(m.load(g_buf, ro_buf, app0_content.size()) == MemoryError::ok);
        assert(std::string(ro_buf, app0_content.size()) == app0_content);
        assert(call("fclose", fh_ro) == 0);

        // 3. Modos de escrita em mount gravável (/temp) devem ter sucesso
        assert(m.store(g_path1, "/temp/writable_file.txt", 24) == MemoryError::ok);
        assert(m.store(g_mode, "wb", 3) == MemoryError::ok);
        auto fh_rw = call("fopen", g_path1, g_mode);
        assert(fh_rw > 0);
        const std::string new_data = "NEW_TEMP_WRITTEN_PAYLOAD";
        assert(m.store(g_buf, new_data.data(), new_data.size()) == MemoryError::ok);
        assert(call("fwrite", g_buf, 1, new_data.size(), fh_rw) == new_data.size());
        assert(call("fclose", fh_rw) == 0);

        // 4. Tentativa de escape com .. ou caminhos fora dos mounts autorizados
        assert(m.store(g_path1, "/app0/../escape.txt", 20) == MemoryError::ok);
        assert(m.store(g_mode, "r", 2) == MemoryError::ok);
        assert(call("fopen", g_path1, g_mode) == 0);

        assert(m.store(g_path1, "/unmounted/file.txt", 20) == MemoryError::ok);
        assert(call("fopen", g_path1, g_mode) == 0);

        // Argumentos nulos
        assert(call("fopen", 0, g_mode) == 0);
        assert(call("fopen", g_path1, 0) == 0);

        // -----------------------------------------------------------------
        // T093: freopen path containment e confinamento a mounts autorizados
        // -----------------------------------------------------------------
        // Abre um stream inicial válido em /temp
        assert(m.store(g_path1, "/temp/writable_file.txt", 24) == MemoryError::ok);
        assert(m.store(g_mode, "r+", 3) == MemoryError::ok);
        auto fh_fre = call("fopen", g_path1, g_mode);
        assert(fh_fre > 0);

        // 1. Rejeição de travessia com .. no freopen
        assert(m.store(g_path2, "/temp/../escape.txt", 20) == MemoryError::ok);
        assert(call("freopen", g_path2, g_mode, fh_fre) == 0);

        // 2. Rejeição de caminho fora de mounts autorizados (escape para sistema host)
        assert(m.store(g_path2, "/etc/passwd", 12) == MemoryError::ok);
        assert(call("freopen", g_path2, g_mode, fh_fre) == 0);

        // 3. Rejeição de modo de escrita ("w") em mount somente-leitura (/app0)
        assert(m.store(g_path2, "/app0/readonly_file.txt", 25) == MemoryError::ok);
        assert(m.store(g_mode, "w", 2) == MemoryError::ok);
        assert(call("freopen", g_path2, g_mode, fh_fre) == 0);

        // Confirma que app0_file ainda está intacto
        {
            std::ifstream ifs(app0_file, std::ios::binary);
            std::string current((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            assert(current == app0_content);
        }

        // 4. freopen válido para leitura em mount somente-leitura (/app0)
        assert(m.store(g_mode, "r", 2) == MemoryError::ok);
        auto fh_fre_ro = call("freopen", g_path2, g_mode, fh_fre);
        assert(fh_fre_ro == fh_fre); // Mantém o mesmo handle
        char fre_buf[64] = {};
        assert(call("fread", g_buf, 1, app0_content.size(), fh_fre_ro) == app0_content.size());
        assert(m.load(g_buf, fre_buf, app0_content.size()) == MemoryError::ok);
        assert(std::string(fre_buf, app0_content.size()) == app0_content);

        // 5. freopen para escrita em mount gravável (/temp)
        assert(m.store(g_path1, "/temp/freopen_out.txt", 22) == MemoryError::ok);
        assert(m.store(g_mode, "w", 2) == MemoryError::ok);
        auto fh_fre_wr = call("freopen", g_path1, g_mode, fh_fre_ro);
        assert(fh_fre_wr == fh_fre);
        const std::string fre_data = "FREOPEN_WRITTEN_DATA";
        assert(m.store(g_buf, fre_data.data(), fre_data.size()) == MemoryError::ok);
        assert(call("fwrite", g_buf, 1, fre_data.size(), fh_fre_wr) == fre_data.size());
        assert(call("fclose", fh_fre_wr) == 0);

        // 6. Parâmetros inválidos para freopen: handle inexistente, ponteiro nulo
        assert(call("freopen", g_path1, g_mode, 99999) == 0);
        assert(call("freopen", 0, g_mode, fh_fre) == 0);
        assert(call("freopen", g_path1, 0, fh_fre) == 0);

        // -----------------------------------------------------------------
        // T095: lstat e access com resolução VFS sem sucesso fictício
        // -----------------------------------------------------------------
        // 1. lstat em arquivo existente em /app0
        assert(m.store(g_path1, "/app0/readonly_file.txt", 25) == MemoryError::ok);
        assert(call("lstat", g_path1, g_stat) == 0);
        SceKernelStat lst{};
        assert(m.load(g_stat, &lst, sizeof(lst)) == MemoryError::ok);
        assert(lst.st_size == static_cast<std::int64_t>(app0_content.size()));
        assert((lst.st_mode & 0100000) != 0); // Arquivo regular

        // 2. lstat em arquivo sintético no VFS
        assert(m.store(g_path1, "/app0/synth_test.bin", 21) == MemoryError::ok);
        assert(call("lstat", g_path1, g_stat) == 0);
        assert(m.load(g_stat, &lst, sizeof(lst)) == MemoryError::ok);
        assert(lst.st_size == static_cast<std::int64_t>(synth_data.size()));

        // 3. lstat em arquivo inexistente deve falhar (retornar -1)
        assert(m.store(g_path1, "/app0/no_such_file.bin", 23) == MemoryError::ok);
        assert(call("lstat", g_path1, g_stat) == static_cast<std::uint64_t>(-1));

        // 4. lstat com travessia ou fora do sandbox deve falhar
        assert(m.store(g_path1, "/app0/../escape.txt", 20) == MemoryError::ok);
        assert(call("lstat", g_path1, g_stat) == static_cast<std::uint64_t>(-1));

        assert(m.store(g_path1, "/etc/passwd", 12) == MemoryError::ok);
        assert(call("lstat", g_path1, g_stat) == static_cast<std::uint64_t>(-1));

        // 5. lstat com ponteiros inválidos
        assert(call("lstat", 0, g_stat) == static_cast<std::uint64_t>(-1));
        assert(call("lstat", g_path1, 0) == static_cast<std::uint64_t>(-1));
        assert(call("lstat", g_path1, 0xFFFFFFFF0000ULL) == static_cast<std::uint64_t>(-1));

        // 6. access em arquivo existente em /app0 (mount read-only)
        // F_OK = 0, R_OK = 4, W_OK = 2, X_OK = 1
        assert(m.store(g_path1, "/app0/readonly_file.txt", 25) == MemoryError::ok);
        assert(call("access", g_path1, 0 /* F_OK */) == 0);
        assert(call("access", g_path1, 4 /* R_OK */) == 0);
        // W_OK em mount read-only DEVE falhar (retornar -1)
        assert(call("access", g_path1, 2 /* W_OK */) == static_cast<std::uint64_t>(-1));
        assert(call("access", g_path1, 6 /* R_OK|W_OK */) == static_cast<std::uint64_t>(-1));

        // 7. access em arquivo sintético (read-only por definição)
        assert(m.store(g_path1, "/app0/synth_test.bin", 21) == MemoryError::ok);
        assert(call("access", g_path1, 0 /* F_OK */) == 0);
        assert(call("access", g_path1, 4 /* R_OK */) == 0);
        assert(call("access", g_path1, 2 /* W_OK */) == static_cast<std::uint64_t>(-1));

        // 8. access em mount gravável (/temp)
        assert(m.store(g_path1, "/temp/freopen_out.txt", 22) == MemoryError::ok);
        assert(call("access", g_path1, 0 /* F_OK */) == 0);
        assert(call("access", g_path1, 4 /* R_OK */) == 0);
        assert(call("access", g_path1, 2 /* W_OK */) == 0); // Gravável!
        assert(call("access", g_path1, 6 /* R_OK|W_OK */) == 0);

        // 9. access em arquivo ausente DEVE falhar (sem sucesso fictício)
        assert(m.store(g_path1, "/app0/does_not_exist.dat", 25) == MemoryError::ok);
        assert(call("access", g_path1, 0 /* F_OK */) == static_cast<std::uint64_t>(-1));
        assert(call("access", g_path1, 4 /* R_OK */) == static_cast<std::uint64_t>(-1));

        // 10. access com travessia ou caminho host fora de mount
        assert(m.store(g_path1, "/app0/../escape.txt", 20) == MemoryError::ok);
        assert(call("access", g_path1, 0) == static_cast<std::uint64_t>(-1));

        assert(m.store(g_path1, "/etc/shadow", 12) == MemoryError::ok);
        assert(call("access", g_path1, 0) == static_cast<std::uint64_t>(-1));

        // 11. access com modo inválido ou ponteiro nulo
        assert(call("access", 0, 0) == static_cast<std::uint64_t>(-1));
        assert(m.store(g_path1, "/temp/freopen_out.txt", 22) == MemoryError::ok);
        assert(call("access", g_path1, 0xFF) == static_cast<std::uint64_t>(-1)); // modo inválido

        // Desanexa VFS e limpa diretórios temporários
        rt.attach_vfs(nullptr);
        std::filesystem::remove_all(test_base, ec);
    }

    // =========================================================================
    // Phase 21 Convergence: T096 (mprotect subfaixas) e T097 (batch unmap)
    // =========================================================================
    {
        // -----------------------------------------------------------------
        // T096: sceKernelMprotect somente à subfaixa de páginas solicitada
        // -----------------------------------------------------------------
        constexpr std::uint64_t m_size = 4 * 16384;
        std::uint64_t inout_addr = 0;
        assert(m.store(0x8050, &inout_addr, 8) == MemoryError::ok);
        assert(call("sceKernelMmap", 0x8050, m_size, 0x33 /* RW */, 0) == 0);
        std::uint64_t m_base = 0;
        assert(m.load(0x8050, &m_base, 8) == MemoryError::ok && m_base != 0);

        std::uint8_t payload[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        assert(m.store(m_base + 0 * 16384, payload, sizeof(payload)) == MemoryError::ok);
        assert(m.store(m_base + 1 * 16384, payload, sizeof(payload)) == MemoryError::ok);
        assert(m.store(m_base + 2 * 16384, payload, sizeof(payload)) == MemoryError::ok);
        assert(m.store(m_base + 3 * 16384, payload, sizeof(payload)) == MemoryError::ok);

        const std::uint64_t sub_addr = m_base + 16384;
        constexpr std::uint64_t sub_len = 16384;
        assert(call("sceKernelMprotect", sub_addr, sub_len, 1 /* PROT_READ */) == 0);

        std::uint8_t read_check[16] = {};
        assert(m.load(sub_addr, read_check, sizeof(read_check)) == MemoryError::ok);
        assert(std::memcmp(read_check, payload, sizeof(payload)) == 0);
        assert(m.store(sub_addr, payload, sizeof(payload)) == MemoryError::denied); // Escrita negada!

        assert(m.store(m_base + 0 * 16384, payload, sizeof(payload)) == MemoryError::ok);
        assert(m.store(m_base + 2 * 16384, payload, sizeof(payload)) == MemoryError::ok);
        assert(m.store(m_base + 3 * 16384, payload, sizeof(payload)) == MemoryError::ok);

        constexpr std::uint64_t q_start = 0x8100, q_end = 0x8108, q_prot = 0x8110;
        assert(call("sceKernelQueryMemoryProtection", sub_addr, q_start, q_end, q_prot) == 0);
        std::uint64_t res_start = 0, res_end = 0;
        std::uint32_t res_prot = 0;
        assert(m.load(q_start, &res_start, 8) == MemoryError::ok && res_start == sub_addr);
        assert(m.load(q_end, &res_end, 8) == MemoryError::ok && res_end == sub_addr + sub_len);
        assert(m.load(q_prot, &res_prot, 4) == MemoryError::ok && (res_prot & 0x11) == 0x11 && (res_prot & 0x22) == 0);

        assert(call("sceKernelQueryMemoryProtection", m_base, q_start, q_end, q_prot) == 0);
        assert(m.load(q_start, &res_start, 8) == MemoryError::ok && res_start == m_base);
        assert(m.load(q_end, &res_end, 8) == MemoryError::ok && res_end == m_base + 16384);
        assert(m.load(q_prot, &res_prot, 4) == MemoryError::ok && (res_prot & 0x33) == 0x33);

        assert(call("sceKernelMprotect", sub_addr + 1, sub_len, 1) != 0);
        assert(call("sceKernelMprotect", sub_addr, sub_len + 1, 1) != 0);
        assert(call("sceKernelMprotect", 0x99000000ULL, 16384, 1) == 0x8002000e);

        assert(call("sceKernelMunmap", m_base, m_size) == 0);

        // -----------------------------------------------------------------
        // T097: Validar faixa e resultado de unmap em sceKernelBatchMap/2
        // -----------------------------------------------------------------
        inout_addr = 0;
        assert(m.store(0x8050, &inout_addr, 8) == MemoryError::ok);
        assert(call("sceKernelMmap", 0x8050, 16384, 0x33, 0) == 0);
        std::uint64_t b1 = 0;
        assert(m.load(0x8050, &b1, 8) == MemoryError::ok && b1 != 0);

        inout_addr = 0;
        assert(m.store(0x8050, &inout_addr, 8) == MemoryError::ok);
        assert(call("sceKernelMmap", 0x8050, 16384, 0x33, 0) == 0);
        std::uint64_t b2 = 0;
        assert(m.load(0x8050, &b2, 8) == MemoryError::ok && b2 != 0);

        inout_addr = 0;
        assert(m.store(0x8050, &inout_addr, 8) == MemoryError::ok);
        assert(call("sceKernelMmap", 0x8050, 16384, 0x33, 0) == 0);
        std::uint64_t b3 = 0;
        assert(m.load(0x8050, &b3, 8) == MemoryError::ok && b3 != 0);

        constexpr std::uint64_t batch_entries = 0x8200;
        constexpr std::uint64_t batch_processed = 0x8300;

        auto write_entry = [&](std::uint64_t ptr, std::uint64_t addr, std::uint64_t len, std::int32_t op) {
            std::uint64_t off = 0;
            std::int8_t prot = 0x33;
            std::int8_t type = 0;
            std::int16_t res = 0;
            assert(m.store(ptr, &addr, 8) == MemoryError::ok);
            assert(m.store(ptr + 8, &off, 8) == MemoryError::ok);
            assert(m.store(ptr + 16, &len, 8) == MemoryError::ok);
            assert(m.store(ptr + 24, &prot, 1) == MemoryError::ok);
            assert(m.store(ptr + 25, &type, 1) == MemoryError::ok);
            assert(m.store(ptr + 26, &res, 2) == MemoryError::ok);
            assert(m.store(ptr + 28, &op, 4) == MemoryError::ok);
        };

        write_entry(batch_entries + 0 * 32, b1, 16384, 1 /* unmap */);
        write_entry(batch_entries + 1 * 32, b2, 16384, 1 /* unmap */);
        assert(call("sceKernelBatchMap", batch_entries, 2, batch_processed) == 0);
        std::int32_t done = 0;
        assert(m.load(batch_processed, &done, 4) == MemoryError::ok && done == 2);
        assert(m.load(b1, payload, 4) == MemoryError::unmapped);
        assert(m.load(b2, payload, 4) == MemoryError::unmapped);
        assert(m.store(b3, payload, 4) == MemoryError::ok);

        write_entry(batch_entries + 0 * 32, b3, 16384, 1 /* unmap */);
        write_entry(batch_entries + 1 * 32, 0x9990000ULL, 16384, 1 /* unmap inválido */);
        write_entry(batch_entries + 2 * 32, 0xAAAA000ULL, 16384, 1);
        assert(call("sceKernelBatchMap", batch_entries, 3, batch_processed) != 0);
        assert(m.load(batch_processed, &done, 4) == MemoryError::ok && done == 1);
        assert(m.load(b3, payload, 4) == MemoryError::unmapped);
    }

    // =========================================================================
    // Phase 23 Convergence: T100 (remove/rmdir) e T101 (localtime/gmtime)
    // =========================================================================
    {
        VirtualFileSystem vfs_p23;
        const std::string tag = unique_tag();
        std::filesystem::path test_base = std::filesystem::temp_directory_path() / ("vfs_phase23_" + tag);
        std::filesystem::path app0_dir = test_base / "app0";
        std::filesystem::path temp_dir = test_base / "temp";
        std::error_code ec;
        std::filesystem::create_directories(app0_dir, ec);
        std::filesystem::create_directories(temp_dir, ec);

        std::filesystem::path ro_file = app0_dir / "protected.dat";
        {
            std::ofstream ofs(ro_file, std::ios::binary);
            ofs << "PROTECTED_DATA_P23";
        }
        std::filesystem::path rw_file = temp_dir / "temp_file.dat";
        std::filesystem::path rw_subfolder = temp_dir / "temp_subfolder";
        {
            std::ofstream ofs(rw_file, std::ios::binary);
            ofs << "TEMP_DATA";
            std::filesystem::create_directories(rw_subfolder, ec);
        }

        assert(vfs_p23.mount("/app0", app0_dir.string(), true));
        assert(vfs_p23.mount("/temp", temp_dir.string(), false));
        rt.attach_vfs(&vfs_p23);

        constexpr std::uint64_t g_path = 0x8500;

        assert(m.store(g_path, "/app0/protected.dat", 20) == MemoryError::ok);
        assert(call("remove", g_path) == static_cast<std::uint64_t>(-1));
        assert(std::filesystem::exists(ro_file));

        assert(m.store(g_path, "/app0", 6) == MemoryError::ok);
        assert(call("rmdir", g_path) == static_cast<std::uint64_t>(-1));

        assert(m.store(g_path, "/app0/../escape.txt", 20) == MemoryError::ok);
        assert(call("remove", g_path) == static_cast<std::uint64_t>(-1));
        assert(m.store(g_path, "/etc/shadow", 12) == MemoryError::ok);
        assert(call("remove", g_path) == static_cast<std::uint64_t>(-1));

        assert(m.store(g_path, "/temp/temp_file.dat", 20) == MemoryError::ok);
        assert(call("remove", g_path) == 0);
        assert(!std::filesystem::exists(rw_file));

        assert(m.store(g_path, "/temp/temp_subfolder", 21) == MemoryError::ok);
        assert(call("rmdir", g_path) == 0);
        assert(!std::filesystem::exists(rw_subfolder));

        assert(call("remove", 0) == static_cast<std::uint64_t>(-1));
        assert(call("rmdir", 0) == static_cast<std::uint64_t>(-1));

        constexpr std::uint64_t g_time_ptr = 0x8600;
        std::int64_t timestamp = 1776345600;
        assert(m.store(g_time_ptr, &timestamp, 8) == MemoryError::ok);

        std::uint64_t tm_buf1 = call("gmtime", g_time_ptr);
        assert(tm_buf1 != 0);
        std::int32_t year1 = 0;
        assert(m.load(tm_buf1 + 20, &year1, 4) == MemoryError::ok);
        assert(year1 == 2026 - 1900);

        for (int i = 0; i < 50; ++i) {
            std::uint64_t tm_buf_loop = call("gmtime", g_time_ptr);
            assert(tm_buf_loop == tm_buf1);
            std::uint64_t loc_buf = call("localtime", g_time_ptr);
            assert(loc_buf == tm_buf1);
        }

        assert(call("gmtime", 0) == 0);
        assert(call("localtime", 0) == 0);

        rt.attach_vfs(nullptr);
        std::filesystem::remove_all(test_base, ec);
    }

    // =========================================================================
    // Phase 24 Convergence: T102 (unlink/mkdir/rename RO), T103 (_open/_close/_lseek), T104 (ctime/strerror)
    // =========================================================================
    {
        VirtualFileSystem vfs_p24;
        const std::string tag = unique_tag();
        std::filesystem::path test_base = std::filesystem::temp_directory_path() / ("vfs_phase24_" + tag);
        std::filesystem::path app0_dir = test_base / "app0";
        std::filesystem::path temp_dir = test_base / "temp";
        std::error_code ec;
        std::filesystem::create_directories(app0_dir, ec);
        std::filesystem::create_directories(temp_dir, ec);

        std::filesystem::path app0_file = app0_dir / "immutable.bin";
        {
            std::ofstream ofs(app0_file, std::ios::binary);
            ofs << "CANNOT_MODIFY_THIS_DATA";
        }
        std::filesystem::path temp_file = temp_dir / "scratch.bin";
        {
            std::ofstream ofs(temp_file, std::ios::binary);
            ofs << "SCRATCH_DATA_0123456789";
        }

        assert(vfs_p24.mount("/app0", app0_dir.string(), true));
        assert(vfs_p24.mount("/temp", temp_dir.string(), false));
        rt.attach_vfs(&vfs_p24);

        constexpr std::uint64_t g_path1 = 0x8700;
        constexpr std::uint64_t g_path2 = 0x8800;
        constexpr std::uint64_t g_buf   = 0x8900;

        assert(m.store(g_path1, "/app0/immutable.bin", 21) == MemoryError::ok);
        assert(call("unlink", g_path1) == static_cast<std::uint64_t>(-1));
        assert(call("sceKernelUnlink", g_path1) == 0x8002001e);
        assert(std::filesystem::exists(app0_file));

        assert(m.store(g_path1, "/app0/forbidden_dir", 21) == MemoryError::ok);
        assert(call("mkdir", g_path1) == static_cast<std::uint64_t>(-1));
        assert(!std::filesystem::exists(app0_dir / "forbidden_dir"));

        assert(m.store(g_path1, "/app0/immutable.bin", 21) == MemoryError::ok);
        assert(m.store(g_path2, "/temp/stolen.bin", 17) == MemoryError::ok);
        assert(call("rename", g_path1, g_path2) == static_cast<std::uint64_t>(-1));
        assert(std::filesystem::exists(app0_file));
        assert(!std::filesystem::exists(temp_dir / "stolen.bin"));

        assert(m.store(g_path1, "/temp/new_dir", 14) == MemoryError::ok);
        assert(call("mkdir", g_path1) == 0);
        assert(std::filesystem::exists(temp_dir / "new_dir"));

        assert(m.store(g_path1, "/temp/scratch.bin", 18) == MemoryError::ok);
        assert(m.store(g_path2, "/temp/renamed_scratch.bin", 26) == MemoryError::ok);
        assert(call("rename", g_path1, g_path2) == 0);
        assert(!std::filesystem::exists(temp_file));
        assert(std::filesystem::exists(temp_dir / "renamed_scratch.bin"));

        assert(call("unlink", g_path2) == 0);
        assert(!std::filesystem::exists(temp_dir / "renamed_scratch.bin"));

        assert(m.store(g_path1, "/app0/immutable.bin", 21) == MemoryError::ok);
        auto fd_open = call("_open", g_path1, 0 /* O_RDONLY */, 0);
        assert(fd_open < 0x80000000ULL && fd_open > 0);

        char read_buf[32] = {};
        assert(call("_read", fd_open, g_buf, 10) == 10);
        assert(m.load(g_buf, read_buf, 10) == MemoryError::ok);
        assert(std::string(read_buf, 10) == "CANNOT_MOD");

        assert(call("_lseek", fd_open, 0, 0 /* SEEK_SET */) == 0);
        assert(call("_read", fd_open, g_buf, 6) == 6);
        assert(m.load(g_buf, read_buf, 6) == MemoryError::ok);
        assert(std::string(read_buf, 6) == "CANNOT");

        assert(call("_close", fd_open) == 0);

        assert(call("_open", g_path1, 1 /* O_WRONLY */, 0) == static_cast<std::uint64_t>(-1));

        assert(m.store(g_path1, "/app0/../escape.txt", 20) == MemoryError::ok);
        assert(call("_open", g_path1, 0, 0) == static_cast<std::uint64_t>(-1));

        std::int64_t test_time = 1776345600;
        constexpr std::uint64_t g_time_slot = 0x8950;
        assert(m.store(g_time_slot, &test_time, 8) == MemoryError::ok);
        std::uint64_t ctime_buf1 = call("ctime", g_time_slot);
        assert(ctime_buf1 != 0);

        std::uint64_t tm_src = call("gmtime", g_time_slot);
        std::uint64_t asctime_buf1 = call("asctime", tm_src);
        assert(asctime_buf1 != 0);

        std::uint64_t strerr_buf1 = call("strerror", 2 /* ENOENT */);
        assert(strerr_buf1 != 0);
        char err_msg[64] = {};
        assert(m.load(strerr_buf1, err_msg, 6) == MemoryError::ok);
        assert(std::strlen(err_msg) > 0);

        for (int i = 0; i < 50; ++i) {
            assert(call("ctime", g_time_slot) == ctime_buf1);
            assert(call("asctime", tm_src) == asctime_buf1);
            assert(call("strerror", 2) == strerr_buf1);
        }

        assert(call("ctime", 0) == 0);

        rt.attach_vfs(nullptr);
        std::filesystem::remove_all(test_base, ec);
    }

    // =========================================================================
    // Testes de máquina guest com GuestScheduler e adaptador scheduler-traps
    // =========================================================================
    {
        GuestMemory gm(2 * 1024 * 1024);
        assert(gm.map(0x10000, 0x10000, bb::read | bb::write) == MemoryError::ok); // code
        assert(gm.map(0x20000, 0x10000, bb::read | bb::write) == MemoryError::ok); // data
        assert(gm.map(0x40000, 0x10000, bb::read | bb::write) == MemoryError::ok); // main stack

        GuestScheduler sched(gm, 32);
        OrbisRuntime srt(gm);
        srt.attach_scheduler(sched);
        srt.set_procparam(0xfeedfaceULL);
        assert(srt.procparam() == 0xfeedfaceULL);

        // Initialize VFS and SaveDataManager for save data tests
        VirtualFileSystem srt_vfs;
        const std::string tag2 = unique_tag();
        std::string srt_temp_dir = std::filesystem::temp_directory_path().string() + "/save_test_" + tag2;
        assert(srt_vfs.mount("/save", srt_temp_dir));
        srt.save_data_manager().initialize(std::filesystem::temp_directory_path().string() + "/save_data_test_" + tag2, 32 * 1024 * 1024, &srt_vfs);

        constexpr std::uint64_t trap_base = 0x50000;
        constexpr std::uint64_t return_trap = 0x50f00;
        sched.add_trap_range(trap_base, 0x1000);

        std::map<std::uint64_t, std::string> trap_names = {
            {0x50000, "scePthreadCreate"},
            {0x50020, "scePthreadExit"},
            {0x50040, "scePthreadJoin"},
            {0x50060, "scePthreadMutexInit"},
            {0x50080, "scePthreadMutexLock"},
            {0x500a0, "scePthreadMutexUnlock"},
            {0x500c0, "scePthreadCondInit"},
            {0x500e0, "scePthreadCondWait"},
            {0x50100, "scePthreadCondSignal"},
            {0x50120, "sceKernelCreateEventFlag"},
            {0x50140, "sceKernelWaitEventFlag"},
            {0x50160, "sceKernelSetEventFlag"},
            {0x50180, "scePthreadOnce"},
            {0x501a0, "__cxa_guard_acquire"},
            {0x501c0, "__cxa_guard_release"},
            {0x501e0, "sceKernelGetProcParam"}
        };

        sched.set_trap([&](GuestScheduler& s, GuestScheduler::Id tid, CpuState& st) -> bool {
            if (st.rip == return_trap) {
                if (srt.has_pending_callback(tid)) {
                    srt.complete_callback(st, tid);
                    return true;
                }
                s.exit(tid, st.gpr[0]);
                return true;
            }
            auto it = trap_names.find(st.rip);
            if (it == trap_names.end()) return false;
            auto inv = srt.invoke_trap(it->second, st, tid, return_trap);
            if (inv.action == OrbisRuntime::InvocationResult::Action::returned) {
                std::uint64_t ret = 0;
                if (gm.load(st.gpr[4], &ret, 8) != MemoryError::ok) return false;
                st.gpr[4] += 8;
                st.rip = ret;
                st.gpr[0] = inv.value;
            }
            return true;
        });

        auto mov_imm64 = [](unsigned reg, std::uint64_t imm) -> std::vector<std::uint8_t> {
            std::vector<std::uint8_t> v;
            if (reg >= 8) {
                v.push_back(0x49);
                v.push_back(static_cast<std::uint8_t>(0xb8 + (reg - 8)));
            } else {
                v.push_back(0x48);
                v.push_back(static_cast<std::uint8_t>(0xb8 + reg));
            }
            for (int i = 0; i < 8; ++i) v.push_back(static_cast<std::uint8_t>(imm >> (i * 8)));
            return v;
        };
        auto append = [](std::vector<std::uint8_t>& dest, const std::vector<std::uint8_t>& src) {
            dest.insert(dest.end(), src.begin(), src.end());
        };

        // Test 1: sceKernelGetProcParam retornando ponteiro de procparam em rax
        {
            CpuState test_cpu{};
            auto inv = srt.invoke_trap("sceKernelGetProcParam", test_cpu, 1, return_trap);
            assert(inv.action == OrbisRuntime::InvocationResult::Action::returned && inv.value == 0xfeedfaceULL);
        }

        // Test 2: Machine-code scePthreadCreate + scePthreadJoin + scePthreadExit
        {
            // Thread 2 code at 0x10100: stores arg (rdi) to [0x20010], returns 0x888
            std::vector<std::uint8_t> t2_code;
            append(t2_code, mov_imm64(3, 0x20010));       // mov rbx, 0x20010
            append(t2_code, {0x48, 0x89, 0x3b});          // mov [rbx], rdi
            append(t2_code, mov_imm64(0, 0x888));         // mov rax, 0x888
            append(t2_code, {0xc3});                      // ret
            assert(gm.store(0x10100, t2_code.data(), t2_code.size()) == MemoryError::ok);

            // Thread 1 code at 0x10000:
            // scePthreadCreate(&tid2, 0, 0x10100, 0x777)
            // scePthreadJoin(tid2, &out_val)
            // ret
            std::vector<std::uint8_t> t1_code;
            append(t1_code, mov_imm64(7, 0x20000));       // rdi = 0x20000 (out tid)
            append(t1_code, mov_imm64(6, 0));             // rsi = 0 (attr)
            append(t1_code, mov_imm64(2, 0x10100));       // rdx = entry
            append(t1_code, mov_imm64(1, 0x777));         // rcx = arg
            append(t1_code, mov_imm64(0, 0x50000));       // rax = scePthreadCreate
            append(t1_code, {0xff, 0xd0});                // call rax
            // Join
            append(t1_code, mov_imm64(3, 0x20000));       // rbx = 0x20000
            append(t1_code, {0x48, 0x8b, 0x3b});          // mov rdi, [rbx] (tid)
            append(t1_code, mov_imm64(6, 0x20008));       // rsi = 0x20008 (&out_val)
            append(t1_code, mov_imm64(0, 0x50040));       // rax = scePthreadJoin
            append(t1_code, {0xff, 0xd0});                // call rax
            append(t1_code, {0xc3});                      // ret
            assert(gm.store(0x10000, t1_code.data(), t1_code.size()) == MemoryError::ok);

            auto main_tid = sched.create(0x10000, 0x40000, 0x50000, return_trap);
            assert(main_tid && sched.start(main_tid) == ORBIS_OK);

            auto run_res = sched.run(500);
            assert(run_res.stop == GuestScheduler::Stop::idle);
            assert(sched.state(main_tid) == GuestScheduler::State::exited);

            std::uint64_t arg_val = 0, ret_val = 0;
            assert(gm.load(0x20010, &arg_val, 8) == MemoryError::ok && arg_val == 0x777);
            assert(gm.load(0x20008, &ret_val, 8) == MemoryError::ok && ret_val == 0x888);
        }

        // Test 3: Machine-code Mutex Lock/Unlock with contention
        {
            // Thread 2 at 0x10300: locks mutex 0x20020, increments [0x20030], unlocks, ret
            std::vector<std::uint8_t> t2_code;
            append(t2_code, mov_imm64(7, 0x20020));       // rdi = 0x20020 (&mutex)
            append(t2_code, mov_imm64(0, 0x50080));       // rax = scePthreadMutexLock
            append(t2_code, {0xff, 0xd0});                // call rax
            append(t2_code, mov_imm64(3, 0x20030));       // rbx = 0x20030
            append(t2_code, {0x48, 0x8b, 0x03});          // mov rax, [rbx]
            append(t2_code, {0x48, 0x83, 0xc0, 0x01});    // add rax, 1
            append(t2_code, {0x48, 0x89, 0x03});          // mov [rbx], rax
            append(t2_code, mov_imm64(7, 0x20020));       // rdi = 0x20020
            append(t2_code, mov_imm64(0, 0x500a0));       // rax = scePthreadMutexUnlock
            append(t2_code, {0xff, 0xd0});                // call rax
            append(t2_code, {0xc3});                      // ret
            assert(gm.store(0x10300, t2_code.data(), t2_code.size()) == MemoryError::ok);

            // Thread 1 at 0x10200: locks mutex, creates Thread 2, sets [0x20030]=10, unlocks, joins
            std::vector<std::uint8_t> t1_code;
            append(t1_code, mov_imm64(7, 0x20020));       // rdi = 0x20020
            append(t1_code, mov_imm64(0, 0x50080));       // lock
            append(t1_code, {0xff, 0xd0});
            append(t1_code, mov_imm64(7, 0x20018));       // out tid
            append(t1_code, mov_imm64(6, 0));
            append(t1_code, mov_imm64(2, 0x10300));       // entry
            append(t1_code, mov_imm64(1, 0));             // arg
            append(t1_code, mov_imm64(0, 0x50000));       // create
            append(t1_code, {0xff, 0xd0});
            append(t1_code, mov_imm64(3, 0x20030));       // rbx = 0x20030
            append(t1_code, mov_imm64(0, 10));            // rax = 10
            append(t1_code, {0x48, 0x89, 0x03});          // [rbx] = 10
            append(t1_code, mov_imm64(7, 0x20020));       // rdi = 0x20020
            append(t1_code, mov_imm64(0, 0x500a0));       // unlock
            append(t1_code, {0xff, 0xd0});
            append(t1_code, mov_imm64(3, 0x20018));       // rbx = &tid2
            append(t1_code, {0x48, 0x8b, 0x3b});          // rdi = tid2
            append(t1_code, mov_imm64(6, 0));             // rsi = 0
            append(t1_code, mov_imm64(0, 0x50040));       // join
            append(t1_code, {0xff, 0xd0});
            append(t1_code, {0xc3});
            assert(gm.store(0x10200, t1_code.data(), t1_code.size()) == MemoryError::ok);

            assert(gm.map(0x60000, 0x10000, bb::read | bb::write) == MemoryError::ok);
            auto tid = sched.create(0x10200, 0x60000, 0x70000, return_trap);
            assert(tid && sched.start(tid) == ORBIS_OK);

            auto run_res = sched.run(500);
            assert(run_res.stop == GuestScheduler::Stop::idle);
            std::uint64_t final_val = 0;
            assert(gm.load(0x20030, &final_val, 8) == MemoryError::ok && final_val == 11);
        }

        // Test 4: Machine-code Cond Wait and Signal
        {
            // Thread 2 at 0x10500 (signaler): locks mutex 0x20020, sets [0x20040]=42, signals cond 0x20038, unlocks, ret
            std::vector<std::uint8_t> t2_code;
            append(t2_code, mov_imm64(7, 0x20020));       // rdi = &mutex
            append(t2_code, mov_imm64(0, 0x50080));       // lock
            append(t2_code, {0xff, 0xd0});
            append(t2_code, mov_imm64(3, 0x20040));       // rbx = 0x20040
            append(t2_code, mov_imm64(0, 42));            // rax = 42
            append(t2_code, {0x48, 0x89, 0x03});          // [rbx] = 42
            append(t2_code, mov_imm64(7, 0x20038));       // rdi = &cond
            append(t2_code, mov_imm64(0, 0x50100));       // signal
            append(t2_code, {0xff, 0xd0});
            append(t2_code, mov_imm64(7, 0x20020));       // rdi = &mutex
            append(t2_code, mov_imm64(0, 0x500a0));       // unlock
            append(t2_code, {0xff, 0xd0});
            append(t2_code, {0xc3});
            assert(gm.store(0x10500, t2_code.data(), t2_code.size()) == MemoryError::ok);

            // Thread 1 at 0x10400 (waiter): inits cond, locks mutex, creates t2, cond_waits, increments [0x20040], unlocks, joins
            std::vector<std::uint8_t> t1_code;
            append(t1_code, mov_imm64(7, 0x20038));       // rdi = &cond
            append(t1_code, mov_imm64(6, 0));
            append(t1_code, mov_imm64(0, 0x500c0));       // cond_init
            append(t1_code, {0xff, 0xd0});
            append(t1_code, mov_imm64(7, 0x20020));       // rdi = &mutex
            append(t1_code, mov_imm64(0, 0x50080));       // lock
            append(t1_code, {0xff, 0xd0});
            append(t1_code, mov_imm64(7, 0x20018));       // out tid
            append(t1_code, mov_imm64(6, 0));
            append(t1_code, mov_imm64(2, 0x10500));       // entry
            append(t1_code, mov_imm64(1, 0));
            append(t1_code, mov_imm64(0, 0x50000));       // create
            append(t1_code, {0xff, 0xd0});
            append(t1_code, mov_imm64(7, 0x20038));       // cond
            append(t1_code, mov_imm64(6, 0x20020));       // mutex
            append(t1_code, mov_imm64(0, 0x500e0));       // cond_wait
            append(t1_code, {0xff, 0xd0});
            // Woken: add 1 to [0x20040]
            append(t1_code, mov_imm64(3, 0x20040));
            append(t1_code, {0x48, 0x8b, 0x03});
            append(t1_code, {0x48, 0x83, 0xc0, 0x01});
            append(t1_code, {0x48, 0x89, 0x03});
            append(t1_code, mov_imm64(7, 0x20020));       // unlock
            append(t1_code, mov_imm64(0, 0x500a0));
            append(t1_code, {0xff, 0xd0});
            append(t1_code, mov_imm64(3, 0x20018));       // join
            append(t1_code, {0x48, 0x8b, 0x3b});
            append(t1_code, mov_imm64(6, 0));
            append(t1_code, mov_imm64(0, 0x50040));
            append(t1_code, {0xff, 0xd0});
            append(t1_code, {0xc3});
            assert(gm.store(0x10400, t1_code.data(), t1_code.size()) == MemoryError::ok);

            assert(gm.map(0x70000, 0x10000, bb::read | bb::write) == MemoryError::ok);
            auto tid = sched.create(0x10400, 0x70000, 0x80000, return_trap);
            assert(tid && sched.start(tid) == ORBIS_OK);

            auto run_res = sched.run(600);
            assert(run_res.stop == GuestScheduler::Stop::idle);
            std::uint64_t final_val = 0;
            assert(gm.load(0x20040, &final_val, 8) == MemoryError::ok && final_val == 43);
        }

        // Test 5: Machine-code EventFlag Wait and Set
        {
            // Thread 2 at 0x10700: sets bit 4 (pattern 4) on event flag [0x20050], ret
            std::vector<std::uint8_t> t2_code;
            append(t2_code, mov_imm64(3, 0x20050));
            append(t2_code, {0x48, 0x8b, 0x3b});          // rdi = event handle
            append(t2_code, mov_imm64(6, 4));             // rsi = 4
            append(t2_code, mov_imm64(0, 0x50160));       // sceKernelSetEventFlag
            append(t2_code, {0xff, 0xd0});
            append(t2_code, {0xc3});
            assert(gm.store(0x10700, t2_code.data(), t2_code.size()) == MemoryError::ok);

            // Thread 1 at 0x10600: creates event flag, creates t2, waits for pattern 4 (out to 0x20058), joins
            std::vector<std::uint8_t> t1_code;
            append(t1_code, mov_imm64(7, 0x20050));       // &event
            append(t1_code, mov_imm64(6, 0));
            append(t1_code, mov_imm64(2, 0));             // mode 0
            append(t1_code, mov_imm64(1, 0));             // init bits 0
            append(t1_code, mov_imm64(0, 0x50120));       // create
            append(t1_code, {0xff, 0xd0});
            append(t1_code, mov_imm64(7, 0x20018));       // out tid
            append(t1_code, mov_imm64(6, 0));
            append(t1_code, mov_imm64(2, 0x10700));       // entry
            append(t1_code, mov_imm64(1, 0));
            append(t1_code, mov_imm64(0, 0x50000));       // create thread
            append(t1_code, {0xff, 0xd0});
            append(t1_code, mov_imm64(3, 0x20050));
            append(t1_code, {0x48, 0x8b, 0x3b});          // rdi = event handle
            append(t1_code, mov_imm64(6, 4));             // mask 4
            append(t1_code, mov_imm64(2, 0));             // mode 0
            append(t1_code, mov_imm64(1, 0x20058));       // out_pattern
            append(t1_code, mov_imm64(8, 0));             // r8 = 0
            append(t1_code, mov_imm64(0, 0x50140));       // wait
            append(t1_code, {0xff, 0xd0});
            append(t1_code, mov_imm64(3, 0x20018));       // join
            append(t1_code, {0x48, 0x8b, 0x3b});
            append(t1_code, mov_imm64(6, 0));
            append(t1_code, mov_imm64(0, 0x50040));
            append(t1_code, {0xff, 0xd0});
            append(t1_code, {0xc3});
            assert(gm.store(0x10600, t1_code.data(), t1_code.size()) == MemoryError::ok);

            assert(gm.map(0x80000, 0x10000, bb::read | bb::write) == MemoryError::ok);
            auto tid = sched.create(0x10600, 0x80000, 0x90000, return_trap);
            assert(tid && sched.start(tid) == ORBIS_OK);

            auto run_res = sched.run(500);
            assert(run_res.stop == GuestScheduler::Stop::idle);
            std::uint64_t out_pat = 0;
            assert(gm.load(0x20058, &out_pat, 8) == MemoryError::ok && out_pat == 4);
        }

        // Test 6: Machine-code scePthreadOnce via guest trampoline
        {
            // init_func at 0x10900: increments [0x20068], returns with ret
            std::vector<std::uint8_t> init_code;
            append(init_code, mov_imm64(3, 0x20068));
            append(init_code, {0x48, 0x8b, 0x03});
            append(init_code, {0x48, 0x83, 0xc0, 0x01});
            append(init_code, {0x48, 0x89, 0x03});
            append(init_code, {0xc3});
            assert(gm.store(0x10900, init_code.data(), init_code.size()) == MemoryError::ok);

            // Caller at 0x10800: calls scePthreadOnce(&ctrl, &init) twice, ret
            std::vector<std::uint8_t> caller_code;
            append(caller_code, mov_imm64(7, 0x20060));   // rdi = &ctrl
            append(caller_code, mov_imm64(6, 0x10900));   // rsi = init_func
            append(caller_code, mov_imm64(0, 0x50180));   // scePthreadOnce
            append(caller_code, {0xff, 0xd0});
            append(caller_code, mov_imm64(7, 0x20060));   // call again
            append(caller_code, mov_imm64(6, 0x10900));
            append(caller_code, mov_imm64(0, 0x50180));
            append(caller_code, {0xff, 0xd0});
            append(caller_code, {0xc3});
            assert(gm.store(0x10800, caller_code.data(), caller_code.size()) == MemoryError::ok);

            assert(gm.map(0x90000, 0x10000, bb::read | bb::write) == MemoryError::ok);
            auto tid = sched.create(0x10800, 0x90000, 0xa0000, return_trap);
            assert(tid && sched.start(tid) == ORBIS_OK);

            auto run_res = sched.run(500);
            assert(run_res.stop == GuestScheduler::Stop::idle);
            std::uint32_t once_ctrl = 0;
            std::uint64_t counter = 0;
            assert(gm.load(0x20060, &once_ctrl, 4) == MemoryError::ok && once_ctrl == 1);
            assert(gm.load(0x20068, &counter, 8) == MemoryError::ok && counter == 1);
        }

        // Test 7: Machine-code __cxa_guard_acquire e __cxa_guard_release
        {
            // Thread 1 at 0x10a00: acquires guard 0x20078, stores result to [0x20080], releases guard,
            // acquires again, stores second result to [0x20088], ret
            std::vector<std::uint8_t> guard_code;
            append(guard_code, mov_imm64(7, 0x20078));    // &guard
            append(guard_code, mov_imm64(0, 0x501a0));    // __cxa_guard_acquire
            append(guard_code, {0xff, 0xd0});
            append(guard_code, mov_imm64(3, 0x20080));
            append(guard_code, {0x48, 0x89, 0x03});       // store first result
            append(guard_code, mov_imm64(7, 0x20078));
            append(guard_code, mov_imm64(0, 0x501c0));    // __cxa_guard_release
            append(guard_code, {0xff, 0xd0});
            append(guard_code, mov_imm64(7, 0x20078));
            append(guard_code, mov_imm64(0, 0x501a0));    // acquire again
            append(guard_code, {0xff, 0xd0});
            append(guard_code, mov_imm64(3, 0x20088));
            append(guard_code, {0x48, 0x89, 0x03});       // store second result
            append(guard_code, {0xc3});
            assert(gm.store(0x10a00, guard_code.data(), guard_code.size()) == MemoryError::ok);

            assert(gm.map(0xa0000, 0x10000, bb::read | bb::write) == MemoryError::ok);
            auto tid = sched.create(0x10a00, 0xa0000, 0xb0000, return_trap);
            assert(tid && sched.start(tid) == ORBIS_OK);

            auto run_res = sched.run(500);
            assert(run_res.stop == GuestScheduler::Stop::idle);
            std::uint64_t first_res = 99, second_res = 99;
            assert(gm.load(0x20080, &first_res, 8) == MemoryError::ok && first_res == 1);
            assert(gm.load(0x20088, &second_res, 8) == MemoryError::ok && second_res == 0);
        }

        // =====================================================================
        // T071: VFS stat e reachability para caminhos virtuais (/app0/...)
        // =====================================================================
        {
            VirtualFileSystem vfs_test;
            const std::string vtag = unique_tag();
            std::filesystem::path host_app_dir = std::filesystem::temp_directory_path() / ("vfs_app0_test_" + vtag);
            std::filesystem::create_directories(host_app_dir);
            std::filesystem::path host_file = host_app_dir / "sample_asset.bin";
            {
                std::ofstream ofs(host_file, std::ios::binary);
                std::vector<char> dummy(1234, 'X');
                ofs.write(dummy.data(), dummy.size());
            }
            assert(vfs_test.mount("/app0", host_app_dir.string()));
            srt.attach_vfs(&vfs_test);

            // Grava caminho virtual "/app0/sample_asset.bin" e inexistente na guest memory
            constexpr std::uint64_t vpath_addr = 0x20100;
            constexpr std::uint64_t vpath_bad_addr = 0x20150;
            constexpr std::uint64_t stat_out_addr = 0x20200;
            const std::string valid_vpath = "/app0/sample_asset.bin";
            const std::string invalid_vpath = "/app0/no_such_file.bin";
            assert(gm.store(vpath_addr, valid_vpath.c_str(), valid_vpath.size() + 1) == MemoryError::ok);
            assert(gm.store(vpath_bad_addr, invalid_vpath.c_str(), invalid_vpath.size() + 1) == MemoryError::ok);

            // sceKernelCheckReachability
            CpuState reach_cpu{};
            reach_cpu.gpr[7] = vpath_addr; // rdi = path
            assert(srt.invoke_trap("sceKernelCheckReachability", reach_cpu, 1, return_trap).value == 0);
            reach_cpu.gpr[7] = vpath_bad_addr;
            assert(srt.invoke_trap("sceKernelCheckReachability", reach_cpu, 1, return_trap).value == 0x80020002);

            // sceKernelStat
            CpuState kstat_cpu{};
            kstat_cpu.gpr[7] = vpath_addr;     // rdi = path
            kstat_cpu.gpr[6] = stat_out_addr; // rsi = sb
            assert(srt.invoke_trap("sceKernelStat", kstat_cpu, 1, return_trap).value == 0);
            SceKernelStat kstat_res{};
            assert(gm.load(stat_out_addr, &kstat_res, sizeof(kstat_res)) == MemoryError::ok);
            assert(kstat_res.st_size == 1234);

            kstat_cpu.gpr[7] = vpath_bad_addr;
            assert(srt.invoke_trap("sceKernelStat", kstat_cpu, 1, return_trap).value == 0x80020002);

            // libc stat
            CpuState cstat_cpu{};
            cstat_cpu.gpr[7] = vpath_addr;
            cstat_cpu.gpr[6] = stat_out_addr;
            assert(srt.invoke_trap("stat", cstat_cpu, 1, return_trap).value == 0);
            assert(gm.load(stat_out_addr, &kstat_res, sizeof(kstat_res)) == MemoryError::ok);
            assert(kstat_res.st_size == 1234);

            cstat_cpu.gpr[7] = vpath_bad_addr;
            assert(srt.invoke_trap("stat", cstat_cpu, 1, return_trap).value == static_cast<std::uint64_t>(-1));

            std::error_code ec_del;
            std::filesystem::remove_all(host_app_dir, ec_del);
        }

        // =====================================================================
        // T062: Leitura incremental com seek/read_at e footprint limitado
        // =====================================================================
        {
            VirtualFileSystem vfs_stream;
            const std::string tag = unique_tag();
            std::filesystem::path temp_sandbox = std::filesystem::temp_directory_path() / ("vfs_stream_t062_" + tag);
            std::filesystem::create_directories(temp_sandbox);

            // 1. Arquivo esparso de grande escala (> 4 GiB) no host montado em /app0
            constexpr std::uint64_t sparse_magic_offset = (std::uint64_t{1} << 32) + 2048; // 4 GiB + 2048
            const std::string magic_text = "STREAM_CHUNK_T062_OK_4GB!";
            std::filesystem::path sparse_file = temp_sandbox / "sparse_large.dat";
            {
                std::ofstream ofs(sparse_file, std::ios::binary);
                ofs.seekp(static_cast<std::streamoff>(sparse_magic_offset));
                ofs.write(magic_text.data(), magic_text.size());
                assert(ofs.good());
            }

            // 2. Arquivo sintético puro no VFS (sem arquivo correspondente no host)
            // Cria um payload de 128 KiB para validar transição de chunks de 64 KiB
            constexpr std::size_t synth_size = 128 * 1024;
            std::vector<std::uint8_t> synth_payload(synth_size);
            for (std::size_t i = 0; i < synth_size; ++i) {
                synth_payload[i] = static_cast<std::uint8_t>((i * 7 + 13) & 0xff);
            }
            assert(vfs_stream.set_synthetic_file("/app0/synthetic_stream.bin", synth_payload));
            assert(vfs_stream.mount("/app0", temp_sandbox.string()));

            srt.attach_vfs(&vfs_stream);

            auto call_trap = [&](const std::string& name, std::uint64_t a0 = 0, std::uint64_t a1 = 0,
                                 std::uint64_t a2 = 0, std::uint64_t a3 = 0) -> std::uint64_t {
                CpuState c{};
                c.gpr[7] = a0;
                c.gpr[6] = a1;
                c.gpr[2] = a2;
                c.gpr[1] = a3;
                return srt.invoke_trap(name, c, 1, return_trap).value;
            };

            // Aloca buffers guest para caminhos e dados
            constexpr std::uint64_t g_path_synth = 0x24000;
            constexpr std::uint64_t g_path_sparse = 0x24100;
            constexpr std::uint64_t g_path_escape = 0x24200;
            constexpr std::uint64_t g_stat_buf = 0x24300;
            constexpr std::uint64_t g_data_buf = 0x25000;

            const std::string p_synth = "/app0/synthetic_stream.bin";
            const std::string p_sparse = "/app0/sparse_large.dat";
            const std::string p_escape = "/app0/../outside.dat";
            assert(gm.store(g_path_synth, p_synth.c_str(), p_synth.size() + 1) == MemoryError::ok);
            assert(gm.store(g_path_sparse, p_sparse.c_str(), p_sparse.size() + 1) == MemoryError::ok);
            assert(gm.store(g_path_escape, p_escape.c_str(), p_escape.size() + 1) == MemoryError::ok);

            // Rejeição de travessia fora do sandbox
            assert(call_trap("sceKernelOpen", g_path_escape, 0, 0) == 0x80020002); // ENOENT

            // Rejeição de escrita em arquivo sintético VFS (read-only)
            assert(call_trap("sceKernelOpen", g_path_synth, 1 /* O_WRONLY */, 0) == 0x8002001e); // EROFS

            // Teste com arquivo sintético VFS (streaming sem cópia integral no FD)
            std::uint64_t fd_synth = call_trap("sceKernelOpen", g_path_synth, 0 /* O_RDONLY */, 0);
            assert(fd_synth >= 3 && fd_synth < 1000);

            // Fstat do arquivo sintético
            assert(call_trap("sceKernelFstat", fd_synth, g_stat_buf) == 0);
            SceKernelStat stat_synth{};
            assert(gm.load(g_stat_buf, &stat_synth, sizeof(stat_synth)) == MemoryError::ok);
            assert(stat_synth.st_size == static_cast<std::int64_t>(synth_size));

            // Leitura de primeiros 100 bytes via sceKernelRead
            std::vector<std::uint8_t> read_buf(100, 0);
            assert(call_trap("sceKernelRead", fd_synth, g_data_buf, 100) == 100);
            assert(gm.load(g_data_buf, read_buf.data(), 100) == MemoryError::ok);
            assert(std::memcmp(read_buf.data(), synth_payload.data(), 100) == 0);

            // Seek cruzando a fronteira de chunk de 64 KiB: offset 65530
            assert(call_trap("sceKernelLseek", fd_synth, 65530, 0 /* SEEK_SET */) == 65530);
            // Lê 20 bytes cruzando de 65530 a 65550 (cobre byte 65535 e 65536 do chunk seguinte)
            read_buf.assign(20, 0);
            assert(call_trap("sceKernelRead", fd_synth, g_data_buf, 20) == 20);
            assert(gm.load(g_data_buf, read_buf.data(), 20) == MemoryError::ok);
            assert(std::memcmp(read_buf.data(), synth_payload.data() + 65530, 20) == 0);

            // Seek relativo (SEEK_CUR) de -10 bytes (volta para 65540)
            assert(call_trap("sceKernelLseek", fd_synth, static_cast<std::uint64_t>(static_cast<std::int64_t>(-10)), 1 /* SEEK_CUR */) == 65540);

            // Positional read (sceKernelPread) no offset 500 sem alterar o seek_pos corrente (65540)
            read_buf.assign(32, 0);
            assert(call_trap("sceKernelPread", fd_synth, g_data_buf, 32, 500) == 32);
            assert(gm.load(g_data_buf, read_buf.data(), 32) == MemoryError::ok);
            assert(std::memcmp(read_buf.data(), synth_payload.data() + 500, 32) == 0);

            // Confirma que seek_pos continua em 65540 após pread
            assert(call_trap("sceKernelLseek", fd_synth, 0, 1 /* SEEK_CUR */) == 65540);

            // Seek a partir do fim (SEEK_END): -50 bytes
            std::uint64_t expected_pos = synth_size - 50;
            assert(call_trap("sceKernelLseek", fd_synth, static_cast<std::uint64_t>(static_cast<std::int64_t>(-50)), 2 /* SEEK_END */) == expected_pos);

            // Lê 50 bytes restantes até EOF
            read_buf.assign(50, 0);
            assert(call_trap("sceKernelRead", fd_synth, g_data_buf, 50) == 50);
            assert(gm.load(g_data_buf, read_buf.data(), 50) == MemoryError::ok);
            assert(std::memcmp(read_buf.data(), synth_payload.data() + expected_pos, 50) == 0);

            // Leitura subsequente no EOF retorna 0
            assert(call_trap("sceKernelRead", fd_synth, g_data_buf, 10) == 0);

            // Fecha o FD sintético
            assert(call_trap("sceKernelClose", fd_synth) == 0);
            // FD fechado retorna EBADF
            assert(call_trap("sceKernelRead", fd_synth, g_data_buf, 10) == 0x80020009);

            // Teste com arquivo esparso de grande escala (> 4 GiB)
            std::uint64_t fd_sparse = call_trap("sceKernelOpen", g_path_sparse, 0 /* O_RDONLY */, 0);
            assert(fd_sparse >= 3 && fd_sparse < 1000);

            // Fstat do arquivo esparso: st_size > 4 GiB
            assert(call_trap("sceKernelFstat", fd_sparse, g_stat_buf) == 0);
            SceKernelStat stat_sparse{};
            assert(gm.load(g_stat_buf, &stat_sparse, sizeof(stat_sparse)) == MemoryError::ok);
            assert(static_cast<std::uint64_t>(stat_sparse.st_size) >= sparse_magic_offset + magic_text.size());

            // Seek para offset > 4 GiB
            assert(call_trap("sceKernelLseek", fd_sparse, sparse_magic_offset, 0 /* SEEK_SET */) == sparse_magic_offset);

            // Leitura incremental dos bytes mágicos no offset de 4 GiB+
            read_buf.assign(magic_text.size(), 0);
            assert(call_trap("sceKernelRead", fd_sparse, g_data_buf, magic_text.size()) == magic_text.size());
            assert(gm.load(g_data_buf, read_buf.data(), magic_text.size()) == MemoryError::ok);
            assert(std::string(reinterpret_cast<char*>(read_buf.data()), magic_text.size()) == magic_text);

            // Positional read (sceKernelPread) no offset de 4 GiB+
            read_buf.assign(magic_text.size(), 0);
            assert(call_trap("sceKernelPread", fd_sparse, g_data_buf, magic_text.size(), sparse_magic_offset) == magic_text.size());
            assert(gm.load(g_data_buf, read_buf.data(), magic_text.size()) == MemoryError::ok);
            assert(std::string(reinterpret_cast<char*>(read_buf.data()), magic_text.size()) == magic_text);

            // Seek até o fim e confirma retorno de EOF na leitura
            std::uint64_t end_pos = call_trap("sceKernelLseek", fd_sparse, 0, 2 /* SEEK_END */);
            assert(end_pos >= sparse_magic_offset + magic_text.size());
            assert(call_trap("sceKernelRead", fd_sparse, g_data_buf, 16) == 0);

            // Validações de limites e erros
            // Seek inválido com valor negativo absoluto
            assert(call_trap("sceKernelLseek", fd_sparse, static_cast<std::uint64_t>(static_cast<std::int64_t>(-100)), 0 /* SEEK_SET */) == 0x80020016);
            // Buffer nulo com nbyte > 0
            assert(call_trap("sceKernelRead", fd_sparse, 0, 16) == 0x80020016);
            assert(call_trap("sceKernelPread", fd_sparse, 0, 16, sparse_magic_offset) == 0x80020016);

            // Fecha arquivo esparso
            assert(call_trap("sceKernelClose", fd_sparse) == 0);

            // =================================================================
            // T085: Leitura de entrada BND4 via FD do kernel sem materializar o pacote
            // =================================================================
            {
                std::vector<std::uint8_t> bnd_raw(260, 0);
                std::memcpy(bnd_raw.data(), "BND4", 4);
                auto w32 = [](std::uint8_t *p, std::uint32_t v) {
                    p[0] = static_cast<std::uint8_t>(v & 0xFF);
                    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
                    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
                    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
                };
                auto w64 = [&](std::uint8_t *p, std::uint64_t v) {
                    w32(p, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
                    w32(p + 4, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
                };
                w32(bnd_raw.data() + 12, 1);
                w64(bnd_raw.data() + 16, 64);
                std::memcpy(bnd_raw.data() + 24, "07D7R6\0\0", 8);
                w64(bnd_raw.data() + 32, 36);
                bnd_raw[0x28] = 2;

                constexpr std::size_t bnd_file_size = 50;
                w64(bnd_raw.data() + 72, bnd_file_size);
                w64(bnd_raw.data() + 80, bnd_file_size);
                w32(bnd_raw.data() + 88, 140);
                w32(bnd_raw.data() + 92, 100);
                w32(bnd_raw.data() + 96, 210);

                std::vector<std::uint8_t> expected_bnd_payload(bnd_file_size);
                for (std::size_t i = 0; i < bnd_file_size; ++i) {
                    expected_bnd_payload[i] = static_cast<std::uint8_t>(i + 11);
                    bnd_raw[140 + i] = expected_bnd_payload[i];
                }
                std::memcpy(bnd_raw.data() + 210, "data_bnd.bin", 13);

                auto bnd_reader = std::make_shared<Bnd4Reader>();
                assert(bnd_reader->load(bnd_raw.data(), bnd_raw.size()) == BinderError::ok);
                assert(vfs_stream.mount_bnd4("/bnd_archive", bnd_reader));

                std::string path_bnd_str = "/bnd_archive/data_bnd.bin";
                constexpr std::uint64_t g_path_bnd = 0x82000;
                assert(gm.store(g_path_bnd, path_bnd_str.c_str(), path_bnd_str.size() + 1) == MemoryError::ok);

                // Abre o arquivo do BND4 via sceKernelOpen (FD VFS stream)
                std::uint64_t fd_bnd = call_trap("sceKernelOpen", g_path_bnd, 0 /* O_RDONLY */, 0);
                assert(fd_bnd >= 3 && fd_bnd < 1000);

                // Fstat verifica tamanho do arquivo dentro do pacote
                assert(call_trap("sceKernelFstat", fd_bnd, g_stat_buf) == 0);
                SceKernelStat stat_bnd{};
                assert(gm.load(g_stat_buf, &stat_bnd, sizeof(stat_bnd)) == MemoryError::ok);
                assert(stat_bnd.st_size == static_cast<std::int64_t>(bnd_file_size));

                // Leitura dos primeiros 20 bytes via sceKernelRead
                read_buf.assign(20, 0);
                assert(call_trap("sceKernelRead", fd_bnd, g_data_buf, 20) == 20);
                assert(gm.load(g_data_buf, read_buf.data(), 20) == MemoryError::ok);
                assert(std::memcmp(read_buf.data(), expected_bnd_payload.data(), 20) == 0);

                // Leitura posicional via sceKernelPread (offset 30, tamanho 15) sem alterar o seek_pos (que está em 20)
                read_buf.assign(15, 0);
                assert(call_trap("sceKernelPread", fd_bnd, g_data_buf, 15, 30) == 15);
                assert(gm.load(g_data_buf, read_buf.data(), 15) == MemoryError::ok);
                assert(std::memcmp(read_buf.data(), expected_bnd_payload.data() + 30, 15) == 0);

                // Continua lendo a partir de seek_pos (20) os 30 bytes restantes até o EOF
                read_buf.assign(30, 0);
                assert(call_trap("sceKernelRead", fd_bnd, g_data_buf, 30) == 30);
                assert(gm.load(g_data_buf, read_buf.data(), 30) == MemoryError::ok);
                assert(std::memcmp(read_buf.data(), expected_bnd_payload.data() + 20, 30) == 0);

                // Leitura subsequente no EOF retorna 0
                assert(call_trap("sceKernelRead", fd_bnd, g_data_buf, 10) == 0);

                // Fecha o FD
                assert(call_trap("sceKernelClose", fd_bnd) == 0);
            }

            srt.attach_vfs(nullptr);

            std::error_code ec_rem;
            std::filesystem::remove_all(temp_sandbox, ec_rem);
        }

        // =====================================================================
        // T074: Destruição HLE de primitivas com waiters acordados com ECANCELED
        // =====================================================================
        {
            assert(gm.map(0xb0000, 0x10000, bb::read | bb::write) == MemoryError::ok);
            assert(gm.map(0xc0000, 0x10000, bb::read | bb::write) == MemoryError::ok);
            constexpr std::uint64_t m_addr = 0x20300;

            CpuState init_cpu{};
            init_cpu.gpr[7] = m_addr;
            init_cpu.gpr[6] = 0;
            assert(srt.invoke_trap("scePthreadMutexInit", init_cpu, 1, return_trap).value == 0);

            auto t_blocker = sched.create(0x10000, 0xb0000, 0xc0000, return_trap);
            auto t_waiter = sched.create(0x10000, 0xc0000, 0xd0000, return_trap);
            assert(t_blocker && t_waiter);
            assert(sched.start(t_blocker) == ORBIS_OK && sched.start(t_waiter) == ORBIS_OK);

            CpuState lock_cpu = sched.cpu(t_blocker);
            lock_cpu.gpr[7] = m_addr;
            assert(srt.invoke_trap("scePthreadMutexLock", lock_cpu, t_blocker, return_trap).value == 0);

            CpuState wait_cpu = sched.cpu(t_waiter);
            wait_cpu.gpr[7] = m_addr;
            auto wait_res = srt.invoke_trap("scePthreadMutexLock", wait_cpu, t_waiter, return_trap);
            assert(wait_res.action == OrbisRuntime::InvocationResult::Action::blocked);
            assert(sched.state(t_waiter) == GuestScheduler::State::waiting);

            // Destrói o mutex via HLE scePthreadMutexDestroy
            CpuState destroy_cpu = sched.cpu(t_blocker);
            destroy_cpu.gpr[7] = m_addr;
            assert(srt.invoke_trap("scePthreadMutexDestroy", destroy_cpu, t_blocker, return_trap).value == 0);
            assert(sched.state(t_waiter) == GuestScheduler::State::ready);
            assert(sched.resume(t_waiter) == ORBIS_KERNEL_ERROR_ECANCELED);
        }

        // =====================================================================
        // T076: sceKernelMunmap parcial com divisão de memória mapeada
        // =====================================================================
        {
            constexpr std::uint64_t direct_alloc_addr = 0x20400;
            constexpr std::uint64_t direct_map_addr = 0x20408;

            CpuState dmem_cpu{};
            dmem_cpu.gpr[7] = 0;
            dmem_cpu.gpr[6] = 0x100000;
            dmem_cpu.gpr[2] = 0x8000;
            dmem_cpu.gpr[1] = 0x8000;
            dmem_cpu.gpr[8] = 0;
            dmem_cpu.gpr[9] = direct_alloc_addr;
            assert(srt.invoke_trap("sceKernelAllocateDirectMemory", dmem_cpu, 1, return_trap).value == 0);

            std::uint64_t phys_part = 0;
            assert(gm.load(direct_alloc_addr, &phys_part, 8) == MemoryError::ok);

            dmem_cpu.gpr[7] = direct_map_addr;
            dmem_cpu.gpr[6] = 0x8000;
            dmem_cpu.gpr[2] = 3;
            dmem_cpu.gpr[1] = 0;
            dmem_cpu.gpr[8] = phys_part;
            dmem_cpu.gpr[9] = 0;
            assert(srt.invoke_trap("sceKernelMapDirectMemory", dmem_cpu, 1, return_trap).value == 0);

            std::uint64_t mapped_addr = 0;
            assert(gm.load(direct_map_addr, &mapped_addr, 8) == MemoryError::ok && mapped_addr > 0);

            // Escreve bytes em toda a extensão mapeada (0x8000 bytes)
            std::uint8_t dummy_b = 0x77;
            assert(gm.store(mapped_addr, &dummy_b, 1) == MemoryError::ok);
            assert(gm.store(mapped_addr + 0x4000, &dummy_b, 1) == MemoryError::ok);

            // Unmap parcial: apenas os primeiros 0x4000 bytes
            CpuState unmap_cpu{};
            unmap_cpu.gpr[7] = mapped_addr;
            unmap_cpu.gpr[6] = 0x4000;
            assert(srt.invoke_trap("sceKernelMunmap", unmap_cpu, 1, return_trap).value == 0);

            // Primeiro bloco foi desmapeado
            std::uint8_t read_b = 0;
            assert(gm.load(mapped_addr, &read_b, 1) == MemoryError::unmapped);

            // Segundo bloco continua mapeado e acessível
            assert(gm.load(mapped_addr + 0x4000, &read_b, 1) == MemoryError::ok && read_b == 0x77);

            // A metade restante mantém a associação física e impede liberação prematura.
            CpuState rel_cpu{};
            rel_cpu.gpr[7] = phys_part;
            rel_cpu.gpr[6] = 0x8000;
            assert(srt.invoke_trap("sceKernelReleaseDirectMemory", rel_cpu, 1, return_trap).value == 0x80020010);
            // Uma faixa já liberada não pode retornar sucesso nem alterar a metade restante.
            assert(srt.invoke_trap("sceKernelMunmap", unmap_cpu, 1, return_trap).value != 0);
            assert(gm.load(mapped_addr + 0x4000, &read_b, 1) == MemoryError::ok);

            // Desmapeia a segunda metade
            unmap_cpu.gpr[7] = mapped_addr + 0x4000;
            unmap_cpu.gpr[6] = 0x4000;
            assert(srt.invoke_trap("sceKernelMunmap", unmap_cpu, 1, return_trap).value == 0);
            assert(gm.load(mapped_addr + 0x4000, &read_b, 1) == MemoryError::unmapped);

            // Libera memória direta somente após desmapear todas as partes.
            assert(srt.invoke_trap("sceKernelReleaseDirectMemory", rel_cpu, 1, return_trap).value == 0);

            // T084: dividir uma associação direta no meio conserva ambas as metades.
            dmem_cpu.gpr[7] = 0;
            dmem_cpu.gpr[6] = 0x100000;
            dmem_cpu.gpr[2] = 0xc000;
            dmem_cpu.gpr[1] = 0x4000;
            dmem_cpu.gpr[9] = direct_alloc_addr;
            assert(srt.invoke_trap("sceKernelAllocateDirectMemory", dmem_cpu, 1, return_trap).value == 0);
            assert(gm.load(direct_alloc_addr, &phys_part, 8) == MemoryError::ok);
            dmem_cpu.gpr[7] = direct_map_addr;
            dmem_cpu.gpr[6] = 0xc000;
            dmem_cpu.gpr[2] = 3;
            dmem_cpu.gpr[1] = 0;
            dmem_cpu.gpr[8] = phys_part;
            dmem_cpu.gpr[9] = 0;
            assert(srt.invoke_trap("sceKernelMapDirectMemory", dmem_cpu, 1, return_trap).value == 0);
            assert(gm.load(direct_map_addr, &mapped_addr, 8) == MemoryError::ok);
            unmap_cpu.gpr[7] = mapped_addr + 0x4000;
            assert(srt.invoke_trap("sceKernelMunmap", unmap_cpu, 1, return_trap).value == 0);
            assert(gm.load(mapped_addr + 0x4000, &read_b, 1) == MemoryError::unmapped);
            assert(gm.store(mapped_addr, &dummy_b, 1) == MemoryError::ok);
            assert(gm.store(mapped_addr + 0x8000, &dummy_b, 1) == MemoryError::ok);
            rel_cpu.gpr[7] = phys_part;
            rel_cpu.gpr[6] = 0xc000;
            assert(srt.invoke_trap("sceKernelReleaseDirectMemory", rel_cpu, 1, return_trap).value == 0x80020010);
            unmap_cpu.gpr[7] = mapped_addr;
            assert(srt.invoke_trap("sceKernelMunmap", unmap_cpu, 1, return_trap).value == 0);
            assert(srt.invoke_trap("sceKernelReleaseDirectMemory", rel_cpu, 1, return_trap).value == 0x80020010);
            unmap_cpu.gpr[7] = mapped_addr + 0x8000;
            assert(srt.invoke_trap("sceKernelMunmap", unmap_cpu, 1, return_trap).value == 0);
            assert(srt.invoke_trap("sceKernelReleaseDirectMemory", rel_cpu, 1, return_trap).value == 0);
        }

        // =====================================================================
        // T052: Inicialização de TLS de módulos e DTV para threads secundárias
        // =====================================================================
        {
            // Registra template TLS de módulo secundário (module_id = 2)
            constexpr std::uint64_t mod_tls_template_addr = 0x20500;
            const std::uint8_t mod_tls_data[4] = {0x11, 0x22, 0x33, 0x44};
            assert(gm.store(mod_tls_template_addr, mod_tls_data, sizeof(mod_tls_data)) == MemoryError::ok);

            Ps4Tls mod2_tls{};
            mod2_tls.address = mod_tls_template_addr;
            mod2_tls.filesz = sizeof(mod_tls_data);
            mod2_tls.memsz = 64;
            mod2_tls.align = 16;
            mod2_tls.module_id = 2;
            srt.add_module_tls_template(mod2_tls);

            constexpr std::uint64_t t2_entry = 0x10b00;
            std::vector<std::uint8_t> t2_code;
            append(t2_code, mov_imm64(0, 0x42));
            append(t2_code, {0xc3});
            assert(gm.store(t2_entry, t2_code.data(), t2_code.size()) == MemoryError::ok);

            constexpr std::uint64_t tid_addr = 0x20510;
            CpuState create_cpu{};
            create_cpu.gpr[7] = tid_addr;      // rdi = &tid
            create_cpu.gpr[6] = 0;             // rsi = attr
            create_cpu.gpr[2] = t2_entry;      // rdx = entry
            create_cpu.gpr[1] = 0;             // rcx = arg
            auto create_res = srt.invoke_trap("scePthreadCreate", create_cpu, 1, return_trap);
            assert(create_res.value == 0);

            std::uint64_t new_tid = 0;
            assert(gm.load(tid_addr, &new_tid, 8) == MemoryError::ok && new_tid > 0);
            auto new_tcb = sched.cpu(new_tid).fs_base;
            assert(new_tcb > 0);

            // Valida estrutura TCB e DTV da thread criada
            std::uint64_t tcb_self = 0, tcb_dtv = 0;
            assert(gm.load(new_tcb, &tcb_self, 8) == MemoryError::ok && tcb_self == new_tcb);
            assert(gm.load(new_tcb + 8, &tcb_dtv, 8) == MemoryError::ok && tcb_dtv == new_tcb + 64);

            std::uint64_t dtv_count = 0, dtv_mod1 = 0, dtv_mod2 = 0;
            assert(gm.load(tcb_dtv, &dtv_count, 8) == MemoryError::ok && dtv_count >= 3);
            assert(gm.load(tcb_dtv + 8, &dtv_mod1, 8) == MemoryError::ok && dtv_mod1 > 0);
            assert(gm.load(tcb_dtv + 16, &dtv_mod2, 8) == MemoryError::ok && dtv_mod2 > 0);

            // Valida que os dados do template de módulo foram copiados para a área de TLS do módulo na thread
            std::uint8_t read_tls[4] = {};
            assert(gm.load(dtv_mod2, read_tls, 4) == MemoryError::ok);
            assert(read_tls[0] == 0x11 && read_tls[1] == 0x22 && read_tls[2] == 0x33 && read_tls[3] == 0x44);
        }
    }

    // =========================================================================
    // T061: Equeue, Flip Events por vídeo handle, espera cooperativa e timeout
    // =========================================================================
    {
        GuestMemory gm_eq(2 * 1024 * 1024);
        assert(gm_eq.map(0x1000, 0x20000, bb::read | bb::write) == MemoryError::ok);
        OrbisRuntime rt_eq(gm_eq);
        X86Interpreter cpu_eq(gm_eq);
        cpu_eq.add_trap_range(0x3000, 32);

        auto call_eq = [&](const std::string& name, std::uint64_t x = 0, std::uint64_t y = 0, std::uint64_t z = 0,
                           std::uint64_t w = 0, std::uint64_t v = 0, std::uint64_t sixth = 0) {
            std::uint64_t trap = 0x3000;
            const std::uint8_t program[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0xf4};
            assert(gm_eq.store(0x1000, program, sizeof(program)) == MemoryError::ok);
            assert(gm_eq.store(0x1002, &trap, 8) == MemoryError::ok);
            cpu_eq.state = {};
            cpu_eq.state.rip = 0x1000;
            cpu_eq.state.gpr[4] = 0x2ff0;
            cpu_eq.state.gpr[7] = x;
            cpu_eq.state.gpr[6] = y;
            cpu_eq.state.gpr[2] = z;
            cpu_eq.state.gpr[1] = w;
            cpu_eq.state.gpr[8] = v;
            cpu_eq.state.gpr[9] = sixth;
            auto r = cpu_eq.run(10);
            assert(r.stop == CpuStop::trap && r.rip == 0x3000);
            auto result = rt_eq.invoke(name, cpu_eq.state);
            std::uint64_t back = 0;
            assert(gm_eq.load(cpu_eq.state.gpr[4], &back, 8) == MemoryError::ok);
            cpu_eq.state.gpr[4] += 8;
            cpu_eq.state.rip = back;
            cpu_eq.state.gpr[0] = result;
            r = cpu_eq.run(10);
            assert(r.stop == CpuStop::halt);
            return result;
        };

        // 1. Criação e validação de parâmetros
        constexpr std::uint64_t eq1_out = 0x10000;
        constexpr std::uint64_t eq2_out = 0x10008;
        constexpr std::uint64_t eq_name_addr = 0x10010;
        assert(gm_eq.store(eq_name_addr, "flip_queue", 11) == MemoryError::ok);

        assert(call_eq("sceKernelCreateEqueue", 0, eq_name_addr) == 0x80020016); // EINVAL
        assert(call_eq("sceKernelCreateEqueue", eq1_out, eq_name_addr) == 0);
        assert(call_eq("sceKernelCreateEqueue", eq2_out, eq_name_addr) == 0);

        std::uint32_t eq1_id = 0, eq2_id = 0;
        assert(gm_eq.load(eq1_out, &eq1_id, 4) == MemoryError::ok && eq1_id >= 100);
        assert(gm_eq.load(eq2_out, &eq2_id, 4) == MemoryError::ok && eq2_id > eq1_id);

        constexpr std::uint64_t ev_buf_addr = 0x11000;
        constexpr std::uint64_t out_cnt_addr = 0x11200;
        constexpr std::uint64_t timeout_addr = 0x11208;

        // Fila inexistente
        assert(call_eq("sceKernelWaitEqueue", 99999, ev_buf_addr, 1, out_cnt_addr, 0) == 0x80020014); // EBADF
        // Parâmetros inválidos
        assert(call_eq("sceKernelWaitEqueue", eq1_id, 0, 1, out_cnt_addr, 0) == 0x80020016); // EINVAL
        assert(call_eq("sceKernelWaitEqueue", eq1_id, ev_buf_addr, 0, out_cnt_addr, 0) == 0x80020016); // EINVAL

        // 2. Timeout zero: polling não bloqueante em fila vazia
        std::uint32_t zero_timeout = 0;
        assert(gm_eq.store(timeout_addr, &zero_timeout, 4) == MemoryError::ok);
        std::int32_t out_cnt = -1;
        assert(gm_eq.store(out_cnt_addr, &out_cnt, 4) == MemoryError::ok);
        assert(call_eq("sceKernelWaitEqueue", eq1_id, ev_buf_addr, 1, out_cnt_addr, timeout_addr) == 0x8002001a); // ETIMEDOUT
        assert(gm_eq.load(out_cnt_addr, &out_cnt, 4) == MemoryError::ok && out_cnt == 0);

        // 3. Timeout cooperativo (5 ms) em fila vazia
        std::uint32_t timeout_5ms = 5000; // 5000 micros = 5ms
        assert(gm_eq.store(timeout_addr, &timeout_5ms, 4) == MemoryError::ok);
        auto t_start = std::chrono::steady_clock::now();
        assert(call_eq("sceKernelWaitEqueue", eq1_id, ev_buf_addr, 1, out_cnt_addr, timeout_addr) == 0x8002001a); // ETIMEDOUT
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_start).count();
        assert(elapsed_ms >= 3); // Comprovando espera cooperativa sem busy-polling imediato
        assert(gm_eq.load(out_cnt_addr, &out_cnt, 4) == MemoryError::ok && out_cnt == 0);

        // 4. Múltiplas filas e direcionamento por vídeo handle
        constexpr std::int32_t vh1 = 1001;
        constexpr std::int32_t vh2 = 1002;
        constexpr std::uint64_t udata1 = 0xAAAA1111ULL;
        constexpr std::uint64_t udata2 = 0xBBBB2222ULL;

        // eq1 registrado para vh1
        assert(call_eq("sceVideoOutAddFlipEvent", eq1_id, vh1, udata1) == 0);
        // eq2 registrado para vh2
        assert(call_eq("sceVideoOutAddFlipEvent", eq2_id, vh2, udata2) == 0);

        // Dispara flip no videoHandle 1001
        rt_eq.dispatch_flip_event(vh1, 0);

        // eq1 deve conter 1 evento
        assert(call_eq("sceKernelWaitEqueue", eq1_id, ev_buf_addr, 1, out_cnt_addr, timeout_addr) == 0);
        assert(gm_eq.load(out_cnt_addr, &out_cnt, 4) == MemoryError::ok && out_cnt == 1);
        struct TestKEvent {
            std::uint64_t ident;
            std::int16_t filter;
            std::uint16_t flags;
            std::uint32_t fflags;
            std::uint64_t data;
            std::uint64_t udata;
        } ev{};
        assert(gm_eq.load(ev_buf_addr, &ev, sizeof(ev)) == MemoryError::ok);
        assert(ev.ident == static_cast<std::uint64_t>(vh1));
        assert(ev.filter == -13);
        assert(ev.udata == udata1);

        // eq2 NÃO deve ter recebido o evento de vh1 (continua vazia -> timeout)
        assert(call_eq("sceKernelWaitEqueue", eq2_id, ev_buf_addr, 1, out_cnt_addr, timeout_addr) == 0x8002001a);

        // Dispara flip no videoHandle 1002
        rt_eq.dispatch_flip_event(vh2, 1);

        // eq1 NÃO recebe de vh2
        assert(call_eq("sceKernelWaitEqueue", eq1_id, ev_buf_addr, 1, out_cnt_addr, timeout_addr) == 0x8002001a);

        // eq2 recebe de vh2
        assert(call_eq("sceKernelWaitEqueue", eq2_id, ev_buf_addr, 1, out_cnt_addr, timeout_addr) == 0);
        assert(gm_eq.load(out_cnt_addr, &out_cnt, 4) == MemoryError::ok && out_cnt == 1);
        assert(gm_eq.load(ev_buf_addr, &ev, sizeof(ev)) == MemoryError::ok);
        assert(ev.ident == static_cast<std::uint64_t>(vh2));
        assert(ev.filter == -13);
        assert(ev.udata == udata2);

        // 5. Produtor-consumidor concorrente com espera cooperativa
        std::atomic<bool> consumer_started{false};
        std::atomic<std::uint64_t> consumer_res{0};
        std::atomic<int> consumer_events{0};

        std::thread consumer_thread([&]() {
            CpuState c_cpu{};
            c_cpu.gpr[7] = eq1_id;          // rdi = eq
            c_cpu.gpr[6] = ev_buf_addr;     // rsi = ev
            c_cpu.gpr[2] = 1;               // rdx = num
            c_cpu.gpr[1] = out_cnt_addr;    // rcx = out
            c_cpu.gpr[8] = 0;               // r8 = nullptr (espera infinita cooperativa)
            consumer_started = true;
            auto res = rt_eq.invoke("sceKernelWaitEqueue", c_cpu);
            consumer_res = res;
            std::int32_t got = 0;
            gm_eq.load(out_cnt_addr, &got, 4);
            consumer_events = got;
        });

        while (!consumer_started) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // Produtor envia flip event em vh1
        rt_eq.dispatch_flip_event(vh1, 0);

        consumer_thread.join();
        assert(consumer_res == 0);
        assert(consumer_events == 1);

        // 6. Desregistro de flip event via sceVideoOutDeleteFlipEvent
        assert(call_eq("sceVideoOutDeleteFlipEvent", eq1_id, vh1) == 0);
        rt_eq.dispatch_flip_event(vh1, 0);
        assert(call_eq("sceKernelWaitEqueue", eq1_id, ev_buf_addr, 1, out_cnt_addr, timeout_addr) == 0x8002001a);

        // 7. Exclusão de fila com thread em espera cooperativa (desbloqueio com EBADF)
        std::atomic<bool> del_waiter_ready{false};
        std::atomic<std::uint64_t> del_waiter_res{0};

        std::thread del_waiter_thread([&]() {
            CpuState c_cpu{};
            c_cpu.gpr[7] = eq1_id;
            c_cpu.gpr[6] = ev_buf_addr;
            c_cpu.gpr[2] = 1;
            c_cpu.gpr[1] = out_cnt_addr;
            c_cpu.gpr[8] = 0; // Espera infinita
            del_waiter_ready = true;
            auto res = rt_eq.invoke("sceKernelWaitEqueue", c_cpu);
            del_waiter_res = res;
        });

        while (!del_waiter_ready) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));

        // Destrói a fila enquanto a thread está esperando
        assert(call_eq("sceKernelDeleteEqueue", eq1_id) == 0);

        del_waiter_thread.join();
        assert(del_waiter_res == 0x80020014); // EBADF

        // Limpa eq2
        assert(call_eq("sceKernelDeleteEqueue", eq2_id) == 0);
    }

    // T064: Verificação de relógio do processo e tempo monotônico integrados ao escalonador
    {
        GuestMemory mem(1 << 20);
        GuestScheduler sched(mem);
        OrbisRuntime rt(mem);
        rt.attach_scheduler(sched);

        sched.set_clock_mode(GuestScheduler::ClockMode::Decoupled);
        sched.advance(42000); // 42 ms virtuais

        CpuState cpu{};
        auto proc_time = rt.invoke("sceKernelGetProcessTime", cpu);
        assert(proc_time == 42000);

        // clock_gettime(CLOCK_MONOTONIC, &ts)
        auto ts_addr = 0x50000ULL;
        mem.map(ts_addr, 0x1000, read | write);
        cpu.gpr[7] = 1; // CLOCK_MONOTONIC
        cpu.gpr[6] = ts_addr;
        assert(rt.invoke("clock_gettime", cpu) == 0);

        std::int64_t sec = 0, nsec = 0;
        assert(mem.load(ts_addr, &sec, 8) == MemoryError::ok);
        assert(mem.load(ts_addr + 8, &nsec, 8) == MemoryError::ok);
        assert(sec == 0);
        assert(nsec == 42000000LL); // 42 ms em nanosegundos
    }

    // =========================================================================
    // Phase 26: T116 (O_WRONLY sem O_TRUNC), T119 (VFS precedence), T121 (ftruncate)
    // =========================================================================
    {
        GuestMemory mem(2 * 1024 * 1024);
        assert(mem.map(0x1000, 0x30000, read | write) == MemoryError::ok);
        OrbisRuntime rt(mem);

        VirtualFileSystem vfs;
        const std::string tag = unique_tag();
        std::filesystem::path test_dir = std::filesystem::temp_directory_path() / ("orbis_p26_" + tag);
        std::error_code ec_setup;
        std::filesystem::create_directories(test_dir, ec_setup);
        assert(vfs.mount("/test", test_dir.string(), false));
        rt.attach_vfs(&vfs);

        auto call = [&](const std::string& name, std::uint64_t a0 = 0, std::uint64_t a1 = 0,
                        std::uint64_t a2 = 0, std::uint64_t a3 = 0) -> std::uint64_t {
            CpuState c{};
            c.gpr[7] = a0;
            c.gpr[6] = a1;
            c.gpr[2] = a2;
            c.gpr[1] = a3;
            return rt.invoke(name, c);
        };

        // 1. T116: Evitar truncamento com O_WRONLY sem O_TRUNC
        std::filesystem::path host_file = test_dir / "preserve_test.bin";
        {
            std::ofstream ofs(host_file, std::ios::binary);
            ofs << "0123456789ABCDEF"; // 16 bytes
        }

        constexpr std::uint64_t g_path = 0x2000;
        constexpr std::uint64_t g_buf = 0x2100;
        const std::string v_path = "/test/preserve_test.bin";
        assert(mem.store(g_path, v_path.c_str(), v_path.size() + 1) == MemoryError::ok);

        // Abre com O_WRONLY (0x0001) sem O_TRUNC (0x0400), sem O_CREAT (0x0200)
        auto fd = call("sceKernelOpen", g_path, 0x0001 /* O_WRONLY */, 0);
        assert(static_cast<std::int32_t>(fd) >= 0 && fd < 0x80000000ULL);

        // Escreve 4 bytes "XXXX" no início
        assert(mem.store(g_buf, "XXXX", 4) == MemoryError::ok);
        assert(call("sceKernelWrite", fd, g_buf, 4) == 4);
        assert(call("sceKernelClose", fd) == 0);

        // Verifica que bytes 4..15 foram preservados ("XXXX456789ABCDEF")
        {
            std::ifstream ifs(host_file, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            assert(content == "XXXX456789ABCDEF");
            assert(content.size() == 16);
        }

        // Abre com O_WRONLY | O_CREAT (0x0201) sem O_TRUNC
        fd = call("sceKernelOpen", g_path, 0x0201, 0);
        assert(static_cast<std::int32_t>(fd) >= 0 && fd < 0x80000000ULL);
        assert(mem.store(g_buf, "YYYY", 4) == MemoryError::ok);
        assert(call("sceKernelWrite", fd, g_buf, 4) == 4);
        assert(call("sceKernelClose", fd) == 0);

        {
            std::ifstream ifs(host_file, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            assert(content == "YYYY456789ABCDEF");
            assert(content.size() == 16);
        }

        // Agora abre com O_WRONLY | O_TRUNC (0x0401) -> deve truncar
        fd = call("sceKernelOpen", g_path, 0x0401, 0);
        assert(static_cast<std::int32_t>(fd) >= 0 && fd < 0x80000000ULL);
        assert(mem.store(g_buf, "TRUNC", 5) == MemoryError::ok);
        assert(call("sceKernelWrite", fd, g_buf, 5) == 5);
        assert(call("sceKernelClose", fd) == 0);

        {
            std::ifstream ifs(host_file, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            assert(content == "TRUNC");
            assert(content.size() == 5);
        }

        // 2. T119: Precedência de arquivo virtual/arquivo montado sobre arquivo host divergente
        std::filesystem::path host_override_file = test_dir / "override.bin";
        {
            std::ofstream ofs(host_override_file, std::ios::binary);
            ofs << "HOST_DIVERGENT_CONTENT";
        }
        std::vector<std::uint8_t> bnd_raw(260, 0);
        std::memcpy(bnd_raw.data(), "BND4", 4);
        auto w32 = [](std::uint8_t *p, std::uint32_t v) {
            p[0] = static_cast<std::uint8_t>(v & 0xFF);
            p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
            p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
            p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
        };
        auto w64 = [&](std::uint8_t *p, std::uint64_t v) {
            w32(p, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
            w32(p + 4, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
        };
        w32(bnd_raw.data() + 12, 1);
        w64(bnd_raw.data() + 16, 64);
        std::memcpy(bnd_raw.data() + 24, "07D7R6\0\0", 8);
        w64(bnd_raw.data() + 32, 36);
        bnd_raw[0x28] = 2;

        const std::string archive_content = "ARCHIVE_OK";
        const std::size_t bnd_file_size = archive_content.size();
        w64(bnd_raw.data() + 72, bnd_file_size);
        w64(bnd_raw.data() + 80, bnd_file_size);
        w32(bnd_raw.data() + 88, 140);
        w32(bnd_raw.data() + 92, 100);
        w32(bnd_raw.data() + 96, 210);

        std::memcpy(bnd_raw.data() + 140, archive_content.data(), archive_content.size());
        std::memcpy(bnd_raw.data() + 210, "override.bin", 13);

        auto bnd_reader = std::make_shared<Bnd4Reader>();
        assert(bnd_reader->load(bnd_raw.data(), bnd_raw.size()) == BinderError::ok);
        assert(vfs.mount_bnd4("/test", bnd_reader));

        const std::string v_override = "/test/override.bin";
        assert(mem.store(g_path, v_override.c_str(), v_override.size() + 1) == MemoryError::ok);

        auto fd_override = call("sceKernelOpen", g_path, 0 /* O_RDONLY */, 0);
        assert(static_cast<std::int32_t>(fd_override) >= 0 && fd_override < 0x80000000ULL);

        char read_synth[32] = {};
        assert(call("sceKernelRead", fd_override, g_buf, sizeof(read_synth)) == archive_content.size());
        assert(mem.load(g_buf, read_synth, archive_content.size()) == MemoryError::ok);
        assert(std::string(read_synth, archive_content.size()) == "ARCHIVE_OK");
        assert(call("sceKernelClose", fd_override) == 0);

        // 3. T121: Erro real em ftruncate / sceKernelFtruncate
        // FD somente-leitura deve falhar
        fd = call("sceKernelOpen", g_path, 0 /* O_RDONLY */, 0);
        assert(static_cast<std::int32_t>(fd) >= 0 && fd < 0x80000000ULL);
        assert(call("sceKernelFtruncate", fd, 10) == 0x80020016); // EINVAL
        assert(call("ftruncate", fd, 10) == static_cast<std::uint64_t>(-1));
        assert(call("sceKernelClose", fd) == 0);

        // FD inválido deve falhar com EBADF
        assert(call("sceKernelFtruncate", 9999, 10) == 0x80020009); // EBADF
        assert(call("ftruncate", 9999, 10) == static_cast<std::uint64_t>(-1));

        // Truncamento em arquivo gravável
        std::filesystem::path trunc_file = test_dir / "trunc_target.bin";
        {
            std::ofstream ofs(trunc_file, std::ios::binary);
            ofs << "01234567890123456789"; // 20 bytes
        }
        const std::string v_trunc = "/test/trunc_target.bin";
        assert(mem.store(g_path, v_trunc.c_str(), v_trunc.size() + 1) == MemoryError::ok);
        fd = call("sceKernelOpen", g_path, 0x0002 /* O_RDWR */, 0);
        assert(static_cast<std::int32_t>(fd) >= 0 && fd < 0x80000000ULL);

        // Comprimento negativo deve falhar com EINVAL
        assert(call("sceKernelFtruncate", fd, static_cast<std::uint64_t>(-1)) == 0x80020016);
        assert(call("ftruncate", fd, static_cast<std::uint64_t>(-5)) == static_cast<std::uint64_t>(-1));

        // Truncamento válido de 20 para 8 bytes
        assert(call("sceKernelFtruncate", fd, 8) == 0);
        assert(call("sceKernelClose", fd) == 0);
        {
            std::ifstream ifs(trunc_file, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            assert(content.size() == 8);
            assert(content == "01234567");
        }

        rt.attach_vfs(nullptr);
        std::error_code ec_clean;
        std::filesystem::remove_all(test_dir, ec_clean);
    }

    // =========================================================================
    // T183: Validação de memória guest e reversão no scheduler para sema,
    // mutex, cond e event; e escrita estrita de 32 bits em sceKernelCreateSema
    // =========================================================================
    {
        GuestMemory mem(2 * 1024 * 1024);
        assert(mem.map(0x1000, 0x10000, read | write) == MemoryError::ok);
        GuestScheduler sched(mem);
        OrbisRuntime rt(mem);
        rt.attach_scheduler(sched);

        auto call = [&](const std::string& name, std::uint64_t a0 = 0, std::uint64_t a1 = 0,
                        std::uint64_t a2 = 0, std::uint64_t a3 = 0, std::uint64_t a4 = 0,
                        std::uint64_t a5 = 0) -> std::uint64_t {
            CpuState c{};
            c.gpr[7] = a0;
            c.gpr[6] = a1;
            c.gpr[2] = a2;
            c.gpr[1] = a3;
            c.gpr[8] = a4;
            c.gpr[9] = a5;
            return rt.invoke(name, c);
        };

        // 1. sceKernelCreateSema não sobrescreve os 4 bytes adjacentes (escreve estritamente 32 bits)
        constexpr std::uint64_t sema_out_addr = 0x2000;
        constexpr std::uint32_t guard_val = 0xDEADBEEF;
        assert(mem.store(sema_out_addr, &guard_val, 4) == MemoryError::ok);
        assert(mem.store(sema_out_addr + 4, &guard_val, 4) == MemoryError::ok);

        // sceKernelCreateSema(out_id_addr, name, attr, init, max, opt)
        auto sema_res = call("sceKernelCreateSema", sema_out_addr, 0, 0, 1, 5, 1);
        assert(sema_res == 0);

        // Handle gravado nos primeiros 4 bytes
        std::uint32_t sema_handle = 0;
        assert(mem.load(sema_out_addr, &sema_handle, 4) == MemoryError::ok && sema_handle > 0);

        // Os 4 bytes vizinhos continuam intactos com guard_val (sem corrupção por escrita de 64 bits)
        std::uint32_t adjacent_val = 0;
        assert(mem.load(sema_out_addr + 4, &adjacent_val, 4) == MemoryError::ok);
        assert(adjacent_val == guard_val);

        // 2. Rejeição de endereço de saída inválido/não-mapeado sem vazar recursos no scheduler
        constexpr std::uint64_t unmapped_addr = 0x9000000ULL; // Não mapeado
        assert(call("sceKernelCreateSema", unmapped_addr, 0, 0, 1, 5, 1) == 0x80020016);
        assert(call("scePthreadMutexInit", unmapped_addr, 0) == 0x80020016);
        assert(call("scePthreadCondInit", unmapped_addr, 0) == 0x80020016);
        assert(call("sceKernelCreateEventFlag", unmapped_addr, 0, 0, 0) == 0x80020016);

        // 3. Destruição segura do semáforo criado
        assert(call("sceKernelDeleteSema", sema_handle) == 0);
    }

    // =========================================================================
    // T152: sysctl / KERN_ARANDOM com limite de comprimento e validação de memória
    // =========================================================================
    {
        GuestMemory mem(1024 * 1024);
        assert(mem.map(0x1000, 0x10000, read | write) == MemoryError::ok);
        OrbisRuntime rt(mem);

        auto call = [&](const std::string& name, std::uint64_t a0 = 0, std::uint64_t a1 = 0,
                        std::uint64_t a2 = 0, std::uint64_t a3 = 0) -> std::uint64_t {
            CpuState c{};
            c.gpr[7] = a0;
            c.gpr[6] = a1;
            c.gpr[2] = a2;
            c.gpr[1] = a3;
            return rt.invoke(name, c);
        };

        constexpr std::uint64_t name_addr = 0x2000;
        constexpr std::uint64_t old_ptr_addr = 0x3000;
        constexpr std::uint64_t oldlen_addr = 0x4000;

        // Configura name mib [CTL_KERN (1), KERN_ARANDOM (37)]
        std::int32_t mib[2] = {1, 37};
        assert(mem.store(name_addr, mib, sizeof(mib)) == MemoryError::ok);

        // 1. Validação com tamanho normal (32 bytes)
        std::uint64_t req_len = 32;
        assert(mem.store(oldlen_addr, &req_len, sizeof(req_len)) == MemoryError::ok);
        assert(call("sysctl", name_addr, 2, old_ptr_addr, oldlen_addr) == 0);

        std::uint64_t written_len = 0;
        assert(mem.load(oldlen_addr, &written_len, sizeof(written_len)) == MemoryError::ok);
        assert(written_len == 32);

        // 2. Limite de comprimento adversarial (10000 bytes -> limitado com segurança a 256 bytes)
        req_len = 10000;
        assert(mem.store(oldlen_addr, &req_len, sizeof(req_len)) == MemoryError::ok);
        assert(call("sysctl", name_addr, 2, old_ptr_addr, oldlen_addr) == 0);
        assert(mem.load(oldlen_addr, &written_len, sizeof(written_len)) == MemoryError::ok);
        assert(written_len == 256); // Limitado a MAX_ARANDOM_LEN sem buffer overflow

        // 3. Ponteiro inválido/não mapeado retorna EFAULT (0x0E)
        constexpr std::uint64_t unmapped_addr = 0x9000000ULL;
        assert(call("sysctl", unmapped_addr, 2, old_ptr_addr, oldlen_addr) == 0x0E);
        assert(call("sysctl", name_addr, 2, unmapped_addr, oldlen_addr) == 0x0E);
        assert(call("sysctl", name_addr, 2, old_ptr_addr, unmapped_addr) == 0x0E);

        // 4. namelen < 2 retorna EINVAL (0x16)
        assert(call("sysctl", name_addr, 1, old_ptr_addr, oldlen_addr) == 0x16);
    }

    // =========================================================================
    // T179 / T180: scePthreadCreate validação preliminar, reversão em falhas,
    // e validação estrita de templates TLS/DTV/TCB sem corrupção ou vazamentos
    // =========================================================================
    {
        GuestMemory mem(8 * 1024 * 1024);
        assert(mem.map(0x1000, 0x100000, read | write) == MemoryError::ok);
        GuestScheduler sched(mem);
        OrbisRuntime rt(mem);
        rt.attach_scheduler(sched);

        auto call_trap = [&](const std::string& name, CpuState& cpu, GuestScheduler::Id tid = 1, std::uint64_t ret = 0x3000) {
            return rt.invoke_trap(name, cpu, tid, ret);
        };

        constexpr std::uint64_t valid_entry = 0x10000;
        constexpr std::uint64_t out_tid_addr = 0x20000;
        constexpr std::uint64_t return_trap = 0x3000;
        constexpr std::uint64_t unmapped_addr = 0x9000000ULL;

        // 1. T179: a(0) inválido/não mapeado -> rejeitado antes de alocar thread ou memória
        std::size_t initial_thread_cnt = sched.thread_count();
        CpuState c_bad_out{};
        c_bad_out.gpr[7] = unmapped_addr; // out handle inválido
        c_bad_out.gpr[6] = 0;             // attr
        c_bad_out.gpr[2] = valid_entry;   // entry
        c_bad_out.gpr[1] = 0;             // arg
        auto res_bad_out = call_trap("scePthreadCreate", c_bad_out, 1, return_trap);
        assert(res_bad_out.value == 0x80020016); // invalid
        assert(sched.thread_count() == initial_thread_cnt); // Sem thread órfã no scheduler

        // 2. T179: entry < 0x1000 -> rejeitado antes de alocar
        CpuState c_bad_entry{};
        c_bad_entry.gpr[7] = out_tid_addr;
        c_bad_entry.gpr[6] = 0;
        c_bad_entry.gpr[2] = 0x400; // entry point inválido
        auto res_bad_entry = call_trap("scePthreadCreate", c_bad_entry, 1, return_trap);
        assert(res_bad_entry.value == 0x80020016);
        assert(sched.thread_count() == initial_thread_cnt);

        // 3. T180: template TLS com filesz > memsz -> rejeição estrita
        Ps4Tls bad_tls{};
        bad_tls.address = 0x30000;
        bad_tls.filesz = 0x2000;
        bad_tls.memsz = 0x1000; // filesz > memsz!
        rt.set_tls_template(bad_tls);

        CpuState c_tls_fail{};
        c_tls_fail.gpr[7] = out_tid_addr;
        c_tls_fail.gpr[6] = 0;
        c_tls_fail.gpr[2] = valid_entry;
        auto res_tls_fail = call_trap("scePthreadCreate", c_tls_fail, 1, return_trap);
        assert(res_tls_fail.value == 0x80020016);
        assert(sched.thread_count() == initial_thread_cnt);

        // 4. T180: template TLS com alinhamento não-potência de 2 -> rejeição
        Ps4Tls bad_align_tls{};
        bad_align_tls.address = 0x30000;
        bad_align_tls.filesz = 0x100;
        bad_align_tls.memsz = 0x100;
        bad_align_tls.align = 15; // Não é potência de 2!
        rt.set_tls_template(bad_align_tls);
        assert(call_trap("scePthreadCreate", c_tls_fail, 1, return_trap).value == 0x80020016);

        // 5. T180: template TLS com memsz excedendo o limite da região de 128 KiB
        Ps4Tls huge_tls{};
        huge_tls.address = 0x30000;
        huge_tls.filesz = 0x100;
        huge_tls.memsz = 0x30000; // 192 KiB > 128 KiB region!
        huge_tls.align = 16;
        rt.set_tls_template(huge_tls);
        assert(call_trap("scePthreadCreate", c_tls_fail, 1, return_trap).value == 0x80020016);

        // 6. T180: template TLS com filesz > 0 apontando para memória não mapeada
        Ps4Tls unmapped_tls{};
        unmapped_tls.address = unmapped_addr;
        unmapped_tls.filesz = 0x100;
        unmapped_tls.memsz = 0x1000;
        unmapped_tls.align = 16;
        rt.set_tls_template(unmapped_tls);
        assert(call_trap("scePthreadCreate", c_tls_fail, 1, return_trap).value == 0x80020016);

        // 7. T180: template de módulo TLS malformado (filesz > memsz) -> rejeição
        Ps4Tls valid_main_tls{};
        valid_main_tls.address = 0x30000;
        valid_main_tls.filesz = 0x100;
        valid_main_tls.memsz = 0x1000;
        valid_main_tls.align = 16;
        rt.set_tls_template(valid_main_tls);

        Ps4Tls bad_mod{};
        bad_mod.module_id = 2;
        bad_mod.filesz = 0x500;
        bad_mod.memsz = 0x200; // filesz > memsz
        rt.set_module_tls_templates({bad_mod});
        assert(call_trap("scePthreadCreate", c_tls_fail, 1, return_trap).value == 0x80020016);
        assert(sched.thread_count() == initial_thread_cnt);

        // 8. T180 / T179: Criação válida com template TLS válido
        Ps4Tls good_mod{};
        good_mod.module_id = 2;
        good_mod.address = 0x31000;
        good_mod.filesz = 16;
        good_mod.memsz = 64;
        good_mod.align = 16;
        rt.set_module_tls_templates({good_mod});

        CpuState c_ok{};
        c_ok.gpr[7] = out_tid_addr;
        c_ok.gpr[6] = 0;
        c_ok.gpr[2] = valid_entry;
        c_ok.gpr[1] = 0x555;
        auto res_ok = call_trap("scePthreadCreate", c_ok, 1, return_trap);
        assert(res_ok.value == 0);
        assert(sched.thread_count() == initial_thread_cnt + 1);

        std::uint64_t created_tid = 0;
        assert(mem.load(out_tid_addr, &created_tid, 8) == MemoryError::ok && created_tid > 0);
    }

    // =========================================================================
    // T150: Identidade, espera e saída em pthread_self, pthread_join e pthread_exit
    // =========================================================================
    {
        GuestMemory mem(4 * 1024 * 1024);
        assert(mem.map(0x1000, 0x80000, read | write) == MemoryError::ok);

        // 1. Modo sem scheduler: recusa operações sem sucesso fictício e retorna tid determinístico
        {
            OrbisRuntime rt_nosched(mem);
            CpuState cpu{};
            // pthread_self retorna identidade determinística guest (1), não endereço host
            assert(rt_nosched.invoke("pthread_self", cpu) == 1);
            assert(rt_nosched.invoke("scePthreadSelf", cpu) == 1);

            // pthread_join e scePthreadJoin sem scheduler retornam erro explícito, não 0
            cpu.gpr[7] = 2; // target tid
            cpu.gpr[6] = 0;
            assert(rt_nosched.invoke("pthread_join", cpu) == 0x80020016);
            assert(rt_nosched.invoke("scePthreadJoin", cpu) == 0x80020016);

            // pthread_exit sem scheduler retorna erro explícito
            assert(rt_nosched.invoke("pthread_exit", cpu) == 0x80020016);
            assert(rt_nosched.invoke("scePthreadExit", cpu) == 0x80020016);
        }

        // 2. Modo com scheduler: ciclo de vida real de threads
        {
            GuestScheduler sched(mem);
            OrbisRuntime rt_sched(mem);
            rt_sched.attach_scheduler(sched);

            // Cria thread 1 (main) e thread 2 no scheduler
            auto main_tid = sched.create(0x10000, 0x50000, 0x60000, 0x3000);
            assert(main_tid == 1);
            assert(sched.start(main_tid) == 0);

            constexpr std::uint64_t t2_entry = 0x20000;
            constexpr std::uint64_t t2_bottom = 0x60000;
            constexpr std::uint64_t t2_top = 0x70000;
            auto tid2 = sched.create(t2_entry, t2_bottom, t2_top, 0x3000, 0x123);
            assert(tid2 == 2);
            assert(sched.start(tid2) == 0);

            // pthread_join em si mesmo deve retornar EDEADLK (0x8002000b)
            CpuState c_join{};
            c_join.gpr[7] = 1; // target = tid atual (1)
            c_join.gpr[6] = 0;
            auto res_deadlk = rt_sched.invoke_trap("pthread_join", c_join, 1, 0x3000);
            assert(static_cast<std::uint32_t>(res_deadlk.value) == static_cast<std::uint32_t>(ORBIS_KERNEL_ERROR_EDEADLK));

            // pthread_join em thread inexistente deve retornar ESRCH (0x80020003)
            c_join.gpr[7] = 9999;
            auto res_srch = rt_sched.invoke_trap("pthread_join", c_join, 1, 0x3000);
            assert(static_cast<std::uint32_t>(res_srch.value) == static_cast<std::uint32_t>(ORBIS_KERNEL_ERROR_ESRCH));

            // pthread_join com ponteiro de saída inválido deve retornar erro
            constexpr std::uint64_t unmapped_addr = 0x9000000ULL;
            c_join.gpr[7] = tid2;
            c_join.gpr[6] = unmapped_addr;
            auto res_bad_out = rt_sched.invoke_trap("pthread_join", c_join, 1, 0x3000);
            assert(static_cast<std::uint32_t>(res_bad_out.value) == static_cast<std::uint32_t>(ORBIS_KERNEL_ERROR_EINVAL));

            // Thread 2 faz pthread_exit com valor 0xBEEF
            CpuState c_exit{};
            c_exit.gpr[7] = 0xBEEF;
            auto res_exit = rt_sched.invoke_trap("pthread_exit", c_exit, tid2, 0x3000);
            assert(res_exit.action == OrbisRuntime::InvocationResult::Action::exited);
            assert(sched.state(tid2) == GuestScheduler::State::exited);
            assert(sched.value(tid2) == 0xBEEF);

            // Agora thread 1 faz pthread_join em thread 2 já encerrada
            constexpr std::uint64_t out_val_addr = 0x25000;
            c_join.gpr[7] = tid2;
            c_join.gpr[6] = out_val_addr;
            auto res_join = rt_sched.invoke_trap("pthread_join", c_join, 1, 0x3000);
            assert(res_join.value == 0);
            std::uint64_t joined_val = 0;
            assert(mem.load(out_val_addr, &joined_val, 8) == MemoryError::ok);
            assert(joined_val == 0xBEEF);
        }
    }

    // =========================================================================
    // T151: Sinais guest, isolamento de máscaras por instância/thread e validação
    // =========================================================================
    {
        GuestMemory mem(2 * 1024 * 1024);
        assert(mem.map(0x1000, 0x40000, read | write) == MemoryError::ok);

        auto call = [&](OrbisRuntime& rt, const std::string& name, std::uint64_t a0 = 0,
                        std::uint64_t a1 = 0, std::uint64_t a2 = 0) -> std::uint64_t {
            CpuState c{};
            c.gpr[7] = a0;
            c.gpr[6] = a1;
            c.gpr[2] = a2;
            return rt.invoke(name, c);
        };

        OrbisRuntime rt1(mem);
        OrbisRuntime rt2(mem);

        constexpr std::uint64_t set1_addr = 0x10000;
        constexpr std::uint64_t set2_addr = 0x10020;
        constexpr std::uint64_t act_addr = 0x10040;
        constexpr std::uint64_t oact_addr = 0x10080;
        constexpr std::uint64_t unmapped_addr = 0x9000000ULL;

        // 1. sigemptyset e sigfillset
        assert(call(rt1, "sigemptyset", set1_addr) == 0);
        std::uint32_t raw_zeros[4] = {};
        assert(mem.load(set1_addr, raw_zeros, sizeof(raw_zeros)) == MemoryError::ok);
        for (int i = 0; i < 4; ++i) assert(raw_zeros[i] == 0);

        assert(call(rt1, "sigfillset", set1_addr) == 0);
        std::uint32_t raw_ones[4] = {};
        assert(mem.load(set1_addr, raw_ones, sizeof(raw_ones)) == MemoryError::ok);
        for (int i = 0; i < 4; ++i) assert(raw_ones[i] == ~0U);

        // Erro com ponteiro inválido em sigemptyset / sigfillset
        assert(call(rt1, "sigemptyset", unmapped_addr) == 0x0E); // EFAULT
        assert(call(rt1, "sigfillset", unmapped_addr) == 0x0E); // EFAULT

        // 2. sigaddset, sigdelset, sigismember
        assert(call(rt1, "sigemptyset", set1_addr) == 0);
        assert(call(rt1, "sigismember", set1_addr, 5) == 0);
        assert(call(rt1, "sigaddset", set1_addr, 5) == 0);
        assert(call(rt1, "sigismember", set1_addr, 5) == 1);
        assert(call(rt1, "sigaddset", set1_addr, 65) == 0);
        assert(call(rt1, "sigismember", set1_addr, 65) == 1);
        assert(call(rt1, "sigismember", set1_addr, 6) == 0);

        assert(call(rt1, "sigdelset", set1_addr, 5) == 0);
        assert(call(rt1, "sigismember", set1_addr, 5) == 0);
        assert(call(rt1, "sigismember", set1_addr, 65) == 1);

        // Rejeição de signo inválido
        assert(call(rt1, "sigaddset", set1_addr, 0) == 0x16); // EINVAL
        assert(call(rt1, "sigaddset", set1_addr, 129) == 0x16); // EINVAL

        // 3. sigaction e signal: manipulação e rejeição de SIGKILL e SIGSTOP
        assert(call(rt1, "signal", 9, 0x1234) == ~0ULL); // SIGKILL rejeitado
        assert(call(rt1, "signal", 19, 0x1234) == ~0ULL); // SIGSTOP rejeitado
        assert(call(rt1, "sigaction", 9, act_addr, oact_addr) == 0x16); // EINVAL

        // Registro de sinal válido (ex: SIGINT = 2)
        assert(call(rt1, "signal", 2, 0xCAFEBABE) == 0); // Retorna anterior (0)
        assert(call(rt1, "signal", 2, 0xDEADBEEF) == 0xCAFEBABE); // Retorna 0xCAFEBABE

        // 4. Isolamento estrito de máscaras entre instâncias distintas
        // Configura máscara na instância 1: bloqueia sinais 1..32
        std::uint32_t mask_in[4] = {0xFFFFFFFFU, 0, 0, 0};
        assert(mem.store(set1_addr, mask_in, sizeof(mask_in)) == MemoryError::ok);
        assert(call(rt1, "sigprocmask", 3 /* SIG_SETMASK */, set1_addr, 0) == 0);

        // Consulta máscara na instância 1 (SIGKILL e SIGSTOP obrigatoriamente preservados desmarcados)
        assert(call(rt1, "sigprocmask", 0, 0, set2_addr) == 0);
        std::uint32_t read_mask1[4] = {};
        assert(mem.load(set2_addr, read_mask1, sizeof(read_mask1)) == MemoryError::ok);
        const std::uint32_t expected_mask = 0xFFFFFFFFU & ~((1U << (9 - 1)) | (1U << (17 - 1)) | (1U << (19 - 1)));
        assert(read_mask1[0] == expected_mask);

        // Instância 2 deve continuar com máscara limpa (independente, sem static global compartilhado)
        assert(call(rt2, "sigprocmask", 0, 0, set2_addr) == 0);
        std::uint32_t read_mask2[4] = {};
        assert(mem.load(set2_addr, read_mask2, sizeof(read_mask2)) == MemoryError::ok);
        assert(read_mask2[0] == 0); // Instância 2 não foi poluída!

        // 5. T151: pthread_sigmask, sigpending e protecao de SIGSTOP (17)
        assert(call(rt1, "signal", 17, 0x1234) == ~0ULL); // SIGSTOP (17) rejeitado
        assert(call(rt1, "sigaction", 17, act_addr, oact_addr) == 0x16); // EINVAL
        assert(call(rt1, "pthread_sigmask", 3 /* SIG_SETMASK */, set1_addr, 0) == 0);
        assert(call(rt1, "sigpending", set2_addr) == 0);
        assert(call(rt1, "sigpending", unmapped_addr) == 0x0E); // EFAULT
    }

    // =========================================================================
    // T155: sceAvPlayer ciclo de vida, validacao e transicao de introducao
    // =========================================================================
    {
        GuestMemory mem(1024 * 1024);
        assert(mem.map(0x1000, 0x20000, read | write) == MemoryError::ok);
        OrbisRuntime rt(mem);

        auto call = [&](const std::string& name, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0) -> std::uint64_t {
            CpuState c{};
            c.gpr[7] = a0;
            c.gpr[6] = a1;
            c.gpr[2] = a2;
            return rt.invoke(name, c);
        };

        // 1. Inicializacao do player
        auto h = call("sceAvPlayerInit");
        assert(h >= 0x10000);

        // 2. Operacoes em player invalido retornam erro
        assert(call("sceAvPlayerAddSource", 999999, 0x5000) == 0x80540001);
        assert(call("sceAvPlayerStart", 999999) == 0x80540001);
        assert(call("sceAvPlayerStop", 999999) == 0x80540001);
        assert(call("sceAvPlayerClose", 999999) == 0x80540001);

        // 3. Start sem fonte configurada retorna erro
        assert(call("sceAvPlayerStart", h) == 0x80540002);

        // 4. Fonte nula ou de formato nao suportado retorna erro (sem confirmar fonte invalida como sucesso)
        assert(call("sceAvPlayerAddSource", h, 0) == 0x80540002);
        constexpr std::uint64_t unmapped_addr = 0x9000000ULL;
        assert(call("sceAvPlayerAddSource", h, unmapped_addr) == 0x80540002);

        constexpr std::uint64_t bad_path_addr = 0x4800;
        const std::string bad_src = "/app0/data/not_a_movie.txt";
        assert(mem.store(bad_path_addr, bad_src.c_str(), bad_src.size() + 1) == MemoryError::ok);
        assert(call("sceAvPlayerAddSource", h, bad_path_addr) == 0x80540002); // Rejeita formato invalido

        // Fonte valida
        constexpr std::uint64_t path_addr = 0x5000;
        const std::string src_path = "/app0/movie/intro.mp4";
        assert(mem.store(path_addr, src_path.c_str(), src_path.size() + 1) == MemoryError::ok);
        assert(call("sceAvPlayerAddSource", h, path_addr) == 0);
        assert(call("sceAvPlayerIsActive", h) == 1);
        assert(call("sceAvPlayerGetDuration", h) == 3000);
        assert(call("sceAvPlayerStreamCount", h) == 2);

        // 5. Streams e informacoes dos fluxos de audio e video
        constexpr std::uint64_t stream_info_addr = 0x6000;
        // Stream 0: Video
        assert(call("sceAvPlayerGetStreamInfo", h, 0, stream_info_addr) == 0);
        std::uint32_t s_video[6] = {};
        assert(mem.load(stream_info_addr, s_video, sizeof(s_video)) == MemoryError::ok);
        assert(s_video[0] == 1); // Video type
        assert(s_video[1] == 1920); // 1080p
        assert(s_video[2] == 1080);
        assert(s_video[3] == 30); // 30 FPS
        assert(s_video[4] == 3000); // 3000 ms

        // Stream 1: Audio
        assert(call("sceAvPlayerGetStreamInfo", h, 1, stream_info_addr) == 0);
        std::uint32_t s_audio[6] = {};
        assert(mem.load(stream_info_addr, s_audio, sizeof(s_audio)) == MemoryError::ok);
        assert(s_audio[0] == 2); // Audio type
        assert(s_audio[1] == 2); // Stereo
        assert(s_audio[2] == 48000); // 48 kHz

        // Stream 2: Indice invalido
        assert(call("sceAvPlayerGetStreamInfo", h, 2, stream_info_addr) == 0x80540003);
        assert(call("sceAvPlayerEnableStream", h, 1, 2) == 0x80540003); // Indice invalido
        assert(call("sceAvPlayerEnableStream", h, 1, 0) == 0);

        // 6. Obtencao de dados de quadro (VideoData) e dados de audio
        constexpr std::uint64_t frame_info_addr = 0x7000;
        assert(call("sceAvPlayerGetVideoData", h, frame_info_addr) == 1); // Quadro disponivel
        std::uint32_t f_data[8] = {};
        assert(mem.load(frame_info_addr, f_data, sizeof(f_data)) == MemoryError::ok);
        assert(f_data[4] == 1920 && f_data[5] == 1080);

        constexpr std::uint64_t audio_data_addr = 0x8000;
        assert(call("sceAvPlayerGetAudioData", h, audio_data_addr) == 1); // Audio disponivel
        std::uint32_t a_data[6] = {};
        assert(mem.load(audio_data_addr, a_data, sizeof(a_data)) == MemoryError::ok);
        assert(a_data[4] == 2 && a_data[5] == 48000);

        // 7. Pausa e retomada
        assert(call("sceAvPlayerPause", h) == 0);
        assert(call("sceAvPlayerIsPaused", h) == 1);
        assert(call("sceAvPlayerResume", h) == 0);
        assert(call("sceAvPlayerIsPaused", h) == 0);

        // 8. Jump to time e Reset
        assert(call("sceAvPlayerJumpToTime", h, 1500) == 0);
        assert(call("sceAvPlayerCurrentTime", h) == 1533);
        assert(call("sceAvPlayerReset", h) == 0);
        assert(call("sceAvPlayerCurrentTime", h) == 33);

        // 9. Avanco no tempo e transicao offline (ao atingir a duracao, torna-se inativo per US2/AC1)
        for (int i = 0; i < 100; ++i) {
            call("sceAvPlayerCurrentTime", h);
        }
        assert(call("sceAvPlayerIsActive", h) == 0); // Transicao de video concluida
        // Quando inativo / EOF: GetVideoData e GetAudioData retornam 0 sem frames disponiveis
        assert(call("sceAvPlayerGetVideoData", h, frame_info_addr) == 0);
        assert(call("sceAvPlayerGetAudioData", h, audio_data_addr) == 0);

        // 10. Fechamento
        assert(call("sceAvPlayerClose", h) == 0);
        assert(call("sceAvPlayerClose", h) == 0x80540001); // Ja fechado
    }

    // =========================================================================
    // T157: Conexão de submissão de draws e dispatches guest (GnmSubmissionListener)
    // =========================================================================
    {
        GuestMemory mem(2 * 1024 * 1024);
        assert(mem.map(0x1000, 0x50000, read | write) == MemoryError::ok);
        OrbisRuntime rt(mem);

        struct TestVideoSink : public VideoOutSink {
            void on_flip(const FlipEvent&) override {}
        } test_sink;
        VideoOutManager vom(16);
        vom.attach_guest_memory(&mem, &test_sink);
        rt.attach_video_out(&vom);

        auto call = [&](const std::string& name, std::uint64_t a0 = 0, std::uint64_t a1 = 0,
                        std::uint64_t a2 = 0, std::uint64_t a3 = 0, std::uint64_t a4 = 0,
                        std::uint64_t a5 = 0, std::uint64_t a6 = 0, std::uint64_t a7 = 0) -> std::uint64_t {
            CpuState c{};
            c.gpr[7] = a0;
            c.gpr[6] = a1;
            c.gpr[2] = a2;
            c.gpr[1] = a3;
            c.gpr[8] = a4;
            c.gpr[9] = a5;
            c.gpr[4] = 0x2ff0;
            assert(mem.store(0x2ff8, &a6, 8) == MemoryError::ok);
            assert(mem.store(0x3000, &a7, 8) == MemoryError::ok);
            return rt.invoke(name, c);
        };

        auto vh = call("sceVideoOutOpen", 0, 0, 0, 0);
        assert(vh > 0);
        constexpr std::uint64_t attr_addr = 0x6000;
        constexpr std::uint64_t addrs_ptr = 0x6100;
        constexpr std::uint64_t buf0_addr = 0x7000;
        assert(call("sceVideoOutSetBufferAttribute", attr_addr, 0x80000000, 1, 0, 2, 2, 2) == 0);
        assert(mem.store(addrs_ptr, &buf0_addr, 8) == MemoryError::ok);
        assert(call("sceVideoOutRegisterBuffers", vh, 0, addrs_ptr, 1, attr_addr) == 0);

        class MockGnmListener : public GnmSubmissionListener {
        public:
            bool allow_draws = true;
            bool allow_dispatches = true;
            std::size_t draw_callback_count = 0;
            std::size_t dispatch_callback_count = 0;
            std::vector<DrawCall> last_draws;
            std::vector<DispatchCall> last_dispatches;

            bool on_draw_calls(const std::vector<DrawCall>& draws) override {
                ++draw_callback_count;
                last_draws = draws;
                return allow_draws;
            }

            bool on_dispatch_calls(const std::vector<DispatchCall>& dispatches) override {
                ++dispatch_callback_count;
                last_dispatches = dispatches;
                return allow_dispatches;
            }
        } mock_listener;

        rt.set_gnm_listener(&mock_listener);
        assert(rt.gnm_listener() == &mock_listener);

        // Escreve 3 vértices de teste em 0x20000 (stride 16)
        float vertices[12] = {
            0.0f, 0.5f, 0.0f, 1.0f,
            -0.5f, -0.5f, 0.0f, 1.0f,
            0.5f, -0.5f, 0.0f, 1.0f
        };
        assert(mem.store(0x20000, vertices, sizeof(vertices)) == MemoryError::ok);

        // Configura RT0 e Vertex Buffer no contexto GNM
        rt.gnm_context()->set_context_reg(bb::gcn_reg::CB_COLOR0_BASE, 0x30000 >> 8);
        rt.gnm_context()->set_context_reg(bb::gcn_reg::CB_COLOR0_PITCH, 64);
        rt.gnm_context()->set_context_reg(bb::gcn_reg::CB_COLOR0_DIM, ((64 - 1) << 14) | (64 - 1));
        rt.gnm_context()->set_context_reg(bb::gcn_reg::CB_COLOR0_INFO, 0x5C); // fmt_id=7, swap=1 = B8G8R8A8_Unorm
        rt.gnm_context()->set_context_reg(bb::gcn_reg::CB_TARGET_MASK, 1);
        rt.gnm_context()->set_user_data(bb::ShaderStage::Vertex, 0, 0x20000); // base lo
        rt.gnm_context()->set_user_data(bb::ShaderStage::Vertex, 1, 16 << 16); // stride 16
        rt.gnm_context()->set_user_data(bb::ShaderStage::Vertex, 2, 3);        // 3 records

        constexpr std::uint64_t dcb_addr = 0x8000;
        constexpr std::uint64_t dcb_ptr_table = 0x8500;
        constexpr std::uint64_t dcb_size_table = 0x8550;
        assert(mem.store(dcb_ptr_table, &dcb_addr, 8) == MemoryError::ok);

        // 1. Draw call: PM4 draw_auto (0xC0012D00, count=3, prim=4 (TriList))
        const std::uint32_t draw_cmds[] = {0xC0012D00, 3, 4};
        assert(mem.store(dcb_addr, draw_cmds, sizeof(draw_cmds)) == MemoryError::ok);
        std::uint32_t dcb_size_bytes = sizeof(draw_cmds);
        assert(mem.store(dcb_size_table, &dcb_size_bytes, 4) == MemoryError::ok);

        // Teste 1A: listener rejeita draw -> erro propagado e flip inibido
        mock_listener.allow_draws = false;
        assert(call("sceGnmSubmitAndFlipCommandBuffers", 1, dcb_ptr_table, dcb_size_table, 0, 0, 0, 99, 42) == UINT64_MAX);
        assert(mock_listener.draw_callback_count == 1);
        assert(mock_listener.last_draws.size() == 1);
        assert(mock_listener.last_draws[0].vertex_count == 3);
        assert(rt.last_rasterizer_status() == RasterizerStatus::Error);
        assert(rt.rasterizer_failure_count() == 1);
        assert(rt.submit_count() == 0);
        assert(!vom.has_new_frame() && vom.flip_count() == 0);

        // Teste 1B: listener aceita draw -> sucesso e flip executado
        mock_listener.allow_draws = true;
        assert(call("sceGnmSubmitAndFlipCommandBuffers", 1, dcb_ptr_table, dcb_size_table, 0, 0, 0, 99, 42) == 0);
        assert(mock_listener.draw_callback_count == 2);
        assert(rt.last_rasterizer_status() == RasterizerStatus::Ok);
        assert(rt.rasterizer_failure_count() == 1);
        assert(rt.submit_count() == 1);
        assert(vom.has_new_frame());
        assert(vom.flip_count() == 1);

        // 2. Dispatch call: PM4 dispatch (0xC0031500, x=4, y=2, z=1, 0)
        const std::uint32_t disp_cmds[] = {0xC0031500, 4, 2, 1, 0};
        assert(mem.store(dcb_addr, disp_cmds, sizeof(disp_cmds)) == MemoryError::ok);
        dcb_size_bytes = sizeof(disp_cmds);
        assert(mem.store(dcb_size_table, &dcb_size_bytes, 4) == MemoryError::ok);

        // Teste 2A: listener rejeita dispatch -> erro propagado
        mock_listener.allow_dispatches = false;
        assert(call("sceGnmSubmitCommandBuffers", 1, dcb_ptr_table, dcb_size_table) == UINT64_MAX);
        assert(mock_listener.dispatch_callback_count == 1);
        assert(mock_listener.last_dispatches.size() == 1);
        assert(mock_listener.last_dispatches[0].threadgroups_x == 4);
        assert(mock_listener.last_dispatches[0].threadgroups_y == 2);
        assert(mock_listener.last_dispatches[0].threadgroups_z == 1);
        assert(rt.last_rasterizer_status() == RasterizerStatus::Error);
        assert(rt.rasterizer_failure_count() == 2);
        assert(rt.submit_count() == 1);

        // Teste 2B: listener aceita dispatch -> sucesso
        mock_listener.allow_dispatches = true;
        assert(call("sceGnmSubmitCommandBuffers", 1, dcb_ptr_table, dcb_size_table) == 0);
        assert(mock_listener.dispatch_callback_count == 2);
        assert(rt.last_rasterizer_status() == RasterizerStatus::Ok);
        assert(rt.rasterizer_failure_count() == 2);
        assert(rt.submit_count() == 2);

        // Teste 3 (T157): Conexão direta com GnmMetalBridge convertendo comandos guest para MetalDrawCommand
        class BridgeGnmListener : public GnmSubmissionListener {
        public:
            explicit BridgeGnmListener(const GuestMemory& m) : bridge_(m) {
                bridge_.set_allow_fallback_shaders(true);
            }
            bool on_draw_calls(const std::vector<DrawCall>& draws) override {
                for (const auto& d : draws) {
                    auto res = bridge_.process_draw_call(d);
                    if (!res.ok()) return false;
                    processed_draws.push_back(*res);
                }
                return true;
            }
            bool on_dispatch_calls(const std::vector<DispatchCall>& dispatches) override {
                for (const auto& dp : dispatches) {
                    auto res = bridge_.process_dispatch(dp);
                    if (!res.ok()) return false;
                    processed_dispatches.push_back(*res);
                }
                return true;
            }
            GnmMetalBridge bridge_;
            std::vector<MetalDrawCommand> processed_draws;
            std::vector<MetalDispatchCommand> processed_dispatches;
        } bridge_listener(mem);

        rt.set_gnm_listener(&bridge_listener);
        assert(rt.gnm_listener() == &bridge_listener);

        // Submissão autoral de draw call válida através da ponte Metal
        assert(mem.store(dcb_addr, draw_cmds, sizeof(draw_cmds)) == MemoryError::ok);
        dcb_size_bytes = sizeof(draw_cmds);
        assert(mem.store(dcb_size_table, &dcb_size_bytes, 4) == MemoryError::ok);
        assert(call("sceGnmSubmitAndFlipCommandBuffers", 1, dcb_ptr_table, dcb_size_table, 0, 0, 0, 99, 43) == 0);
        assert(!bridge_listener.processed_draws.empty());
        const auto& metal_cmd = bridge_listener.processed_draws.back();
        assert(metal_cmd.valid);
        assert(metal_cmd.vertex_count == 3);
        assert(metal_cmd.pipeline_config.vs_hash != 0);
        assert(metal_cmd.pipeline_config.active_color_attachments == 1);
        assert(metal_cmd.pipeline_config.color_attachments[0].pixel_format == MetalPixelFormat::BGRA8Unorm);
        assert(vom.has_new_frame());
        PresentationFrame pres_frame;
        assert(vom.read_presentation_frame(pres_frame));
        assert(pres_frame.valid);
        assert(pres_frame.width == 2);
        assert(pres_frame.height == 2);
        assert(!pres_frame.pixel_data.empty());
        vom.consume_new_frame();

        // Submissão de draw com buffer unmapped na ponte Metal -> ponte rejeita, erro é propagado e flip bloqueado
        BufferDescriptor bad_buf{};
        bad_buf.base_address = 0x7FFF0000u;
        bad_buf.num_records = 64;
        bad_buf.stride = 16;
        bridge_listener.bridge_.bind_buffer(ShaderStage::Vertex, 0, bad_buf);
        assert(call("sceGnmSubmitAndFlipCommandBuffers", 1, dcb_ptr_table, dcb_size_table, 0, 0, 0, 99, 44) == UINT64_MAX);
        assert(rt.last_rasterizer_status() == RasterizerStatus::Error);
        assert(!vom.has_new_frame());

        // Teste 4: Desvinculação do listener -> submissões passam diretamente sem callbacks adicionais
        rt.set_gnm_listener(nullptr);
        assert(rt.gnm_listener() == nullptr);
        rt.gnm_context()->set_user_data(bb::ShaderStage::Vertex, 0, 0x20000);
        assert(mem.store(dcb_addr, draw_cmds, sizeof(draw_cmds)) == MemoryError::ok);
        assert(call("sceGnmSubmitCommandBuffers", 1, dcb_ptr_table, dcb_size_table) == 0);
        assert(rt.submit_count() == 4);

        assert(call("sceVideoOutClose", vh) == 0);
    }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 1;
    }
}
