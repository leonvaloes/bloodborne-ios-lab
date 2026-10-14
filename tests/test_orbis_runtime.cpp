#include "../core/orbis_runtime.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#define assert(condition) do { if (!(condition)) throw std::runtime_error("falha: " #condition); } while (false)

using namespace bb;
int main() {
    try {
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

    // =========================================================================
    // Testes de subsistemas HLE: libSceUserService, libScePad, libSceSaveData,
    // libSceAudioOut, Math, Conversões, Ordenação e snprintf
    // =========================================================================
    assert(m.map(0x5000, 0xb000, read | write) == MemoryError::ok);

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
    assert(call("sceSaveDataMount", 0x5440, 0x5500) == 0);
    SceSaveDataMountResult sres{};
    assert(m.load(0x5500, &sres, sizeof(sres)) == MemoryError::ok);
    assert(std::string(sres.point.data).find("/savedata") != std::string::npos);
    assert(m.store(0x5600, &sres.point, sizeof(sres.point)) == MemoryError::ok);
    assert(call("sceSaveDataUmount", 0x5600, 1) == 0);

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
    assert(m.store(0x5e00, "val=%d u=%u hex=%x s=%s", 25) == MemoryError::ok);
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

    // 6. _sceKernelRtldSetApplicationHeapAPI
    assert(call("_sceKernelRtldSetApplicationHeapAPI", 0x9000) == 0);

    // =========================================================================
    // Testes de máquina guest com GuestScheduler e adaptador scheduler-traps
    // =========================================================================
    {
        GuestMemory gm(2 * 1024 * 1024);
        assert(gm.map(0x10000, 0x10000, read | write) == MemoryError::ok); // code
        assert(gm.map(0x20000, 0x10000, read | write) == MemoryError::ok); // data
        assert(gm.map(0x40000, 0x10000, read | write) == MemoryError::ok); // main stack

        GuestScheduler sched(gm, 32);
        OrbisRuntime srt(gm);
        srt.attach_scheduler(sched);
        srt.set_procparam(0xfeedfaceULL);
        assert(srt.procparam() == 0xfeedfaceULL);

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

            assert(gm.map(0x60000, 0x10000, read | write) == MemoryError::ok);
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

            assert(gm.map(0x70000, 0x10000, read | write) == MemoryError::ok);
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

            assert(gm.map(0x80000, 0x10000, read | write) == MemoryError::ok);
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

            assert(gm.map(0x90000, 0x10000, read | write) == MemoryError::ok);
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

            assert(gm.map(0xa0000, 0x10000, read | write) == MemoryError::ok);
            auto tid = sched.create(0x10a00, 0xa0000, 0xb0000, return_trap);
            assert(tid && sched.start(tid) == ORBIS_OK);

            auto run_res = sched.run(500);
            assert(run_res.stop == GuestScheduler::Stop::idle);
            std::uint64_t first_res = 99, second_res = 99;
            assert(gm.load(0x20080, &first_res, 8) == MemoryError::ok && first_res == 1);
            assert(gm.load(0x20088, &second_res, 8) == MemoryError::ok && second_res == 0);
        }
    }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 1;
    }
}
