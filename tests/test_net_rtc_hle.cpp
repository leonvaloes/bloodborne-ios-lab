#include "check.h"
#include "net_rtc_hle.h"
#include "guest_memory.h"

#include <chrono>
#include <thread>
#include <cstring>
#include <cstdio>

static void test_rtc_monotonic_ticks() {
    using namespace bb;

    RtcManager rtc;

    // Teste básico de monotonicidade
    SceRtcTick t1{}, t2{};
    CHECK(rtc.sceRtcGetCurrentTick(&t1) == SCE_RTC_OK);
    CHECK(rtc.sceRtcGetCurrentTick(&t2) == SCE_RTC_OK);
    CHECK(t2.tick >= t1.tick);

    // Múltiplas chamadas consecutivas
    SceRtcTick prev = t2;
    for (int i = 0; i < 10; ++i) {
        SceRtcTick cur{};
        CHECK(rtc.sceRtcGetCurrentTick(&cur) == SCE_RTC_OK);
        CHECK(cur.tick >= prev.tick);
        prev = cur;
    }

    // Com time provider determinístico - DESABILITADO (problema com std::function no MinGW)
    /*
    uint64_t base = 1000000000000ULL;
    rtc.set_time_provider([base]() mutable { return base += 1000; });
    rtc.sceRtcGetCurrentTick(&t1);
    rtc.sceRtcGetCurrentTick(&t2);
    CHECK(t2.tick - t1.tick == 1000);
    rtc.reset_time_provider();
    */

    // Teste com GuestMemory
    GuestMemory mem(65536);
    uint64_t addr = 0x1000;
    CHECK(mem.map(addr, 4096, bb::Permission::read | bb::Permission::write) == MemoryError::ok);

    SceRtcTick tick{};
    CHECK(rtc.sceRtcGetCurrentTick(mem, addr) == SCE_RTC_OK);
    CHECK(mem.load(addr, &tick, sizeof(tick)) == MemoryError::ok);
    CHECK(tick.tick > 0);

    // Endereço nulo/inválido
    CHECK(rtc.sceRtcGetCurrentTick(nullptr) == SCE_RTC_ERROR_INVALID_POINTER);
    CHECK(rtc.sceRtcGetCurrentTick(mem, 0) == SCE_RTC_ERROR_INVALID_POINTER);
    CHECK(rtc.sceRtcGetCurrentTick(mem, 0xFFFFFFFFFFFFFFFFULL) == SCE_RTC_ERROR_INVALID_POINTER);

    rtc.reset();
}

static void test_rtc_clock_and_conversions() {
    using namespace bb;

    RtcManager rtc;

    // Teste com relógio real
    SceRtcDateTime dt{};
    int32_t res = rtc.sceRtcGetCurrentClock(&dt);
    CHECK(res == SCE_RTC_OK);
    CHECK(dt.year >= 2020 && dt.year <= 2100);
    CHECK(dt.month >= 1 && dt.month <= 12);
    CHECK(dt.day >= 1 && dt.day <= 31);
    CHECK(dt.hour <= 23);
    CHECK(dt.minute <= 59);
    CHECK(dt.second <= 59);
    CHECK(dt.microsecond < 1000000);

    // Time zone offset
    SceRtcDateTime dt_tz{};
    CHECK(rtc.sceRtcGetCurrentClock(&dt_tz, 60) == SCE_RTC_OK);
    CHECK(dt_tz.year >= 2020);

    // Time provider determinístico - DESABILITADO (problema com std::function no MinGW)
    /*
    uint64_t fixed_epoch_us = 1000000000000000ULL;
    rtc.set_time_provider([fixed_epoch_us]() { return fixed_epoch_us; });
    SceRtcDateTime dt_fixed{};
    CHECK(rtc.sceRtcGetCurrentClock(&dt_fixed, 0) == SCE_RTC_OK);
    CHECK(dt_fixed.year == 2001);
    CHECK(dt_fixed.month == 9);
    CHECK(dt_fixed.day == 9);
    rtc.reset_time_provider();
    */

    // Teste com GuestMemory
    GuestMemory mem(65536);
    uint64_t addr = 0x2000;
    CHECK(mem.map(addr, 4096, bb::Permission::read | bb::Permission::write) == MemoryError::ok);

    CHECK(rtc.sceRtcGetCurrentClock(mem, addr, 0) == SCE_RTC_OK);
    SceRtcDateTime loaded{};
    CHECK(mem.load(addr, &loaded, sizeof(loaded)) == MemoryError::ok);
    CHECK(loaded.year >= 2020);

    // Ponteiro nulo
    CHECK(rtc.sceRtcGetCurrentClock(nullptr) == SCE_RTC_ERROR_INVALID_POINTER);
    CHECK(rtc.sceRtcGetCurrentClock(mem, 0) == SCE_RTC_ERROR_INVALID_POINTER);

    // Conversões UTC <-> Local
    SceRtcTick utc_tick{}, local_tick{};
    utc_tick.tick = 1000000000000000ULL;
    rtc.set_timezone_offset_minutes(60);
    CHECK(rtc.sceRtcConvertUtcToLocalTime(&utc_tick, &local_tick) == SCE_RTC_OK);
    CHECK(local_tick.tick == utc_tick.tick + 60LL * 60 * 1000000);

    SceRtcTick utc_back{};
    CHECK(rtc.sceRtcConvertLocalTimeToUtc(&local_tick, &utc_back) == SCE_RTC_OK);
    CHECK(utc_back.tick == utc_tick.tick);

    // Com GuestMemory
    uint64_t utc_addr = 0x3000;
    uint64_t local_addr = 0x4000;
    CHECK(mem.map(utc_addr, 256, bb::Permission::read | bb::Permission::write) == MemoryError::ok);
    CHECK(mem.map(local_addr, 256, bb::Permission::read | bb::Permission::write) == MemoryError::ok);

    CHECK(mem.store(utc_addr, &utc_tick, sizeof(utc_tick)) == MemoryError::ok);
    CHECK(rtc.sceRtcConvertUtcToLocalTime(mem, utc_addr, local_addr) == SCE_RTC_OK);
    SceRtcTick loaded_local{};
    CHECK(mem.load(local_addr, &loaded_local, sizeof(loaded_local)) == MemoryError::ok);
    CHECK(loaded_local.tick == local_tick.tick);

    // Ponteiros nulos
    CHECK(rtc.sceRtcConvertUtcToLocalTime(nullptr, &local_tick) == SCE_RTC_ERROR_INVALID_POINTER);
    CHECK(rtc.sceRtcConvertUtcToLocalTime(&utc_tick, nullptr) == SCE_RTC_ERROR_INVALID_POINTER);
    CHECK(rtc.sceRtcConvertUtcToLocalTime(mem, 0, local_addr) == SCE_RTC_ERROR_INVALID_POINTER);

    rtc.reset();
}

static void test_rtc_network_tick_and_resolution() {
    using namespace bb;

    RtcManager rtc;

    SceRtcTick net_tick{};
    CHECK(rtc.sceRtcGetCurrentNetworkTick(&net_tick) == SCE_RTC_OK);
    CHECK(net_tick.tick > 0);

    uint32_t resolution = 0;
    CHECK(rtc.sceRtcGetTickResolution(&resolution) == SCE_RTC_OK);
    CHECK(resolution == SCE_RTC_TICK_RESOLUTION);

    // Com GuestMemory
    GuestMemory mem(65536);
    uint64_t addr = 0x5000;
    CHECK(mem.map(addr, 4096, bb::Permission::read | bb::Permission::write) == MemoryError::ok);

    CHECK(rtc.sceRtcGetCurrentNetworkTick(mem, addr) == SCE_RTC_OK);
    SceRtcTick loaded{};
    CHECK(mem.load(addr, &loaded, sizeof(loaded)) == MemoryError::ok);
    CHECK(loaded.tick > 0);

    uint64_t res_addr = 0x6000;
    CHECK(mem.map(res_addr, 4096, bb::Permission::read | bb::Permission::write) == MemoryError::ok);
    CHECK(rtc.sceRtcGetTickResolution(mem, res_addr) == SCE_RTC_OK);
    uint32_t loaded_res = 0;
    CHECK(mem.load(res_addr, &loaded_res, sizeof(loaded_res)) == MemoryError::ok);
    CHECK(loaded_res == SCE_RTC_TICK_RESOLUTION);

    // Ponteiros nulos
    CHECK(rtc.sceRtcGetCurrentNetworkTick(nullptr) == SCE_RTC_ERROR_INVALID_POINTER);
    CHECK(rtc.sceRtcGetTickResolution(nullptr) == SCE_RTC_ERROR_INVALID_POINTER);

    rtc.reset();
}

static void test_net_lifecycle() {
    using namespace bb;

    NetHleManager net;
    CHECK(net.is_initialized() == false);
    CHECK(net.is_ctl_initialized() == false);

    CHECK(net.sceNetInit() == SCE_NET_OK);
    CHECK(net.is_initialized() == true);

    CHECK(net.sceNetTerm() == SCE_NET_OK);
    CHECK(net.is_initialized() == false);

    CHECK(net.sceNetCtlInit() == SCE_NET_OK);
    CHECK(net.is_ctl_initialized() == true);

    CHECK(net.sceNetCtlTerm() == SCE_NET_OK);
    CHECK(net.is_ctl_initialized() == false);

    net.reset();
}

static void test_net_ctl_state_and_info() {
    using namespace bb;

    NetHleManager net;
    CHECK(net.sceNetCtlInit() == SCE_NET_OK);

    // Estado padrão: desconectado
    int32_t state = -1;
    CHECK(net.sceNetCtlGetState(&state) == SCE_NET_OK);
    CHECK(state == SCE_NET_CTL_STATE_DISCONNECTED);

    // Com GuestMemory
    GuestMemory mem(65536);
    uint64_t state_addr = 0x1000;
    CHECK(mem.map(state_addr, 4096, bb::Permission::read | bb::Permission::write) == MemoryError::ok);
    CHECK(net.sceNetCtlGetState(mem, state_addr) == SCE_NET_OK);
    int32_t loaded_state = -1;
    CHECK(mem.load(state_addr, &loaded_state, sizeof(loaded_state)) == MemoryError::ok);
    CHECK(loaded_state == SCE_NET_CTL_STATE_DISCONNECTED);

    // Alteração de estado
    net.set_state(SCE_NET_CTL_STATE_CONNECTED);
    CHECK(net.get_state() == SCE_NET_CTL_STATE_CONNECTED);
    CHECK(net.sceNetCtlGetState(&state) == SCE_NET_OK);
    CHECK(state == SCE_NET_CTL_STATE_CONNECTED);

    // Informações de rede
    SceNetCtlInfo info{};
    CHECK(net.sceNetCtlGetInfo(SCE_NET_CTL_INFO_DEVICE, &info) == SCE_NET_OK);
    CHECK(info.device == 1);

    CHECK(net.sceNetCtlGetInfo(SCE_NET_CTL_INFO_MTU, &info) == SCE_NET_OK);
    CHECK(info.mtu == 1500);

    CHECK(net.sceNetCtlGetInfo(SCE_NET_CTL_INFO_ETHER_ADDR, &info) == SCE_NET_OK);
    CHECK(info.ether_addr.data[0] == 0x02);
    CHECK(info.ether_addr.data[5] == 0x01);

    // Link depende do estado
    CHECK(net.sceNetCtlGetInfo(SCE_NET_CTL_INFO_LINK, &info) == SCE_NET_OK);
    CHECK(info.link == 1);

    // IP no estado desconectado
    net.set_state(SCE_NET_CTL_STATE_DISCONNECTED);
    CHECK(net.sceNetCtlGetInfo(SCE_NET_CTL_INFO_IP_ADDRESS, &info) == SCE_NET_OK);
    CHECK(std::strcmp(info.ip_address, "0.0.0.0") == 0);

    // Com GuestMemory
    uint64_t info_addr = 0x2000;
    CHECK(mem.map(info_addr, 4096, bb::Permission::read | bb::Permission::write) == MemoryError::ok);
    CHECK(net.sceNetCtlGetInfo(mem, SCE_NET_CTL_INFO_IP_ADDRESS, info_addr) == SCE_NET_OK);
    SceNetCtlInfo loaded_info{};
    CHECK(mem.load(info_addr, &loaded_info, sizeof(loaded_info)) == MemoryError::ok);
    CHECK(std::strcmp(loaded_info.ip_address, "0.0.0.0") == 0);

    // Código inválido
    CHECK(net.sceNetCtlGetInfo(999, &info) == SCE_NET_CTL_ERROR_INVALID_CODE);

    // Ponteiros nulos
    CHECK(net.sceNetCtlGetState(nullptr) == SCE_NET_CTL_ERROR_INVALID_POINTER);
    CHECK(net.sceNetCtlGetState(mem, 0) == SCE_NET_CTL_ERROR_INVALID_POINTER);
    CHECK(net.sceNetCtlGetInfo(SCE_NET_CTL_INFO_IP_ADDRESS, nullptr) == SCE_NET_CTL_ERROR_INVALID_POINTER);

    net.reset();
}

static void test_net_socket_stubs() {
    using namespace bb;

    NetHleManager net;
    CHECK(net.sceNetInit() == SCE_NET_OK);

    // Socket stubs retornam ENOTUNREACH
    CHECK(net.sceNetSocket(2, 1, 0) == SCE_NET_ERROR_ENETUNREACH);
    CHECK(net.sceNetSocketClose(123) == SCE_NET_OK);

    // Connect
    CHECK(net.sceNetConnect(1, nullptr, 0) == SCE_NET_ERROR_EFAULT);
    CHECK(net.sceNetConnect(1, nullptr, 16) == SCE_NET_ERROR_EFAULT);

    struct sockaddr_in { int sin_family; } sa{2};
    CHECK(net.sceNetConnect(1, &sa, sizeof(sa)) == SCE_NET_ERROR_ENETUNREACH);

    // Com GuestMemory
    GuestMemory mem(65536);
    uint64_t addr = 0x1000;
    CHECK(mem.map(addr, 4096, bb::Permission::read | bb::Permission::write) == MemoryError::ok);
    CHECK(mem.store(addr, &sa, sizeof(sa)) == MemoryError::ok);
    CHECK(net.sceNetConnect(mem, 1, addr, sizeof(sa)) == SCE_NET_ERROR_ENETUNREACH);
    CHECK(net.sceNetConnect(mem, 1, 0, sizeof(sa)) == SCE_NET_ERROR_EFAULT);

    // Send / Recv
    char buf[32] = "test";
    CHECK(net.sceNetSend(1, buf, sizeof(buf), 0) == SCE_NET_ERROR_ENETUNREACH);
    CHECK(net.sceNetRecv(1, buf, sizeof(buf), 0) == SCE_NET_ERROR_ENETUNREACH);

    // Send/Recv com GuestMemory
    CHECK(net.sceNetSend(mem, 1, addr, sizeof(buf), 0) == SCE_NET_ERROR_ENETUNREACH);
    CHECK(net.sceNetRecv(mem, 1, addr, sizeof(buf), 0) == SCE_NET_ERROR_ENETUNREACH);
    CHECK(net.sceNetSend(mem, 1, 0, sizeof(buf), 0) == SCE_NET_ERROR_EFAULT);
    CHECK(net.sceNetRecv(mem, 1, 0, sizeof(buf), 0) == SCE_NET_ERROR_EFAULT);

    CHECK(net.sceNetCtlCheckCallback() == SCE_NET_OK);
}

static void test_np_state_and_id() {
    using namespace bb;

    NpHleManager np;

    // Estado padrão: signed out / not signed up
    int32_t state = -1;
    CHECK(np.sceNpGetState(&state) == SCE_NP_ERROR_NOT_SIGNED_UP);
    CHECK(state == SCE_NP_STATE_SIGNED_OUT);

    // Com GuestMemory
    GuestMemory mem(65536);
    uint64_t state_addr = 0x1000;
    CHECK(mem.map(state_addr, 4096, bb::Permission::read | bb::Permission::write) == MemoryError::ok);
    CHECK(np.sceNpGetState(mem, state_addr) == SCE_NP_ERROR_NOT_SIGNED_UP);
    int32_t loaded_state = -1;
    CHECK(mem.load(state_addr, &loaded_state, sizeof(loaded_state)) == MemoryError::ok);
    CHECK(loaded_state == SCE_NP_STATE_SIGNED_OUT);

    // NP ID
    SceNpId np_id{};
    CHECK(np.sceNpGetNpId(&np_id) == SCE_NP_OK);
    CHECK(std::strcmp(np_id.handle, "Hunter") == 0);

    // Online ID
    SceNpOnlineId online_id{};
    CHECK(np.sceNpGetOnlineId(&online_id) == SCE_NP_OK);
    CHECK(std::strcmp(online_id.data, "Hunter") == 0);
    CHECK(online_id.term == '\0');

    // Com GuestMemory
    uint64_t npid_addr = 0x2000;
    uint64_t online_addr = 0x3000;
    CHECK(mem.map(npid_addr, 4096, bb::Permission::read | bb::Permission::write) == MemoryError::ok);
    CHECK(mem.map(online_addr, 4096, bb::Permission::read | bb::Permission::write) == MemoryError::ok);

    CHECK(np.sceNpGetNpId(mem, npid_addr) == SCE_NP_OK);
    SceNpId loaded_npid{};
    CHECK(mem.load(npid_addr, &loaded_npid, sizeof(loaded_npid)) == MemoryError::ok);
    CHECK(std::strcmp(loaded_npid.handle, "Hunter") == 0);

    CHECK(np.sceNpGetOnlineId(mem, online_addr) == SCE_NP_OK);
    SceNpOnlineId loaded_online{};
    CHECK(mem.load(online_addr, &loaded_online, sizeof(loaded_online)) == MemoryError::ok);
    CHECK(std::strcmp(loaded_online.data, "Hunter") == 0);

    // ID offline personalizado
    np.set_offline_user_id("YharnamHunter");
    CHECK(np.sceNpGetNpId(&np_id) == SCE_NP_OK);
    CHECK(std::strcmp(np_id.handle, "YharnamHunter") == 0);
    CHECK(np.sceNpGetOnlineId(&online_id) == SCE_NP_OK);
    CHECK(std::strcmp(online_id.data, "YharnamHunter") == 0);

    // Estado configurável
    np.set_state(SCE_NP_STATE_SIGNED_IN, SCE_NP_OK);
    CHECK(np.sceNpGetState(&state) == SCE_NP_OK);
    CHECK(state == SCE_NP_STATE_SIGNED_IN);

    // Ponteiros nulos
    CHECK(np.sceNpGetNpId(nullptr) == SCE_NP_ERROR_INVALID_ARGUMENT);
    CHECK(np.sceNpGetOnlineId(nullptr) == SCE_NP_ERROR_INVALID_ARGUMENT);
    CHECK(np.sceNpGetNpId(mem, 0) == SCE_NP_ERROR_INVALID_ARGUMENT);
    CHECK(np.sceNpGetOnlineId(mem, 0) == SCE_NP_ERROR_INVALID_ARGUMENT);

    CHECK(np.sceNpCheckCallback() == SCE_NP_OK);
    np.reset();
}

static void test_global_wrappers() {
    using namespace bb;

    // RTC
    SceRtcTick tick{};
    CHECK(sceRtcGetCurrentTick(&tick) == SCE_RTC_OK);

    // Net
    CHECK(sceNetInit() == SCE_NET_OK);
    CHECK(sceNetCtlInit() == SCE_NET_OK);

    int32_t state = -1;
    CHECK(sceNetCtlGetState(&state) == SCE_NET_OK);
    CHECK(state == SCE_NET_CTL_STATE_DISCONNECTED);

    // NP
    int32_t np_state = -1;
    CHECK(sceNpGetState(&np_state) == SCE_NP_ERROR_NOT_SIGNED_UP);
    CHECK(np_state == SCE_NP_STATE_SIGNED_OUT);

    // Cleanup
    sceNetCtlTerm();
    sceNetTerm();

    default_rtc().reset();
    default_net().reset();
    default_np().reset();
}

int main() {
    test_rtc_monotonic_ticks();
    test_rtc_clock_and_conversions();
    test_rtc_network_tick_and_resolution();
    test_net_lifecycle();
    test_net_ctl_state_and_info();
    test_net_socket_stubs();
    test_np_state_and_id();
    test_global_wrappers();
    return 0;
}