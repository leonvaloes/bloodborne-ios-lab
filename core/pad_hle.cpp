#include "pad_hle.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace bb {

PadHleManager::PadHleManager(InputManager* default_input, UserServiceManager* user_service)
    : default_input_(default_input), user_service_(user_service) {
    if (!default_input_) {
        fallback_input_ = std::make_unique<InputManager>();
    }
    reset();
}

void PadHleManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < SCE_PAD_MAX_PORTS; ++i) {
        ports_[i].handle = -1;
        ports_[i].user_id = 0;
        ports_[i].type = 0;
        ports_[i].index = static_cast<std::int32_t>(i);
        ports_[i].is_open = false;
        ports_[i].input_source = nullptr;
        ports_[i].owned_input.reset();
        ports_[i].vibration = ScePadVibrationParam{};
        ports_[i].lightbar = ScePadColor{0, 0, 255, 0};
        ports_[i].deadzone = 0.15f;
        ports_[i].last_timestamp = 0;
    }
    next_handle_ = 1;
    timestamp_counter_ = 1000;
}

std::uint64_t PadHleManager::generate_timestamp_locked() {
    auto now_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now_us <= timestamp_counter_) {
        timestamp_counter_ += 1000;
    } else {
        timestamp_counter_ = now_us;
    }
    return timestamp_counter_;
}

PadPortInstance* PadHleManager::find_port_by_handle_locked(std::int32_t handle) {
    if (handle <= 0) {
        return nullptr;
    }
    for (std::size_t i = 0; i < SCE_PAD_MAX_PORTS; ++i) {
        if (ports_[i].is_open && ports_[i].handle == handle) {
            return &ports_[i];
        }
    }
    return nullptr;
}

const PadPortInstance* PadHleManager::find_port_by_handle_locked(std::int32_t handle) const {
    if (handle <= 0) {
        return nullptr;
    }
    for (std::size_t i = 0; i < SCE_PAD_MAX_PORTS; ++i) {
        if (ports_[i].is_open && ports_[i].handle == handle) {
            return &ports_[i];
        }
    }
    return nullptr;
}

std::int32_t PadHleManager::scePadOpen(std::uint32_t userId, std::int32_t type, std::int32_t index, const void* /*param*/) {
    if (userId == 0 || userId == static_cast<std::uint32_t>(SCE_USER_SERVICE_USER_ID_INVALID)) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }
    if (type != SCE_PAD_PORT_TYPE_STANDARD) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= SCE_PAD_MAX_PORTS) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (user_service_ && !user_service_->is_user_logged_in(static_cast<SceUserServiceUserId>(userId))) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }

    if (ports_[static_cast<std::size_t>(index)].is_open) {
        return SCE_PAD_ERROR_ALREADY_OPENED;
    }

    for (std::size_t i = 0; i < SCE_PAD_MAX_PORTS; ++i) {
        if (ports_[i].is_open && ports_[i].user_id == userId) {
            return SCE_PAD_ERROR_ALREADY_OPENED;
        }
    }

    std::int32_t handle = next_handle_++;
    PadPortInstance& port = ports_[static_cast<std::size_t>(index)];
    port.handle = handle;
    port.user_id = userId;
    port.type = type;
    port.index = index;
    port.is_open = true;
    port.vibration = ScePadVibrationParam{};
    port.lightbar = ScePadColor{0, 0, 255, 0};
    port.deadzone = 0.15f;
    port.last_timestamp = generate_timestamp_locked();
    port.input_source = default_input_ ? default_input_ : fallback_input_.get();

    return handle;
}

std::int32_t PadHleManager::scePadOpen(GuestMemory& mem, std::uint32_t userId, std::int32_t type, std::int32_t index, std::uint64_t param_addr) {
    if (param_addr != 0) {
        ScePadOpenParam param{};
        if (mem.load(param_addr, &param, sizeof(param)) != MemoryError::ok) {
            return SCE_PAD_ERROR_INVALID_ARG;
        }
    }
    return scePadOpen(userId, type, index, nullptr);
}

std::int32_t PadHleManager::scePadClose(std::int32_t handle) {
    if (handle <= 0) {
        return SCE_PAD_ERROR_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    PadPortInstance* port = find_port_by_handle_locked(handle);
    if (!port || !port->is_open) {
        return SCE_PAD_ERROR_INVALID_HANDLE;
    }
    port->is_open = false;
    port->handle = -1;
    port->user_id = 0;
    port->input_source = nullptr;
    port->owned_input.reset();
    return 0; // SCE_OK
}

ScePadData PadHleManager::format_pad_data(const PadData& pad, std::uint64_t timestamp) {
    ScePadData out{};
    out.buttons = pad.buttons;

    // Sticks analógicos calibrados de [-1.0f, +1.0f] para [0..255], centro neutro exato em 128
    out.left_stick_x = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round((pad.left_stick_x + 1.0f) * 127.5f)), 0, 255));
    out.left_stick_y = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round((pad.left_stick_y + 1.0f) * 127.5f)), 0, 255));
    out.right_stick_x = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round((pad.right_stick_x + 1.0f) * 127.5f)), 0, 255));
    out.right_stick_y = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round((pad.right_stick_y + 1.0f) * 127.5f)), 0, 255));

    // Triggers analógicos calibrados de [0.0f, +1.0f] para [0..255]
    out.l2_analog = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round(pad.l2_analog * 255.0f)), 0, 255));
    out.r2_analog = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round(pad.r2_analog * 255.0f)), 0, 255));

    out.padding[0] = 0;
    out.padding[1] = 0;
    out.connected = pad.connected ? 1 : 0;
    out.timestamp = timestamp;
    return out;
}

std::int32_t PadHleManager::scePadReadState(std::int32_t handle, ScePadData* data) {
    if (!data) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }
    if (handle <= 0) {
        return SCE_PAD_ERROR_INVALID_HANDLE;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    PadPortInstance* port = find_port_by_handle_locked(handle);
    if (!port || !port->is_open) {
        return SCE_PAD_ERROR_INVALID_HANDLE;
    }

    InputManager* input = port->input_source ? port->input_source : (default_input_ ? default_input_ : fallback_input_.get());
    PadData raw_pad = input->poll();
    port->last_timestamp = generate_timestamp_locked();
    *data = format_pad_data(raw_pad, port->last_timestamp);

    return 0; // SCE_OK
}

std::int32_t PadHleManager::scePadReadState(GuestMemory& mem, std::int32_t handle, std::uint64_t guest_addr) {
    if (guest_addr == 0) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }
    ScePadData state{};
    std::int32_t ret = scePadReadState(handle, &state);
    if (ret != 0) {
        return ret;
    }
    if (mem.store(guest_addr, &state, sizeof(state)) != MemoryError::ok) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }
    return 0; // SCE_OK
}

std::int32_t PadHleManager::scePadSetVibration(std::int32_t handle, const ScePadVibrationParam* param) {
    if (!param) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }
    if (handle <= 0) {
        return SCE_PAD_ERROR_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    PadPortInstance* port = find_port_by_handle_locked(handle);
    if (!port || !port->is_open) {
        return SCE_PAD_ERROR_INVALID_HANDLE;
    }
    port->vibration = *param;
    return 0; // SCE_OK
}

std::int32_t PadHleManager::scePadSetVibration(GuestMemory& mem, std::int32_t handle, std::uint64_t guest_param_addr) {
    if (guest_param_addr == 0) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }
    ScePadVibrationParam param{};
    if (mem.load(guest_param_addr, &param, sizeof(param)) != MemoryError::ok) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }
    return scePadSetVibration(handle, &param);
}

std::int32_t PadHleManager::scePadSetLightBar(std::int32_t handle, const ScePadColor* color) {
    if (!color) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }
    if (handle <= 0) {
        return SCE_PAD_ERROR_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    PadPortInstance* port = find_port_by_handle_locked(handle);
    if (!port || !port->is_open) {
        return SCE_PAD_ERROR_INVALID_HANDLE;
    }
    port->lightbar = *color;
    return 0; // SCE_OK
}

std::int32_t PadHleManager::scePadSetLightBar(GuestMemory& mem, std::int32_t handle, std::uint64_t guest_color_addr) {
    if (guest_color_addr == 0) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }
    ScePadColor color{};
    if (mem.load(guest_color_addr, &color, sizeof(color)) != MemoryError::ok) {
        return SCE_PAD_ERROR_INVALID_ARG;
    }
    return scePadSetLightBar(handle, &color);
}

void PadHleManager::set_user_service(UserServiceManager* user_service) {
    std::lock_guard<std::mutex> lock(mutex_);
    user_service_ = user_service;
}

void PadHleManager::set_default_input(InputManager* input) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_input_ = input;
}

void PadHleManager::bind_port_input(std::int32_t handle, InputManager* input) {
    std::lock_guard<std::mutex> lock(mutex_);
    PadPortInstance* port = find_port_by_handle_locked(handle);
    if (port && port->is_open) {
        port->input_source = input;
    }
}

InputManager* PadHleManager::get_input_manager(std::int32_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const PadPortInstance* port = find_port_by_handle_locked(handle);
    if (port && port->is_open) {
        return port->input_source ? port->input_source : (default_input_ ? default_input_ : fallback_input_.get());
    }
    return nullptr;
}

bool PadHleManager::is_handle_valid(std::int32_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return find_port_by_handle_locked(handle) != nullptr;
}

std::size_t PadHleManager::active_pad_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (std::size_t i = 0; i < SCE_PAD_MAX_PORTS; ++i) {
        if (ports_[i].is_open) {
            ++count;
        }
    }
    return count;
}

ScePadVibrationParam PadHleManager::get_vibration(std::int32_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const PadPortInstance* port = find_port_by_handle_locked(handle);
    if (port && port->is_open) {
        return port->vibration;
    }
    return ScePadVibrationParam{};
}

ScePadColor PadHleManager::get_lightbar(std::int32_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const PadPortInstance* port = find_port_by_handle_locked(handle);
    if (port && port->is_open) {
        return port->lightbar;
    }
    return ScePadColor{0, 0, 0, 0};
}

void PadHleManager::set_deadzone(std::int32_t handle, float deadzone) {
    std::lock_guard<std::mutex> lock(mutex_);
    PadPortInstance* port = find_port_by_handle_locked(handle);
    if (port && port->is_open) {
        port->deadzone = deadzone;
        if (port->input_source) {
            port->input_source->set_deadzone(deadzone);
        }
    }
}

PadHleManager& default_pad_hle() {
    static PadHleManager s_instance(nullptr, &default_user_service());
    return s_instance;
}

std::int32_t scePadOpen(std::uint32_t userId, std::int32_t type, std::int32_t index, const void* param) {
    return default_pad_hle().scePadOpen(userId, type, index, param);
}

std::int32_t scePadClose(std::int32_t handle) {
    return default_pad_hle().scePadClose(handle);
}

std::int32_t scePadReadState(std::int32_t handle, ScePadData* data) {
    return default_pad_hle().scePadReadState(handle, data);
}

std::int32_t scePadReadState(GuestMemory& mem, std::int32_t handle, std::uint64_t guest_addr) {
    return default_pad_hle().scePadReadState(mem, handle, guest_addr);
}

std::int32_t scePadSetVibration(std::int32_t handle, const ScePadVibrationParam* param) {
    return default_pad_hle().scePadSetVibration(handle, param);
}

std::int32_t scePadSetVibration(GuestMemory& mem, std::int32_t handle, std::uint64_t guest_param_addr) {
    return default_pad_hle().scePadSetVibration(mem, handle, guest_param_addr);
}

std::int32_t scePadSetLightBar(std::int32_t handle, const ScePadColor* color) {
    return default_pad_hle().scePadSetLightBar(handle, color);
}

std::int32_t scePadSetLightBar(GuestMemory& mem, std::int32_t handle, std::uint64_t guest_color_addr) {
    return default_pad_hle().scePadSetLightBar(mem, handle, guest_color_addr);
}

} // namespace bb
