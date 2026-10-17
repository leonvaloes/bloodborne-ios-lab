#include "input.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace bb {

InputManager::InputManager() {
    reset();
}

void InputManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = true;
    registered_controllers_.clear();
    active_controller_id_ = 0;
    for (auto &src : sources_) {
        src = PadData{};
        src.connected = true;
    }
}

void InputManager::clear_source(InputSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto idx = static_cast<std::size_t>(source);
    if (idx < sources_.size()) {
        bool was_conn = sources_[idx].connected;
        sources_[idx] = PadData{};
        sources_[idx].connected = was_conn;
    }
}

void InputManager::set_button(std::uint32_t button, bool pressed, InputSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto idx = static_cast<std::size_t>(source);
    if (idx >= sources_.size()) return;
    if (pressed) {
        sources_[idx].buttons |= button;
    } else {
        sources_[idx].buttons &= ~button;
    }
}

void InputManager::set_buttons_mask(std::uint32_t mask, InputSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto idx = static_cast<std::size_t>(source);
    if (idx >= sources_.size()) return;
    sources_[idx].buttons = mask;
}

void InputManager::set_left_stick(float x, float y, InputSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto idx = static_cast<std::size_t>(source);
    if (idx >= sources_.size()) return;
    sources_[idx].left_stick_x = std::clamp(x, -1.0f, 1.0f);
    sources_[idx].left_stick_y = std::clamp(y, -1.0f, 1.0f);
}

void InputManager::set_right_stick(float x, float y, InputSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto idx = static_cast<std::size_t>(source);
    if (idx >= sources_.size()) return;
    sources_[idx].right_stick_x = std::clamp(x, -1.0f, 1.0f);
    sources_[idx].right_stick_y = std::clamp(y, -1.0f, 1.0f);
}

void InputManager::set_triggers(float l2, float r2, InputSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto idx = static_cast<std::size_t>(source);
    if (idx >= sources_.size()) return;
    sources_[idx].l2_analog = std::clamp(l2, 0.0f, 1.0f);
    sources_[idx].r2_analog = std::clamp(r2, 0.0f, 1.0f);
    if (sources_[idx].l2_analog > 0.2f) sources_[idx].buttons |= PAD_L2;
    else sources_[idx].buttons &= ~PAD_L2;
    if (sources_[idx].r2_analog > 0.2f) sources_[idx].buttons |= PAD_R2;
    else sources_[idx].buttons &= ~PAD_R2;
}

void InputManager::set_connected(bool connected) {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = connected;
    if (!connected) {
        for (auto &src : sources_) {
            src.buttons = 0;
            src.left_stick_x = 0.0f;
            src.left_stick_y = 0.0f;
            src.right_stick_x = 0.0f;
            src.right_stick_y = 0.0f;
            src.l2_analog = 0.0f;
            src.r2_analog = 0.0f;
        }
    }
}

void InputManager::set_connected(bool connected, InputSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto idx = static_cast<std::size_t>(source);
    if (idx < sources_.size()) {
        sources_[idx].connected = connected;
        if (!connected) {
            // Desconexão limpa imediatamente os botões para evitar entradas retidas na troca
            sources_[idx].buttons = 0;
            sources_[idx].left_stick_x = 0.0f;
            sources_[idx].left_stick_y = 0.0f;
            sources_[idx].right_stick_x = 0.0f;
            sources_[idx].right_stick_y = 0.0f;
            sources_[idx].l2_analog = 0.0f;
            sources_[idx].r2_analog = 0.0f;
        }
    }
}

void InputManager::set_deadzone(float deadzone) {
    std::lock_guard<std::mutex> lock(mutex_);
    deadzone_ = std::clamp(deadzone, 0.0f, 0.9f);
}

void InputManager::set_pad_state(const PadData &state, InputSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto idx = static_cast<std::size_t>(source);
    if (idx < sources_.size()) {
        sources_[idx] = state;
        if (!state.connected) {
            sources_[idx].buttons = 0;
            sources_[idx].left_stick_x = 0.0f;
            sources_[idx].left_stick_y = 0.0f;
            sources_[idx].right_stick_x = 0.0f;
            sources_[idx].right_stick_y = 0.0f;
            sources_[idx].l2_analog = 0.0f;
            sources_[idx].r2_analog = 0.0f;
        }
    }
}

PadData InputManager::poll_source(InputSource source) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto idx = static_cast<std::size_t>(source);
    if (idx < sources_.size()) {
        PadData out = sources_[idx];
        out.connected = connected_ && sources_[idx].connected;
        apply_radial_deadzone(sources_[idx].left_stick_x, sources_[idx].left_stick_y, deadzone_, out.left_stick_x, out.left_stick_y);
        apply_radial_deadzone(sources_[idx].right_stick_x, sources_[idx].right_stick_y, deadzone_, out.right_stick_x, out.right_stick_y);
        return out;
    }
    return PadData{};
}

float InputManager::apply_deadzone(float val, float threshold) {
    const float abs_val = std::abs(val);
    if (abs_val <= threshold) return 0.0f;
    const float scaled = (abs_val - threshold) / (1.0f - threshold);
    return std::clamp((val < 0.0f ? -scaled : scaled), -1.0f, 1.0f);
}

void InputManager::apply_radial_deadzone(float in_x, float in_y, float threshold, float &out_x, float &out_y) {
    const float mag = std::sqrt(in_x * in_x + in_y * in_y);
    if (mag <= threshold || mag <= 0.0001f) {
        out_x = 0.0f;
        out_y = 0.0f;
        return;
    }
    const float normalized_mag = std::min(1.0f, (mag - threshold) / (1.0f - threshold));
    out_x = (in_x / mag) * normalized_mag;
    out_y = (in_y / mag) * normalized_mag;
}

PadData InputManager::poll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto &touch = sources_[static_cast<std::size_t>(InputSource::Touch)];
    const auto &gamepad = sources_[static_cast<std::size_t>(InputSource::Gamepad)];

    const bool touch_active = connected_ && touch.connected;
    const bool gp_active = connected_ && gamepad.connected;

    PadData out{};
    out.connected = touch_active || gp_active;

    // Ignora botões, eixos e gatilhos de fontes desconectadas
    const std::uint32_t touch_btn = touch_active ? touch.buttons : 0;
    const std::uint32_t gp_btn = gp_active ? gamepad.buttons : 0;
    out.buttons = touch_btn | gp_btn;

    const float touch_lx = touch_active ? touch.left_stick_x : 0.0f;
    const float touch_ly = touch_active ? touch.left_stick_y : 0.0f;
    const float gp_lx = gp_active ? gamepad.left_stick_x : 0.0f;
    const float gp_ly = gp_active ? gamepad.left_stick_y : 0.0f;

    // Arbitração do analógico esquerdo por magnitude dominante
    float touch_l_mag = std::sqrt(touch_lx * touch_lx + touch_ly * touch_ly);
    float gp_l_mag = std::sqrt(gp_lx * gp_lx + gp_ly * gp_ly);
    if (gp_l_mag >= touch_l_mag && gp_l_mag > 0.0001f) {
        out.left_stick_x = gp_lx;
        out.left_stick_y = gp_ly;
    } else if (touch_l_mag > 0.0001f) {
        out.left_stick_x = touch_lx;
        out.left_stick_y = touch_ly;
    } else {
        out.left_stick_x = 0.0f;
        out.left_stick_y = 0.0f;
    }

    const float touch_rx = touch_active ? touch.right_stick_x : 0.0f;
    const float touch_ry = touch_active ? touch.right_stick_y : 0.0f;
    const float gp_rx = gp_active ? gamepad.right_stick_x : 0.0f;
    const float gp_ry = gp_active ? gamepad.right_stick_y : 0.0f;

    // Arbitração do analógico direito por magnitude dominante
    float touch_r_mag = std::sqrt(touch_rx * touch_rx + touch_ry * touch_ry);
    float gp_r_mag = std::sqrt(gp_rx * gp_rx + gp_ry * gp_ry);
    if (gp_r_mag >= touch_r_mag && gp_r_mag > 0.0001f) {
        out.right_stick_x = gp_rx;
        out.right_stick_y = gp_ry;
    } else if (touch_r_mag > 0.0001f) {
        out.right_stick_x = touch_rx;
        out.right_stick_y = touch_ry;
    } else {
        out.right_stick_x = 0.0f;
        out.right_stick_y = 0.0f;
    }

    // Gatilhos analógicos L2 / R2: pressão máxima entre as fontes ativas
    const float touch_l2 = touch_active ? touch.l2_analog : 0.0f;
    const float gp_l2 = gp_active ? gamepad.l2_analog : 0.0f;
    const float touch_r2 = touch_active ? touch.r2_analog : 0.0f;
    const float gp_r2 = gp_active ? gamepad.r2_analog : 0.0f;

    out.l2_analog = std::max(touch_l2, gp_l2);
    out.r2_analog = std::max(touch_r2, gp_r2);
    if (out.l2_analog > 0.2f) out.buttons |= PAD_L2;
    if (out.r2_analog > 0.2f) out.buttons |= PAD_R2;

    apply_radial_deadzone(out.left_stick_x, out.left_stick_y, deadzone_, out.left_stick_x, out.left_stick_y);
    apply_radial_deadzone(out.right_stick_x, out.right_stick_y, deadzone_, out.right_stick_x, out.right_stick_y);
    return out;
}

MemoryError InputManager::write_hle_pad_data(GuestMemory &mem, std::uint64_t addr) const {
    const PadData data = poll();
    std::uint8_t raw[16]{};

    // Buttons bitmask (offset 0..3)
    std::memcpy(raw, &data.buttons, sizeof(data.buttons));

    // Left stick X/Y (offset 4, 5): map [-1.0, 1.0] to [0..255]
    raw[4] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round((data.left_stick_x + 1.0f) * 127.5f)), 0, 255));
    raw[5] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round((data.left_stick_y + 1.0f) * 127.5f)), 0, 255));

    // Right stick X/Y (offset 6, 7): map [-1.0, 1.0] to [0..255]
    raw[6] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round((data.right_stick_x + 1.0f) * 127.5f)), 0, 255));
    raw[7] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round((data.right_stick_y + 1.0f) * 127.5f)), 0, 255));

    // Triggers L2/R2 analog (offset 8, 9): map [0.0, 1.0] to [0..255]
    raw[8] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round(data.l2_analog * 255.0f)), 0, 255));
    raw[9] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round(data.r2_analog * 255.0f)), 0, 255));

    // Connected state (offset 12..15)
    std::uint32_t conn = data.connected ? 1 : 0;
    std::memcpy(raw + 12, &conn, sizeof(conn));

    return mem.store(addr, raw, sizeof(raw));
}

bool InputManager::register_controller(std::uint64_t controller_id) {
    if (controller_id == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto id : registered_controllers_) {
        if (id == controller_id) {
            return (active_controller_id_ == controller_id);
        }
    }
    registered_controllers_.push_back(controller_id);
    if (active_controller_id_ == 0) {
        active_controller_id_ = controller_id;
        sources_[static_cast<std::size_t>(InputSource::Gamepad)].connected = true;
        return true;
    }
    return false;
}

bool InputManager::unregister_controller(std::uint64_t controller_id) {
    if (controller_id == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find(registered_controllers_.begin(), registered_controllers_.end(), controller_id);
    if (it == registered_controllers_.end()) return false;
    registered_controllers_.erase(it);

    if (active_controller_id_ != controller_id) {
        return false; // Controle secundário desconectado; ativo inalterado
    }

    if (!registered_controllers_.empty()) {
        active_controller_id_ = registered_controllers_.front();
        sources_[static_cast<std::size_t>(InputSource::Gamepad)] = PadData{};
        sources_[static_cast<std::size_t>(InputSource::Gamepad)].connected = true;
        return true; // Alternou para outro controle disponível e limpou entradas retidas
    } else {
        active_controller_id_ = 0;
        sources_[static_cast<std::size_t>(InputSource::Gamepad)] = PadData{};
        sources_[static_cast<std::size_t>(InputSource::Gamepad)].connected = false;
        return true; // Ativo desconectado e nenhum controle remanescente
    }
}

bool InputManager::set_active_controller(std::uint64_t controller_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (controller_id == 0) {
        active_controller_id_ = 0;
        sources_[static_cast<std::size_t>(InputSource::Gamepad)] = PadData{};
        sources_[static_cast<std::size_t>(InputSource::Gamepad)].connected = false;
        return true;
    }
    auto it = std::find(registered_controllers_.begin(), registered_controllers_.end(), controller_id);
    if (it == registered_controllers_.end()) return false;
    if (active_controller_id_ != controller_id) {
        active_controller_id_ = controller_id;
        sources_[static_cast<std::size_t>(InputSource::Gamepad)] = PadData{};
        sources_[static_cast<std::size_t>(InputSource::Gamepad)].connected = true;
    }
    return true;
}

std::uint64_t InputManager::active_controller_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_controller_id_;
}

std::size_t InputManager::connected_controllers_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registered_controllers_.size();
}

bool InputManager::is_controller_active(std::uint64_t controller_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_controller_id_ == controller_id;
}

} // namespace bb
