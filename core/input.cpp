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
    state_ = PadData{};
    state_.connected = true;
}

void InputManager::set_button(std::uint32_t button, bool pressed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pressed) {
        state_.buttons |= button;
    } else {
        state_.buttons &= ~button;
    }
}

void InputManager::set_buttons_mask(std::uint32_t mask) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.buttons = mask;
}

void InputManager::set_left_stick(float x, float y) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.left_stick_x = std::clamp(x, -1.0f, 1.0f);
    state_.left_stick_y = std::clamp(y, -1.0f, 1.0f);
}

void InputManager::set_right_stick(float x, float y) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.right_stick_x = std::clamp(x, -1.0f, 1.0f);
    state_.right_stick_y = std::clamp(y, -1.0f, 1.0f);
}

void InputManager::set_triggers(float l2, float r2) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.l2_analog = std::clamp(l2, 0.0f, 1.0f);
    state_.r2_analog = std::clamp(r2, 0.0f, 1.0f);
    if (state_.l2_analog > 0.2f) state_.buttons |= PAD_L2;
    else state_.buttons &= ~PAD_L2;
    if (state_.r2_analog > 0.2f) state_.buttons |= PAD_R2;
    else state_.buttons &= ~PAD_R2;
}

void InputManager::set_connected(bool connected) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.connected = connected;
}

void InputManager::set_deadzone(float deadzone) {
    std::lock_guard<std::mutex> lock(mutex_);
    deadzone_ = std::clamp(deadzone, 0.0f, 0.9f);
}

void InputManager::set_pad_state(const PadData &state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
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
    PadData out = state_;
    apply_radial_deadzone(state_.left_stick_x, state_.left_stick_y, deadzone_, out.left_stick_x, out.left_stick_y);
    apply_radial_deadzone(state_.right_stick_x, state_.right_stick_y, deadzone_, out.right_stick_x, out.right_stick_y);
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

} // namespace bb
