#ifndef BB_NET_RTC_HLE_H
#define BB_NET_RTC_HLE_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>
#include <mutex>

namespace bb {

class GuestMemory;

// ============================================================================
// libSceRtc - Códigos de Retorno, Constantes e Estruturas
// ============================================================================

constexpr int32_t SCE_RTC_OK                      = 0;
constexpr int32_t SCE_RTC_ERROR_INVALID_VALUE     = static_cast<int32_t>(0x80250001);
constexpr int32_t SCE_RTC_ERROR_INVALID_POINTER   = static_cast<int32_t>(0x80250002);
constexpr int32_t SCE_RTC_ERROR_NOT_INITIALIZED   = static_cast<int32_t>(0x80250003);
constexpr int32_t SCE_RTC_ERROR_BAD_PARSE         = static_cast<int32_t>(0x80250004);

// Resolução padrão do tick no Orbis OS: 1.000.000 ticks/segundo (1 tick = 1 microssegundo)
constexpr uint64_t SCE_RTC_TICK_RESOLUTION        = 1000000ULL;

#pragma pack(push, 1)

// Tick de 64-bit monotônico de alta precisão (em microssegundos)
struct SceRtcTick {
    uint64_t tick{0};
};

// Estrutura de data/hora do Orbis OS (ano, mês, dia, hora, minuto, segundo, microssegundo)
struct SceRtcDateTime {
    uint16_t year{0};
    uint16_t month{0};
    uint16_t day{0};
    uint16_t hour{0};
    uint16_t minute{0};
    uint16_t second{0};
    uint32_t microsecond{0};
};

#pragma pack(pop)

static_assert(sizeof(SceRtcTick) == 8, "SceRtcTick layout");
static_assert(sizeof(SceRtcDateTime) == 16, "SceRtcDateTime layout");

// ============================================================================
// libSceNet / libSceNetCtl - Códigos de Retorno, Constantes e Estruturas
// ============================================================================

constexpr int32_t SCE_NET_OK                           = 0;
constexpr int32_t SCE_NET_ERROR_EPERM                  = static_cast<int32_t>(0x80410101);
constexpr int32_t SCE_NET_ERROR_ENOENT                 = static_cast<int32_t>(0x80410102);
constexpr int32_t SCE_NET_ERROR_EBADF                  = static_cast<int32_t>(0x80410109);
constexpr int32_t SCE_NET_ERROR_EFAULT                 = static_cast<int32_t>(0x8041010E);
constexpr int32_t SCE_NET_ERROR_EINVAL                 = static_cast<int32_t>(0x80410116);
constexpr int32_t SCE_NET_ERROR_ENETUNREACH            = static_cast<int32_t>(0x80410133);
constexpr int32_t SCE_NET_ERROR_ENOTCONN               = static_cast<int32_t>(0x80410139);
constexpr int32_t SCE_NET_ERROR_ECONNREFUSED           = static_cast<int32_t>(0x8041013D);
constexpr int32_t SCE_NET_ERROR_ALREADY_INITIALIZED    = static_cast<int32_t>(0x80410180);
constexpr int32_t SCE_NET_ERROR_NOT_INITIALIZED        = static_cast<int32_t>(0x80410181);

// Aliases canônicos
constexpr int32_t SCE_NET_ENETUNREACH                  = SCE_NET_ERROR_ENETUNREACH;
constexpr int32_t SCE_NET_EINVAL                       = SCE_NET_ERROR_EINVAL;
constexpr int32_t SCE_NET_EFAULT                       = SCE_NET_ERROR_EFAULT;
constexpr int32_t SCE_NET_EBADF                        = SCE_NET_ERROR_EBADF;

// Estados de controle de rede (SceNetCtlState)
constexpr int32_t SCE_NET_CTL_STATE_DISCONNECTED       = 0;
constexpr int32_t SCE_NET_CTL_STATE_CONNECTING         = 1;
constexpr int32_t SCE_NET_CTL_STATE_IPOBTAINING        = 2;
constexpr int32_t SCE_NET_CTL_STATE_IPOBTAINED         = 3;
constexpr int32_t SCE_NET_CTL_STATE_CONNECTED          = 4;

// Códigos de erro do SceNetCtl
constexpr int32_t SCE_NET_CTL_ERROR_NOT_INITIALIZED    = static_cast<int32_t>(0x80430101);
constexpr int32_t SCE_NET_CTL_ERROR_INVALID_CODE       = static_cast<int32_t>(0x80430102);
constexpr int32_t SCE_NET_CTL_ERROR_INVALID_POINTER    = static_cast<int32_t>(0x80430103);

// Identificadores de consulta do SceNetCtlInfo
constexpr int32_t SCE_NET_CTL_INFO_DEVICE              = 1;
constexpr int32_t SCE_NET_CTL_INFO_ETHER_ADDR          = 2;
constexpr int32_t SCE_NET_CTL_INFO_MTU                 = 3;
constexpr int32_t SCE_NET_CTL_INFO_LINK                = 4;
constexpr int32_t SCE_NET_CTL_INFO_IP_CONFIG           = 5;
constexpr int32_t SCE_NET_CTL_INFO_DHCP_HOSTNAME       = 6;
constexpr int32_t SCE_NET_CTL_INFO_PPPOE_AUTH_NAME     = 7;
constexpr int32_t SCE_NET_CTL_INFO_IP_ADDRESS          = 8;
constexpr int32_t SCE_NET_CTL_INFO_NETMASK             = 9;
constexpr int32_t SCE_NET_CTL_INFO_DEFAULT_ROUTE       = 10;
constexpr int32_t SCE_NET_CTL_INFO_PRIMARY_DNS         = 11;
constexpr int32_t SCE_NET_CTL_INFO_SECONDARY_DNS       = 12;
constexpr int32_t SCE_NET_CTL_INFO_HTTP_PROXY_CONFIG   = 13;
constexpr int32_t SCE_NET_CTL_INFO_HTTP_PROXY_SERVER   = 14;
constexpr int32_t SCE_NET_CTL_INFO_HTTP_PROXY_PORT     = 15;
constexpr int32_t SCE_NET_CTL_INFO_BSSID               = 16;
constexpr int32_t SCE_NET_CTL_INFO_SSID                = 17;
constexpr int32_t SCE_NET_CTL_INFO_WIFI_SECURITY       = 18;
constexpr int32_t SCE_NET_CTL_INFO_RSSI_PERCENTAGE     = 19;
constexpr int32_t SCE_NET_CTL_INFO_CHANNEL             = 20;
constexpr int32_t SCE_NET_CTL_INFO_IP_ADDRESS_V6       = 21;
constexpr int32_t SCE_NET_CTL_INFO_NAT_TYPE            = 22;

#pragma pack(push, 1)

struct SceNetEtherAddr {
    uint8_t data[6]{0};
    uint8_t padding[2]{0};
};

union SceNetCtlInfo {
    uint32_t device;
    SceNetEtherAddr ether_addr;
    uint32_t mtu;
    uint32_t link;
    uint32_t ip_config;
    char dhcp_hostname[256];
    char pppoe_auth_name[128];
    char ip_address[16];
    char netmask[16];
    char default_route[16];
    char primary_dns[16];
    char secondary_dns[16];
    uint32_t http_proxy_config;
    char http_proxy_server[256];
    uint32_t http_proxy_port;
    uint8_t bssid[6];
    char ssid[33];
    uint32_t wifi_security;
    uint32_t rssi_percentage;
    uint32_t channel;
    uint32_t nat_type;
    uint8_t raw[256];
};

#pragma pack(pop)

static_assert(sizeof(SceNetEtherAddr) == 8, "SceNetEtherAddr layout");
static_assert(sizeof(SceNetCtlInfo) == 256, "SceNetCtlInfo layout");

// ============================================================================
// libSceNp - Códigos de Retorno, Constantes e Estruturas
// ============================================================================

constexpr int32_t SCE_NP_OK                            = 0;
constexpr int32_t SCE_NP_ERROR_NOT_SIGNED_UP           = static_cast<int32_t>(0x80550001);
constexpr int32_t SCE_NP_ERROR_NOT_INITIALIZED         = static_cast<int32_t>(0x80550002);
constexpr int32_t SCE_NP_ERROR_INVALID_ARGUMENT        = static_cast<int32_t>(0x80550003);
constexpr int32_t SCE_NP_ERROR_OUT_OF_MEMORY           = static_cast<int32_t>(0x80550004);
constexpr int32_t SCE_NP_ERROR_SIGNED_OUT              = static_cast<int32_t>(0x80550005);

constexpr int32_t SCE_NP_STATE_UNKNOWN                 = 0;
constexpr int32_t SCE_NP_STATE_SIGNED_OUT              = 1;
constexpr int32_t SCE_NP_STATE_SIGNED_IN               = 2;

#pragma pack(push, 1)

// Identificador NP padrão (32 bytes)
struct SceNpId {
    char handle[16]{0};
    uint8_t opt[8]{0};
    uint8_t reserved[8]{0};
};

// Identificador Online NP (20 bytes)
struct SceNpOnlineId {
    char data[16]{0};
    char term{0};
    char dummy[3]{0};
};

#pragma pack(pop)

static_assert(sizeof(SceNpId) == 32, "SceNpId layout");
static_assert(sizeof(SceNpOnlineId) == 20, "SceNpOnlineId layout");

// ============================================================================
// RtcManager
// ============================================================================

class RtcManager {
public:
    using TimeProvider = std::function<uint64_t()>;

    RtcManager();
    ~RtcManager() = default;

    int32_t sceRtcGetCurrentTick(SceRtcTick* tick);
    int32_t sceRtcGetCurrentTick(GuestMemory& mem, uint64_t tick_addr);

    int32_t sceRtcGetCurrentClock(SceRtcDateTime* clock, int32_t time_zone = 0);
    int32_t sceRtcGetCurrentClock(GuestMemory& mem, uint64_t clock_addr, int32_t time_zone = 0);

    int32_t sceRtcGetCurrentClockLocalTime(SceRtcDateTime* clock);
    int32_t sceRtcGetCurrentClockLocalTime(GuestMemory& mem, uint64_t clock_addr);

    int32_t sceRtcGetCurrentNetworkTick(SceRtcTick* tick);
    int32_t sceRtcGetCurrentNetworkTick(GuestMemory& mem, uint64_t tick_addr);

    int32_t sceRtcConvertUtcToLocalTime(const SceRtcTick* utc_tick, SceRtcTick* local_tick);
    int32_t sceRtcConvertUtcToLocalTime(GuestMemory& mem, uint64_t utc_tick_addr, uint64_t local_tick_addr);

    int32_t sceRtcConvertLocalTimeToUtc(const SceRtcTick* local_tick, SceRtcTick* utc_tick);
    int32_t sceRtcConvertLocalTimeToUtc(GuestMemory& mem, uint64_t local_tick_addr, uint64_t utc_tick_addr);

    int32_t sceRtcGetTickResolution(uint32_t* resolution);
    int32_t sceRtcGetTickResolution(GuestMemory& mem, uint64_t resolution_addr);

    // Configuração determinística para testes
    void set_time_provider(TimeProvider provider);
    void reset_time_provider();
    void set_timezone_offset_minutes(int32_t minutes);
    int32_t get_timezone_offset_minutes() const;
    void set_base_tick(uint64_t base_us);
    void reset();

    // Métodos estáticos de conversão
    static bool tick_to_datetime(uint64_t ticks_us, SceRtcDateTime& dt);
    static bool datetime_to_tick(const SceRtcDateTime& dt, uint64_t& ticks_us);
    static bool is_leap_year(uint16_t year);
    static uint16_t days_in_month(uint16_t year, uint16_t month);

private:
    uint64_t current_ticks_us_locked();

    mutable std::mutex mutex_;
    TimeProvider time_provider_{nullptr};
    int32_t tz_offset_minutes_{0};
    uint64_t base_tick_{0};
    uint64_t last_tick_{0};
};

// ============================================================================
// NetHleManager
// ============================================================================

class NetHleManager {
public:
    NetHleManager();
    ~NetHleManager() = default;

    int32_t sceNetInit();
    int32_t sceNetTerm();
    bool is_initialized() const;

    // libSceNetCtl
    int32_t sceNetCtlInit();
    int32_t sceNetCtlTerm();
    bool is_ctl_initialized() const;
    int32_t sceNetCtlGetState(int32_t* state);
    int32_t sceNetCtlGetState(GuestMemory& mem, uint64_t state_addr);
    int32_t sceNetCtlGetInfo(int32_t code, SceNetCtlInfo* info);
    int32_t sceNetCtlGetInfo(GuestMemory& mem, int32_t code, uint64_t info_addr);
    int32_t sceNetCtlCheckCallback();

    // Stubs seguros de rede
    int32_t sceNetSocket(int32_t domain, int32_t type, int32_t protocol);
    int32_t sceNetSocketClose(int32_t s);
    int32_t sceNetConnect(int32_t s, const void* name, uint32_t namelen);
    int32_t sceNetConnect(GuestMemory& mem, int32_t s, uint64_t name_addr, uint32_t namelen);
    int32_t sceNetSend(int32_t s, const void* msg, std::size_t len, int32_t flags);
    int32_t sceNetSend(GuestMemory& mem, int32_t s, uint64_t msg_addr, std::size_t len, int32_t flags);
    int32_t sceNetRecv(int32_t s, void* buf, std::size_t len, int32_t flags);
    int32_t sceNetRecv(GuestMemory& mem, int32_t s, uint64_t buf_addr, std::size_t len, int32_t flags);

    // Configuração
    void set_state(int32_t state);
    int32_t get_state() const;
    void reset();

private:
    mutable std::mutex mutex_;
    bool net_initialized_{false};
    bool netctl_initialized_{false};
    int32_t state_{SCE_NET_CTL_STATE_DISCONNECTED};
};

// ============================================================================
// NpHleManager
// ============================================================================

class NpHleManager {
public:
    NpHleManager();
    ~NpHleManager() = default;

    int32_t sceNpGetState(int32_t* state);
    int32_t sceNpGetState(GuestMemory& mem, uint64_t state_addr);

    int32_t sceNpGetNpId(SceNpId* np_id);
    int32_t sceNpGetNpId(GuestMemory& mem, uint64_t np_id_addr);

    int32_t sceNpGetOnlineId(SceNpOnlineId* online_id);
    int32_t sceNpGetOnlineId(GuestMemory& mem, uint64_t online_id_addr);

    int32_t sceNpCheckCallback();

    // Configuração
    void set_state(int32_t state, int32_t return_code = SCE_NP_ERROR_NOT_SIGNED_UP);
    void set_offline_user_id(const std::string& name);
    std::string get_offline_user_id() const;
    void reset();

private:
    mutable std::mutex mutex_;
    int32_t state_{SCE_NP_STATE_SIGNED_OUT};
    int32_t return_code_{SCE_NP_ERROR_NOT_SIGNED_UP};
    std::string offline_user_id_{"Hunter"};
};

// Instâncias globais padrão HLE
RtcManager& default_rtc();
NetHleManager& default_net();
NpHleManager& default_np();

// Funções C / Wrappers delegando para instâncias padrão
int32_t sceRtcGetCurrentTick(SceRtcTick* tick);
int32_t sceRtcGetCurrentTick(GuestMemory& mem, uint64_t tick_addr);
int32_t sceRtcGetCurrentClock(SceRtcDateTime* clock, int32_t time_zone = 0);
int32_t sceRtcGetCurrentClock(GuestMemory& mem, uint64_t clock_addr, int32_t time_zone = 0);
int32_t sceRtcGetCurrentClockLocalTime(SceRtcDateTime* clock);
int32_t sceRtcGetCurrentClockLocalTime(GuestMemory& mem, uint64_t clock_addr);
int32_t sceRtcGetCurrentNetworkTick(SceRtcTick* tick);
int32_t sceRtcGetCurrentNetworkTick(GuestMemory& mem, uint64_t tick_addr);
int32_t sceRtcConvertUtcToLocalTime(const SceRtcTick* utc_tick, SceRtcTick* local_tick);
int32_t sceRtcConvertUtcToLocalTime(GuestMemory& mem, uint64_t utc_tick_addr, uint64_t local_tick_addr);
int32_t sceRtcConvertLocalTimeToUtc(const SceRtcTick* local_tick, SceRtcTick* utc_tick);
int32_t sceRtcConvertLocalTimeToUtc(GuestMemory& mem, uint64_t local_tick_addr, uint64_t utc_tick_addr);

int32_t sceNetInit();
int32_t sceNetTerm();
int32_t sceNetCtlInit();
int32_t sceNetCtlTerm();
int32_t sceNetCtlGetState(int32_t* state);
int32_t sceNetCtlGetState(GuestMemory& mem, uint64_t state_addr);
int32_t sceNetCtlGetInfo(int32_t code, SceNetCtlInfo* info);
int32_t sceNetCtlGetInfo(GuestMemory& mem, int32_t code, uint64_t info_addr);
int32_t sceNetCtlCheckCallback();
int32_t sceNetSocket(int32_t domain, int32_t type, int32_t protocol);
int32_t sceNetSocketClose(int32_t s);
int32_t sceNetConnect(int32_t s, const void* name, uint32_t namelen);
int32_t sceNetConnect(GuestMemory& mem, int32_t s, uint64_t name_addr, uint32_t namelen);
int32_t sceNetSend(int32_t s, const void* msg, std::size_t len, int32_t flags);
int32_t sceNetSend(GuestMemory& mem, int32_t s, uint64_t msg_addr, std::size_t len, int32_t flags);
int32_t sceNetRecv(int32_t s, void* buf, std::size_t len, int32_t flags);
int32_t sceNetRecv(GuestMemory& mem, int32_t s, uint64_t buf_addr, std::size_t len, int32_t flags);

int32_t sceNpGetState(int32_t* state);
int32_t sceNpGetState(GuestMemory& mem, uint64_t state_addr);
int32_t sceNpGetNpId(SceNpId* np_id);
int32_t sceNpGetNpId(GuestMemory& mem, uint64_t np_id_addr);
int32_t sceNpGetOnlineId(SceNpOnlineId* online_id);
int32_t sceNpGetOnlineId(GuestMemory& mem, uint64_t online_id_addr);
int32_t sceNpCheckCallback();

} // namespace bb

#endif // BB_NET_RTC_HLE_H
