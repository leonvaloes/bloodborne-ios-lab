#include "net_rtc_hle.h"
#include "guest_memory.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace bb {

// ============================================================================
// RtcManager
// ============================================================================

RtcManager::RtcManager() = default;

bool RtcManager::is_leap_year(uint16_t year) {
    if (year % 4 != 0) {
        return false;
    }
    if (year % 100 != 0) {
        return true;
    }
    return (year % 400 == 0);
}

uint16_t RtcManager::days_in_month(uint16_t year, uint16_t month) {
    if (month < 1 || month > 12) {
        return 0;
    }
    static const uint16_t kDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return kDays[month - 1];
}

bool RtcManager::tick_to_datetime(uint64_t ticks_us, SceRtcDateTime& dt) {
    uint64_t total_us = ticks_us;
    uint32_t microsecond = static_cast<uint32_t>(total_us % 1000000ULL);
    uint64_t total_s = total_us / 1000000ULL;
    uint16_t second = static_cast<uint16_t>(total_s % 60);
    uint64_t total_m = total_s / 60;
    uint16_t minute = static_cast<uint16_t>(total_m % 60);
    uint64_t total_h = total_m / 60;
    uint16_t hour = static_cast<uint16_t>(total_h % 24);
    int64_t days = static_cast<int64_t>(total_h / 24);

    // Algoritmo civil gregoriano (Howard Hinnant) para conversão determinística de dias desde 1970-01-01
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = static_cast<int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) {
        y += 1;
    }

    if (y < 1 || y > 9999) {
        return false;
    }

    dt.year = static_cast<uint16_t>(y);
    dt.month = static_cast<uint16_t>(m);
    dt.day = static_cast<uint16_t>(d);
    dt.hour = hour;
    dt.minute = minute;
    dt.second = second;
    dt.microsecond = microsecond;
    return true;
}

bool RtcManager::datetime_to_tick(const SceRtcDateTime& dt, uint64_t& ticks_us) {
    if (dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 ||
        dt.hour > 23 || dt.minute > 59 || dt.second > 59 || dt.microsecond >= 1000000) {
        return false;
    }
    if (dt.day > days_in_month(dt.year, dt.month)) {
        return false;
    }

    int64_t y = static_cast<int64_t>(dt.year) - (dt.month <= 2 ? 1 : 0);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * (dt.month > 2 ? dt.month - 3 : dt.month + 9) + 2) / 5 + dt.day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;

    if (days < 0) {
        return false;
    }

    uint64_t total_s = static_cast<uint64_t>(days) * 86400ULL +
                       static_cast<uint64_t>(dt.hour) * 3600ULL +
                       static_cast<uint64_t>(dt.minute) * 60ULL +
                       static_cast<uint64_t>(dt.second);
    ticks_us = total_s * 1000000ULL + static_cast<uint64_t>(dt.microsecond);
    return true;
}

uint64_t RtcManager::current_ticks_us_locked() {
    uint64_t tick = 0;
    if (time_provider_) {
        tick = time_provider_();
    } else {
        auto now = std::chrono::steady_clock::now();
        auto duration = now.time_since_epoch();
        tick = base_tick_ + static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
    }
    if (tick <= last_tick_) {
        tick = last_tick_ + 1;
    }
    last_tick_ = tick;
    return tick;
}

int32_t RtcManager::sceRtcGetCurrentTick(SceRtcTick* tick) {
    if (tick == nullptr) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    tick->tick = current_ticks_us_locked();
    return SCE_RTC_OK;
}

int32_t RtcManager::sceRtcGetCurrentTick(GuestMemory& mem, uint64_t tick_addr) {
    if (tick_addr == 0) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    SceRtcTick tick{};
    int32_t res = sceRtcGetCurrentTick(&tick);
    if (res != SCE_RTC_OK) {
        return res;
    }
    if (mem.store(tick_addr, &tick, sizeof(tick)) != MemoryError::ok) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    return SCE_RTC_OK;
}

int32_t RtcManager::sceRtcGetCurrentClock(SceRtcDateTime* clock, int32_t time_zone) {
    if (clock == nullptr) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now_us = 0;
    if (time_provider_) {
        now_us = time_provider_();
    } else {
        auto sys_now = std::chrono::system_clock::now();
        now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(sys_now.time_since_epoch()).count());
    }

    int64_t adjusted_us = static_cast<int64_t>(now_us) + static_cast<int64_t>(time_zone) * 60LL * 1000000LL;
    if (adjusted_us < 0) {
        adjusted_us = 0;
    }

    if (!tick_to_datetime(static_cast<uint64_t>(adjusted_us), *clock)) {
        return SCE_RTC_ERROR_INVALID_VALUE;
    }
    return SCE_RTC_OK;
}

int32_t RtcManager::sceRtcGetCurrentClock(GuestMemory& mem, uint64_t clock_addr, int32_t time_zone) {
    if (clock_addr == 0) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    SceRtcDateTime dt{};
    int32_t res = sceRtcGetCurrentClock(&dt, time_zone);
    if (res != SCE_RTC_OK) {
        return res;
    }
    if (mem.store(clock_addr, &dt, sizeof(dt)) != MemoryError::ok) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    return SCE_RTC_OK;
}

int32_t RtcManager::sceRtcGetCurrentClockLocalTime(SceRtcDateTime* clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sceRtcGetCurrentClock(clock, tz_offset_minutes_);
}

int32_t RtcManager::sceRtcGetCurrentClockLocalTime(GuestMemory& mem, uint64_t clock_addr) {
    if (clock_addr == 0) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    SceRtcDateTime dt{};
    int32_t res = sceRtcGetCurrentClockLocalTime(&dt);
    if (res != SCE_RTC_OK) {
        return res;
    }
    if (mem.store(clock_addr, &dt, sizeof(dt)) != MemoryError::ok) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    return SCE_RTC_OK;
}

int32_t RtcManager::sceRtcGetCurrentNetworkTick(SceRtcTick* tick) {
    if (tick == nullptr) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    tick->tick = current_ticks_us_locked();
    return SCE_RTC_OK;
}

int32_t RtcManager::sceRtcGetCurrentNetworkTick(GuestMemory& mem, uint64_t tick_addr) {
    if (tick_addr == 0) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    SceRtcTick tick{};
    int32_t res = sceRtcGetCurrentNetworkTick(&tick);
    if (res != SCE_RTC_OK) {
        return res;
    }
    if (mem.store(tick_addr, &tick, sizeof(tick)) != MemoryError::ok) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    return SCE_RTC_OK;
}

int32_t RtcManager::sceRtcConvertUtcToLocalTime(const SceRtcTick* utc_tick, SceRtcTick* local_tick) {
    if (utc_tick == nullptr || local_tick == nullptr) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t offset_us = static_cast<int64_t>(tz_offset_minutes_) * 60LL * 1000000LL;
    int64_t val = static_cast<int64_t>(utc_tick->tick) + offset_us;
    if (val < 0) {
        val = 0;
    }
    local_tick->tick = static_cast<uint64_t>(val);
    return SCE_RTC_OK;
}

int32_t RtcManager::sceRtcConvertUtcToLocalTime(GuestMemory& mem, uint64_t utc_tick_addr, uint64_t local_tick_addr) {
    if (utc_tick_addr == 0 || local_tick_addr == 0) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    SceRtcTick utc_tick{};
    if (mem.load(utc_tick_addr, &utc_tick, sizeof(utc_tick)) != MemoryError::ok) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    SceRtcTick local_tick{};
    int32_t res = sceRtcConvertUtcToLocalTime(&utc_tick, &local_tick);
    if (res != SCE_RTC_OK) {
        return res;
    }
    if (mem.store(local_tick_addr, &local_tick, sizeof(local_tick)) != MemoryError::ok) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    return SCE_RTC_OK;
}

int32_t RtcManager::sceRtcConvertLocalTimeToUtc(const SceRtcTick* local_tick, SceRtcTick* utc_tick) {
    if (local_tick == nullptr || utc_tick == nullptr) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t offset_us = static_cast<int64_t>(tz_offset_minutes_) * 60LL * 1000000LL;
    int64_t val = static_cast<int64_t>(local_tick->tick) - offset_us;
    if (val < 0) {
        val = 0;
    }
    utc_tick->tick = static_cast<uint64_t>(val);
    return SCE_RTC_OK;
}

int32_t RtcManager::sceRtcConvertLocalTimeToUtc(GuestMemory& mem, uint64_t local_tick_addr, uint64_t utc_tick_addr) {
    if (local_tick_addr == 0 || utc_tick_addr == 0) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    SceRtcTick local_tick{};
    if (mem.load(local_tick_addr, &local_tick, sizeof(local_tick)) != MemoryError::ok) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    SceRtcTick utc_tick{};
    int32_t res = sceRtcConvertLocalTimeToUtc(&local_tick, &utc_tick);
    if (res != SCE_RTC_OK) {
        return res;
    }
    if (mem.store(utc_tick_addr, &utc_tick, sizeof(utc_tick)) != MemoryError::ok) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    return SCE_RTC_OK;
}

int32_t RtcManager::sceRtcGetTickResolution(uint32_t* resolution) {
    if (resolution == nullptr) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    *resolution = static_cast<uint32_t>(SCE_RTC_TICK_RESOLUTION);
    return SCE_RTC_OK;
}

int32_t RtcManager::sceRtcGetTickResolution(GuestMemory& mem, uint64_t resolution_addr) {
    if (resolution_addr == 0) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    uint32_t res = static_cast<uint32_t>(SCE_RTC_TICK_RESOLUTION);
    if (mem.store(resolution_addr, &res, sizeof(res)) != MemoryError::ok) {
        return SCE_RTC_ERROR_INVALID_POINTER;
    }
    return SCE_RTC_OK;
}

void RtcManager::set_time_provider(TimeProvider provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    time_provider_ = std::move(provider);
}

void RtcManager::reset_time_provider() {
    std::lock_guard<std::mutex> lock(mutex_);
    time_provider_ = nullptr;
}

void RtcManager::set_timezone_offset_minutes(int32_t minutes) {
    std::lock_guard<std::mutex> lock(mutex_);
    tz_offset_minutes_ = minutes;
}

int32_t RtcManager::get_timezone_offset_minutes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tz_offset_minutes_;
}

void RtcManager::set_base_tick(uint64_t base_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    base_tick_ = base_us;
}

void RtcManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    time_provider_ = nullptr;
    tz_offset_minutes_ = 0;
    base_tick_ = 0;
    last_tick_ = 0;
}

// ============================================================================
// NetHleManager
// ============================================================================

NetHleManager::NetHleManager() = default;

int32_t NetHleManager::sceNetInit() {
    std::lock_guard<std::mutex> lock(mutex_);
    net_initialized_ = true;
    return SCE_NET_OK;
}

int32_t NetHleManager::sceNetTerm() {
    std::lock_guard<std::mutex> lock(mutex_);
    net_initialized_ = false;
    return SCE_NET_OK;
}

bool NetHleManager::is_initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return net_initialized_;
}

int32_t NetHleManager::sceNetCtlInit() {
    std::lock_guard<std::mutex> lock(mutex_);
    netctl_initialized_ = true;
    return SCE_NET_OK;
}

int32_t NetHleManager::sceNetCtlTerm() {
    std::lock_guard<std::mutex> lock(mutex_);
    netctl_initialized_ = false;
    return SCE_NET_OK;
}

bool NetHleManager::is_ctl_initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return netctl_initialized_;
}

int32_t NetHleManager::sceNetCtlGetState(int32_t* state) {
    if (state == nullptr) {
        return SCE_NET_CTL_ERROR_INVALID_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    *state = state_;
    return SCE_NET_OK;
}

int32_t NetHleManager::sceNetCtlGetState(GuestMemory& mem, uint64_t state_addr) {
    if (state_addr == 0) {
        return SCE_NET_CTL_ERROR_INVALID_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (mem.store(state_addr, &state_, sizeof(state_)) != MemoryError::ok) {
        return SCE_NET_CTL_ERROR_INVALID_POINTER;
    }
    return SCE_NET_OK;
}

int32_t NetHleManager::sceNetCtlGetInfo(int32_t code, SceNetCtlInfo* info) {
    if (info == nullptr) {
        return SCE_NET_CTL_ERROR_INVALID_POINTER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    *info = SceNetCtlInfo{};

    switch (code) {
        case SCE_NET_CTL_INFO_DEVICE:
            info->device = 1;
            break;
        case SCE_NET_CTL_INFO_ETHER_ADDR:
            info->ether_addr.data[0] = 0x02;
            info->ether_addr.data[1] = 0x00;
            info->ether_addr.data[2] = 0x00;
            info->ether_addr.data[3] = 0x00;
            info->ether_addr.data[4] = 0x00;
            info->ether_addr.data[5] = 0x01;
            break;
        case SCE_NET_CTL_INFO_MTU:
            info->mtu = 1500;
            break;
        case SCE_NET_CTL_INFO_LINK:
            info->link = (state_ == SCE_NET_CTL_STATE_CONNECTED) ? 1 : 0;
            break;
        case SCE_NET_CTL_INFO_IP_CONFIG:
            info->ip_config = 0;
            break;
        case SCE_NET_CTL_INFO_DHCP_HOSTNAME:
            std::snprintf(info->dhcp_hostname, sizeof(info->dhcp_hostname), "PS4-Hunter");
            break;
        case SCE_NET_CTL_INFO_IP_ADDRESS:
            if (state_ == SCE_NET_CTL_STATE_CONNECTED) {
                std::snprintf(info->ip_address, sizeof(info->ip_address), "127.0.0.1");
            } else {
                std::snprintf(info->ip_address, sizeof(info->ip_address), "0.0.0.0");
            }
            break;
        case SCE_NET_CTL_INFO_NETMASK:
            std::snprintf(info->netmask, sizeof(info->netmask), "255.255.255.0");
            break;
        case SCE_NET_CTL_INFO_DEFAULT_ROUTE:
            std::snprintf(info->default_route, sizeof(info->default_route), "0.0.0.0");
            break;
        case SCE_NET_CTL_INFO_PRIMARY_DNS:
            std::snprintf(info->primary_dns, sizeof(info->primary_dns), "0.0.0.0");
            break;
        case SCE_NET_CTL_INFO_SECONDARY_DNS:
            std::snprintf(info->secondary_dns, sizeof(info->secondary_dns), "0.0.0.0");
            break;
        case SCE_NET_CTL_INFO_NAT_TYPE:
            info->nat_type = 0;
            break;
        default:
            return SCE_NET_CTL_ERROR_INVALID_CODE;
    }
    return SCE_NET_OK;
}

int32_t NetHleManager::sceNetCtlGetInfo(GuestMemory& mem, int32_t code, uint64_t info_addr) {
    if (info_addr == 0) {
        return SCE_NET_CTL_ERROR_INVALID_POINTER;
    }
    SceNetCtlInfo local_info{};
    int32_t res = sceNetCtlGetInfo(code, &local_info);
    if (res != SCE_NET_OK) {
        return res;
    }
    if (mem.store(info_addr, &local_info, sizeof(local_info)) != MemoryError::ok) {
        return SCE_NET_CTL_ERROR_INVALID_POINTER;
    }
    return SCE_NET_OK;
}

int32_t NetHleManager::sceNetCtlCheckCallback() {
    return SCE_NET_OK;
}

int32_t NetHleManager::sceNetSocket(int32_t domain, int32_t type, int32_t protocol) {
    (void)domain;
    (void)type;
    (void)protocol;
    return SCE_NET_ERROR_ENETUNREACH;
}

int32_t NetHleManager::sceNetSocketClose(int32_t s) {
    (void)s;
    return SCE_NET_OK;
}

int32_t NetHleManager::sceNetConnect(int32_t s, const void* name, uint32_t namelen) {
    (void)s;
    if (name == nullptr || namelen == 0) {
        return SCE_NET_ERROR_EFAULT;
    }
    return SCE_NET_ERROR_ENETUNREACH;
}

int32_t NetHleManager::sceNetConnect(GuestMemory& mem, int32_t s, uint64_t name_addr, uint32_t namelen) {
    (void)s;
    if (name_addr == 0 || namelen == 0) {
        return SCE_NET_ERROR_EFAULT;
    }
    uint8_t dummy = 0;
    if (mem.load(name_addr, &dummy, 1) != MemoryError::ok) {
        return SCE_NET_ERROR_EFAULT;
    }
    return SCE_NET_ERROR_ENETUNREACH;
}

int32_t NetHleManager::sceNetSend(int32_t s, const void* msg, std::size_t len, int32_t flags) {
    (void)s;
    (void)flags;
    if (len > 0 && msg == nullptr) {
        return SCE_NET_ERROR_EFAULT;
    }
    return SCE_NET_ERROR_ENETUNREACH;
}

int32_t NetHleManager::sceNetSend(GuestMemory& mem, int32_t s, uint64_t msg_addr, std::size_t len, int32_t flags) {
    (void)s;
    (void)flags;
    if (len > 0) {
        if (msg_addr == 0) {
            return SCE_NET_ERROR_EFAULT;
        }
        uint8_t dummy = 0;
        if (mem.load(msg_addr, &dummy, 1) != MemoryError::ok) {
            return SCE_NET_ERROR_EFAULT;
        }
    }
    return SCE_NET_ERROR_ENETUNREACH;
}

int32_t NetHleManager::sceNetRecv(int32_t s, void* buf, std::size_t len, int32_t flags) {
    (void)s;
    (void)flags;
    if (len > 0 && buf == nullptr) {
        return SCE_NET_ERROR_EFAULT;
    }
    return SCE_NET_ERROR_ENETUNREACH;
}

int32_t NetHleManager::sceNetRecv(GuestMemory& mem, int32_t s, uint64_t buf_addr, std::size_t len, int32_t flags) {
    (void)s;
    (void)flags;
    if (len > 0) {
        if (buf_addr == 0) {
            return SCE_NET_ERROR_EFAULT;
        }
        uint8_t dummy = 0;
        if (mem.load(buf_addr, &dummy, 1) != MemoryError::ok) {
            return SCE_NET_ERROR_EFAULT;
        }
    }
    return SCE_NET_ERROR_ENETUNREACH;
}

void NetHleManager::set_state(int32_t state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
}

int32_t NetHleManager::get_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void NetHleManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    net_initialized_ = false;
    netctl_initialized_ = false;
    state_ = SCE_NET_CTL_STATE_DISCONNECTED;
}

// ============================================================================
// NpHleManager
// ============================================================================

NpHleManager::NpHleManager() = default;

int32_t NpHleManager::sceNpGetState(int32_t* state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state != nullptr) {
        *state = state_;
    }
    return return_code_;
}

int32_t NpHleManager::sceNpGetState(GuestMemory& mem, uint64_t state_addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_addr != 0) {
        if (mem.store(state_addr, &state_, sizeof(state_)) != MemoryError::ok) {
            return SCE_NP_ERROR_INVALID_ARGUMENT;
        }
    }
    return return_code_;
}

int32_t NpHleManager::sceNpGetNpId(SceNpId* np_id) {
    if (np_id == nullptr) {
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    *np_id = SceNpId{};
    std::size_t len = std::min(offline_user_id_.size(), sizeof(np_id->handle) - 1);
    std::memcpy(np_id->handle, offline_user_id_.c_str(), len);
    np_id->handle[len] = '\0';
    return SCE_NP_OK;
}

int32_t NpHleManager::sceNpGetNpId(GuestMemory& mem, uint64_t np_id_addr) {
    if (np_id_addr == 0) {
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    }
    SceNpId id{};
    int32_t res = sceNpGetNpId(&id);
    if (res != SCE_NP_OK) {
        return res;
    }
    if (mem.store(np_id_addr, &id, sizeof(id)) != MemoryError::ok) {
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    }
    return SCE_NP_OK;
}

int32_t NpHleManager::sceNpGetOnlineId(SceNpOnlineId* online_id) {
    if (online_id == nullptr) {
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    *online_id = SceNpOnlineId{};
    std::size_t len = std::min(offline_user_id_.size(), sizeof(online_id->data) - 1);
    std::memcpy(online_id->data, offline_user_id_.c_str(), len);
    online_id->data[len] = '\0';
    online_id->term = '\0';
    return SCE_NP_OK;
}

int32_t NpHleManager::sceNpGetOnlineId(GuestMemory& mem, uint64_t online_id_addr) {
    if (online_id_addr == 0) {
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    }
    SceNpOnlineId id{};
    int32_t res = sceNpGetOnlineId(&id);
    if (res != SCE_NP_OK) {
        return res;
    }
    if (mem.store(online_id_addr, &id, sizeof(id)) != MemoryError::ok) {
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    }
    return SCE_NP_OK;
}

int32_t NpHleManager::sceNpCheckCallback() {
    return SCE_NP_OK;
}

void NpHleManager::set_state(int32_t state, int32_t return_code) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    return_code_ = return_code;
}

void NpHleManager::set_offline_user_id(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    offline_user_id_ = name;
}

std::string NpHleManager::get_offline_user_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return offline_user_id_;
}

void NpHleManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = SCE_NP_STATE_SIGNED_OUT;
    return_code_ = SCE_NP_ERROR_NOT_SIGNED_UP;
    offline_user_id_ = "Hunter";
}

// ============================================================================
// Singletons padrão e wrappers de nível C
// ============================================================================

static RtcManager g_default_rtc;
static NetHleManager g_default_net;
static NpHleManager g_default_np;

RtcManager& default_rtc() {
    return g_default_rtc;
}

NetHleManager& default_net() {
    return g_default_net;
}

NpHleManager& default_np() {
    return g_default_np;
}

int32_t sceRtcGetCurrentTick(SceRtcTick* tick) {
    return default_rtc().sceRtcGetCurrentTick(tick);
}

int32_t sceRtcGetCurrentTick(GuestMemory& mem, uint64_t tick_addr) {
    return default_rtc().sceRtcGetCurrentTick(mem, tick_addr);
}

int32_t sceRtcGetCurrentClock(SceRtcDateTime* clock, int32_t time_zone) {
    return default_rtc().sceRtcGetCurrentClock(clock, time_zone);
}

int32_t sceRtcGetCurrentClock(GuestMemory& mem, uint64_t clock_addr, int32_t time_zone) {
    return default_rtc().sceRtcGetCurrentClock(mem, clock_addr, time_zone);
}

int32_t sceRtcGetCurrentClockLocalTime(SceRtcDateTime* clock) {
    return default_rtc().sceRtcGetCurrentClockLocalTime(clock);
}

int32_t sceRtcGetCurrentClockLocalTime(GuestMemory& mem, uint64_t clock_addr) {
    return default_rtc().sceRtcGetCurrentClockLocalTime(mem, clock_addr);
}

int32_t sceRtcGetCurrentNetworkTick(SceRtcTick* tick) {
    return default_rtc().sceRtcGetCurrentNetworkTick(tick);
}

int32_t sceRtcGetCurrentNetworkTick(GuestMemory& mem, uint64_t tick_addr) {
    return default_rtc().sceRtcGetCurrentNetworkTick(mem, tick_addr);
}

int32_t sceRtcConvertUtcToLocalTime(const SceRtcTick* utc_tick, SceRtcTick* local_tick) {
    return default_rtc().sceRtcConvertUtcToLocalTime(utc_tick, local_tick);
}

int32_t sceRtcConvertUtcToLocalTime(GuestMemory& mem, uint64_t utc_tick_addr, uint64_t local_tick_addr) {
    return default_rtc().sceRtcConvertUtcToLocalTime(mem, utc_tick_addr, local_tick_addr);
}

int32_t sceRtcConvertLocalTimeToUtc(const SceRtcTick* local_tick, SceRtcTick* utc_tick) {
    return default_rtc().sceRtcConvertLocalTimeToUtc(local_tick, utc_tick);
}

int32_t sceRtcConvertLocalTimeToUtc(GuestMemory& mem, uint64_t local_tick_addr, uint64_t utc_tick_addr) {
    return default_rtc().sceRtcConvertLocalTimeToUtc(mem, local_tick_addr, utc_tick_addr);
}

int32_t sceNetInit() {
    return default_net().sceNetInit();
}

int32_t sceNetTerm() {
    return default_net().sceNetTerm();
}

int32_t sceNetCtlInit() {
    return default_net().sceNetCtlInit();
}

int32_t sceNetCtlTerm() {
    return default_net().sceNetCtlTerm();
}

int32_t sceNetCtlGetState(int32_t* state) {
    return default_net().sceNetCtlGetState(state);
}

int32_t sceNetCtlGetState(GuestMemory& mem, uint64_t state_addr) {
    return default_net().sceNetCtlGetState(mem, state_addr);
}

int32_t sceNetCtlGetInfo(int32_t code, SceNetCtlInfo* info) {
    return default_net().sceNetCtlGetInfo(code, info);
}

int32_t sceNetCtlGetInfo(GuestMemory& mem, int32_t code, uint64_t info_addr) {
    return default_net().sceNetCtlGetInfo(mem, code, info_addr);
}

int32_t sceNetCtlCheckCallback() {
    return default_net().sceNetCtlCheckCallback();
}

int32_t sceNetSocket(int32_t domain, int32_t type, int32_t protocol) {
    return default_net().sceNetSocket(domain, type, protocol);
}

int32_t sceNetSocketClose(int32_t s) {
    return default_net().sceNetSocketClose(s);
}

int32_t sceNetConnect(int32_t s, const void* name, uint32_t namelen) {
    return default_net().sceNetConnect(s, name, namelen);
}

int32_t sceNetConnect(GuestMemory& mem, int32_t s, uint64_t name_addr, uint32_t namelen) {
    return default_net().sceNetConnect(mem, s, name_addr, namelen);
}

int32_t sceNetSend(int32_t s, const void* msg, std::size_t len, int32_t flags) {
    return default_net().sceNetSend(s, msg, len, flags);
}

int32_t sceNetSend(GuestMemory& mem, int32_t s, uint64_t msg_addr, std::size_t len, int32_t flags) {
    return default_net().sceNetSend(mem, s, msg_addr, len, flags);
}

int32_t sceNetRecv(int32_t s, void* buf, std::size_t len, int32_t flags) {
    return default_net().sceNetRecv(s, buf, len, flags);
}

int32_t sceNetRecv(GuestMemory& mem, int32_t s, uint64_t buf_addr, std::size_t len, int32_t flags) {
    return default_net().sceNetRecv(mem, s, buf_addr, len, flags);
}

int32_t sceNpGetState(int32_t* state) {
    return default_np().sceNpGetState(state);
}

int32_t sceNpGetState(GuestMemory& mem, uint64_t state_addr) {
    return default_np().sceNpGetState(mem, state_addr);
}

int32_t sceNpGetNpId(SceNpId* np_id) {
    return default_np().sceNpGetNpId(np_id);
}

int32_t sceNpGetNpId(GuestMemory& mem, uint64_t np_id_addr) {
    return default_np().sceNpGetNpId(mem, np_id_addr);
}

int32_t sceNpGetOnlineId(SceNpOnlineId* online_id) {
    return default_np().sceNpGetOnlineId(online_id);
}

int32_t sceNpGetOnlineId(GuestMemory& mem, uint64_t online_id_addr) {
    return default_np().sceNpGetOnlineId(mem, online_id_addr);
}

int32_t sceNpCheckCallback() {
    return default_np().sceNpCheckCallback();
}

} // namespace bb
