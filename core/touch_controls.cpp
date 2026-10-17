#include "touch_controls.h"
#include <algorithm>
#include <cmath>

namespace bb {

const char* HapticEvent::type_name() const {
    switch (type) {
        case HapticType::None:      return "none";
        case HapticType::Light:     return "light";
        case HapticType::Medium:    return "medium";
        case HapticType::Heavy:     return "heavy";
        case HapticType::Selection: return "selection";
        case HapticType::Impact:    return "impact";
        default:                    return "unknown";
    }
}

HapticEvent HapticEvent::translate_vibration(std::uint8_t large, std::uint8_t small) {
    HapticEvent ev{};
    ev.large_motor = large;
    ev.small_motor = small;

    if (large == 0 && small == 0) {
        ev.type = HapticType::None;
        ev.intensity = 0.0f;
        ev.sharpness = 0.0f;
        ev.duration_ms = 0.0f;
        return ev;
    }

    const float large_norm = static_cast<float>(large) / 255.0f;
    const float small_norm = static_cast<float>(small) / 255.0f;

    // Se ambos os motores estiverem acionados simultaneamente:
    // CoreHaptics / Impact composto com intensidade máxima e nitidez baseada no motor de alta frequência
    if (large > 0 && small > 0) {
        ev.type = HapticType::Impact;
        ev.intensity = std::max(large_norm, small_norm);
        ev.sharpness = small_norm;
        ev.duration_ms = 150.0f + 150.0f * ev.intensity;
        return ev;
    }

    // Apenas motor grande ativo (baixa frequência / impactos pesados):
    if (large > 0 && small == 0) {
        ev.intensity = large_norm;
        ev.sharpness = 0.1f + 0.2f * large_norm;
        ev.duration_ms = 100.0f + 200.0f * large_norm;

        if (large >= 170) {
            ev.type = HapticType::Heavy;
        } else if (large >= 70) {
            ev.type = HapticType::Medium;
        } else {
            ev.type = HapticType::Light;
        }
        return ev;
    }

    // Apenas motor pequeno ativo (alta frequência / sutilezas):
    if (small > 0 && large == 0) {
        ev.intensity = small_norm;
        ev.sharpness = 0.7f + 0.3f * small_norm;
        ev.duration_ms = 50.0f + 100.0f * small_norm;

        if (small < 50) {
            ev.type = HapticType::Selection;
        } else if (small < 170) {
            ev.type = HapticType::Light;
        } else {
            ev.type = HapticType::Medium;
        }
        return ev;
    }

    return ev;
}

HapticEvent HapticEvent::translate_vibration(const ScePadVibrationParam& param) {
    return translate_vibration(param.large_motor, param.small_motor);
}

TouchControlsOverlay::TouchControlsOverlay(InputManager* input, const ScreenLayoutConfig& config)
    : layout_(config), input_manager_(input) {
    if (input_manager_) {
        input_manager_->set_deadzone(0.0f);
    }

    left_stick_.deadzone = 0.15f;
    left_stick_.sensitivity = 1.0f;
    left_stick_.travel_radius = 50.0f;
    left_stick_.mode = StickMode::Static;

    right_stick_.deadzone = 0.15f;
    right_stick_.sensitivity = 1.0f;
    right_stick_.travel_radius = 50.0f;
    right_stick_.mode = StickMode::Static;

    state_.connected = true;

    recalculate_layout_locked();
    sync_input_manager_locked();
}

void TouchControlsOverlay::set_screen_layout(const ScreenLayoutConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!std::isfinite(config.width) || config.width <= 0.0f ||
        !std::isfinite(config.height) || config.height <= 0.0f ||
        !std::isfinite(config.safe_left) || config.safe_left < 0.0f ||
        !std::isfinite(config.safe_right) || config.safe_right < 0.0f ||
        !std::isfinite(config.safe_top) || config.safe_top < 0.0f ||
        !std::isfinite(config.safe_bottom) || config.safe_bottom < 0.0f ||
        (config.safe_left + config.safe_right >= config.width) ||
        (config.safe_top + config.safe_bottom >= config.height)) {
        std::fprintf(stderr, "[TouchControls] Invalid screen layout rejected: %.2fx%.2f safe=(%.2f, %.2f, %.2f, %.2f)\n",
                     config.width, config.height, config.safe_left, config.safe_right, config.safe_top, config.safe_bottom);
        return;
    }
    layout_ = config;
    recalculate_layout_locked();
}

ScreenLayoutConfig TouchControlsOverlay::get_screen_layout() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return layout_;
}

bool TouchControlsOverlay::is_point_valid(float x, float y) const {
    if (x < 0.0f || x > layout_.width || y < 0.0f || y > layout_.height) {
        return false;
    }
    if (layout_.dynamic_island_exclusion_enabled && layout_.dynamic_island_exclusion.contains(x, y)) {
        return false;
    }
    return true;
}

bool TouchControlsOverlay::is_in_exclusion_zone(float x, float y) const {
    return layout_.dynamic_island_exclusion_enabled && layout_.dynamic_island_exclusion.contains(x, y);
}

void TouchControlsOverlay::set_left_stick_mode(StickMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    left_stick_.mode = mode;
}

StickMode TouchControlsOverlay::get_left_stick_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return left_stick_.mode;
}

void TouchControlsOverlay::set_right_stick_mode(StickMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    right_stick_.mode = mode;
}

StickMode TouchControlsOverlay::get_right_stick_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return right_stick_.mode;
}

void TouchControlsOverlay::set_camera_sensitivity(float sens) {
    std::lock_guard<std::mutex> lock(mutex_);
    right_stick_.sensitivity = std::clamp(sens, 0.1f, 5.0f);
}

float TouchControlsOverlay::get_camera_sensitivity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return right_stick_.sensitivity;
}

void TouchControlsOverlay::set_stick_deadzone(float deadzone) {
    std::lock_guard<std::mutex> lock(mutex_);
    const float dz = std::clamp(deadzone, 0.0f, 0.9f);
    left_stick_.deadzone = dz;
    right_stick_.deadzone = dz;
}

float TouchControlsOverlay::get_stick_deadzone() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return left_stick_.deadzone;
}

void TouchControlsOverlay::recalculate_layout_locked() {
    const float sl = layout_.safe_left;
    const float sr = layout_.width - layout_.safe_right;
    const float st = layout_.safe_top;
    const float sb = layout_.height - layout_.safe_bottom;

    // Analógico Esquerdo (Movimento do Caçador):
    left_stick_.base_center = TouchCircle{sl + 110.0f, sb - 100.0f, 50.0f};
    left_stick_.bounds = TouchCircle{sl + 110.0f, sb - 100.0f, 75.0f};
    left_stick_.current_center = left_stick_.base_center;
    left_stick_.handle_x = left_stick_.base_center.center_x;
    left_stick_.handle_y = left_stick_.base_center.center_y;

    // Analógico Direito (Câmera do Jogo):
    right_stick_.base_center = TouchCircle{sr - 250.0f, sb - 100.0f, 50.0f};
    right_stick_.bounds = TouchCircle{sr - 250.0f, sb - 100.0f, 75.0f};
    right_stick_.current_center = right_stick_.base_center;
    right_stick_.handle_x = right_stick_.base_center.center_x;
    right_stick_.handle_y = right_stick_.base_center.center_y;

    // Inicializa botões virtuais
    buttons_.clear();

    // 1. D-Pad Virtual (acima do stick esquerdo)
    const float dpad_cx = sl + 110.0f;
    const float dpad_cy = st + 140.0f;
    const float dpad_dist = 42.0f;
    const float dpad_r = 24.0f;
    buttons_.push_back({VirtualControlId::DpadUp,    TouchCircle{dpad_cx, dpad_cy - dpad_dist, dpad_r}, PAD_UP, false, 0});
    buttons_.push_back({VirtualControlId::DpadDown,  TouchCircle{dpad_cx, dpad_cy + dpad_dist, dpad_r}, PAD_DOWN, false, 0});
    buttons_.push_back({VirtualControlId::DpadLeft,  TouchCircle{dpad_cx - dpad_dist, dpad_cy, dpad_r}, PAD_LEFT, false, 0});
    buttons_.push_back({VirtualControlId::DpadRight, TouchCircle{dpad_cx + dpad_dist, dpad_cy, dpad_r}, PAD_RIGHT, false, 0});

    // 2. Botões de Ação Virtuais (losango à direita)
    const float action_cx = sr - 100.0f;
    const float action_cy = sb - 100.0f;
    const float action_dist = 44.0f;
    const float action_r = 25.0f;
    buttons_.push_back({VirtualControlId::ButtonTriangle, TouchCircle{action_cx, action_cy - action_dist, action_r}, PAD_TRIANGLE, false, 0});
    buttons_.push_back({VirtualControlId::ButtonCircle,   TouchCircle{action_cx + action_dist, action_cy, action_r}, PAD_CIRCLE, false, 0});
    buttons_.push_back({VirtualControlId::ButtonCross,    TouchCircle{action_cx, action_cy + action_dist, action_r}, PAD_CROSS, false, 0});
    buttons_.push_back({VirtualControlId::ButtonSquare,   TouchCircle{action_cx - action_dist, action_cy, action_r}, PAD_SQUARE, false, 0});

    // 3. Gatilhos e botões superiores (L1, L2, R1, R2)
    buttons_.push_back({VirtualControlId::ButtonL1, TouchCircle{sl + 45.0f, st + 35.0f, 26.0f}, PAD_L1, false, 0});
    buttons_.push_back({VirtualControlId::ButtonL2, TouchCircle{sl + 115.0f, st + 35.0f, 26.0f}, PAD_L2, false, 0});
    buttons_.push_back({VirtualControlId::ButtonR1, TouchCircle{sr - 45.0f, st + 35.0f, 26.0f}, PAD_R1, false, 0});
    buttons_.push_back({VirtualControlId::ButtonR2, TouchCircle{sr - 115.0f, st + 35.0f, 26.0f}, PAD_R2, false, 0});

    // 4. Lock-on (R3), L3, Options e TouchPad
    buttons_.push_back({VirtualControlId::ButtonR3,       TouchCircle{sr - 250.0f, st + 140.0f, 25.0f}, PAD_R3, false, 0});
    buttons_.push_back({VirtualControlId::ButtonL3,       TouchCircle{sl + 185.0f, st + 35.0f, 22.0f}, PAD_L3, false, 0});
    buttons_.push_back({VirtualControlId::ButtonOptions,  TouchCircle{layout_.width * 0.5f + 40.0f, st + 30.0f, 20.0f}, PAD_OPTIONS, false, 0});
    buttons_.push_back({VirtualControlId::ButtonTouchPad, TouchCircle{layout_.width * 0.5f - 40.0f, st + 30.0f, 22.0f}, PAD_TOUCH_PAD, false, 0});
}

TouchCircle TouchControlsOverlay::get_control_bounds(VirtualControlId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id == VirtualControlId::LeftStick) return left_stick_.bounds;
    if (id == VirtualControlId::RightStick) return right_stick_.bounds;
    for (const auto& btn : buttons_) {
        if (btn.id == id) return btn.bounds;
    }
    return TouchCircle{};
}

TouchCircle TouchControlsOverlay::get_left_stick_bounds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return left_stick_.bounds;
}

TouchCircle TouchControlsOverlay::get_right_stick_bounds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return right_stick_.bounds;
}

bool TouchControlsOverlay::is_control_pressed(VirtualControlId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& btn : buttons_) {
        if (btn.id == id) return btn.is_pressed;
    }
    if (id == VirtualControlId::LeftStick) return left_stick_.active_touch_id != 0;
    if (id == VirtualControlId::RightStick) return right_stick_.active_touch_id != 0;
    return false;
}

std::size_t TouchControlsOverlay::active_touch_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_touch_count_locked();
}

std::size_t TouchControlsOverlay::active_touch_count_locked() const {
    std::size_t count = 0;
    for (const auto& t : active_touches_) {
        if (t.active) ++count;
    }
    return count;
}

bool TouchControlsOverlay::is_touch_active(std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return find_active_touch_locked(id) != nullptr;
}

bool TouchControlsOverlay::get_active_touch(std::uint64_t id, ActiveTouch& out_touch) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* t = find_active_touch_locked(id);
    if (!t) return false;
    out_touch = *t;
    return true;
}

ActiveTouch* TouchControlsOverlay::find_active_touch_locked(std::uint64_t id) {
    for (auto& t : active_touches_) {
        if (t.active && t.id == id) return &t;
    }
    return nullptr;
}

const ActiveTouch* TouchControlsOverlay::find_active_touch_locked(std::uint64_t id) const {
    for (const auto& t : active_touches_) {
        if (t.active && t.id == id) return &t;
    }
    return nullptr;
}

void TouchControlsOverlay::update_stick_locked(TouchStickState& stick, float touch_x, float touch_y, bool is_right_stick) {
    const float dx = touch_x - stick.current_center.center_x;
    const float dy = touch_y - stick.current_center.center_y;
    const float dist = std::sqrt(dx * dx + dy * dy);

    float norm_x = 0.0f;
    float norm_y = 0.0f;
    if (dist > 0.0001f && stick.travel_radius > 0.0001f) {
        const float clamped_dist = std::min(dist, stick.travel_radius);
        norm_x = (dx / dist) * (clamped_dist / stick.travel_radius);
        norm_y = (dy / dist) * (clamped_dist / stick.travel_radius);
        stick.handle_x = stick.current_center.center_x + (dx / dist) * clamped_dist;
        stick.handle_y = stick.current_center.center_y + (dy / dist) * clamped_dist;
    } else {
        stick.handle_x = stick.current_center.center_x;
        stick.handle_y = stick.current_center.center_y;
    }

    float out_x = 0.0f;
    float out_y = 0.0f;
    InputManager::apply_radial_deadzone(norm_x, norm_y, stick.deadzone, out_x, out_y);

    if (is_right_stick) {
        out_x = std::clamp(out_x * stick.sensitivity, -1.0f, 1.0f);
        out_y = std::clamp(out_y * stick.sensitivity, -1.0f, 1.0f);
        state_.right_stick_x = out_x;
        state_.right_stick_y = out_y;
    } else {
        state_.left_stick_x = out_x;
        state_.left_stick_y = out_y;
    }
}

void TouchControlsOverlay::release_control_locked(VirtualControlId control, std::uint64_t touch_id) {
    if (control == VirtualControlId::LeftStick && left_stick_.active_touch_id == touch_id) {
        left_stick_.active_touch_id = 0;
        left_stick_.handle_x = left_stick_.base_center.center_x;
        left_stick_.handle_y = left_stick_.base_center.center_y;
        left_stick_.current_center = left_stick_.base_center;
        state_.left_stick_x = 0.0f;
        state_.left_stick_y = 0.0f;
    } else if (control == VirtualControlId::RightStick && right_stick_.active_touch_id == touch_id) {
        right_stick_.active_touch_id = 0;
        right_stick_.handle_x = right_stick_.base_center.center_x;
        right_stick_.handle_y = right_stick_.base_center.center_y;
        right_stick_.current_center = right_stick_.base_center;
        state_.right_stick_x = 0.0f;
        state_.right_stick_y = 0.0f;
    } else {
        for (auto& btn : buttons_) {
            if (btn.id == control && btn.active_touch_id == touch_id) {
                btn.is_pressed = false;
                btn.active_touch_id = 0;
                state_.buttons &= ~btn.pad_mask;
                if (btn.id == VirtualControlId::ButtonL2) state_.l2_analog = 0.0f;
                if (btn.id == VirtualControlId::ButtonR2) state_.r2_analog = 0.0f;
                break;
            }
        }
    }
}

bool TouchControlsOverlay::on_touch_locked(const TouchPoint& touch) {
    if (touch.phase == TouchPhase::Began) {
        // Validação estrita de limites de tela e exclusão Dynamic Island
        if (!is_point_valid(touch.x, touch.y)) {
            return false;
        }

        // Se já existe toque com este ID ou capacidade máxima atingida, rejeitar
        if (find_active_touch_locked(touch.id) != nullptr) {
            return false;
        }
        if (active_touch_count_locked() >= MAX_SIMULTANEOUS_TOUCHES) {
            return false;
        }

        VirtualControlId hit_control = VirtualControlId::None;

        // 1. Prioridade para botões digitais e gatilhos
        for (auto& btn : buttons_) {
            if (btn.bounds.contains(touch.x, touch.y)) {
                hit_control = btn.id;
                btn.is_pressed = true;
                btn.active_touch_id = touch.id;
                state_.buttons |= btn.pad_mask;
                if (btn.id == VirtualControlId::ButtonL2) state_.l2_analog = 1.0f;
                if (btn.id == VirtualControlId::ButtonR2) state_.r2_analog = 1.0f;
                break;
            }
        }

        // 2. Se não atingiu botão, testar analógico esquerdo
        if (hit_control == VirtualControlId::None && left_stick_.active_touch_id == 0) {
            bool hit_left = false;
            if (left_stick_.mode == StickMode::Static) {
                hit_left = left_stick_.bounds.contains(touch.x, touch.y);
            } else {
                hit_left = (touch.x >= layout_.safe_left && touch.x <= (layout_.width * 0.45f) &&
                            touch.y >= (layout_.height * 0.20f) && touch.y <= (layout_.height - layout_.safe_bottom));
            }
            if (hit_left) {
                hit_control = VirtualControlId::LeftStick;
                left_stick_.active_touch_id = touch.id;
                if (left_stick_.mode == StickMode::Floating) {
                    left_stick_.current_center.center_x = touch.x;
                    left_stick_.current_center.center_y = touch.y;
                }
                update_stick_locked(left_stick_, touch.x, touch.y, false);
            }
        }

        // 3. Se não atingiu botão nem analógico esquerdo, testar analógico direito
        if (hit_control == VirtualControlId::None && right_stick_.active_touch_id == 0) {
            bool hit_right = false;
            if (right_stick_.mode == StickMode::Static) {
                hit_right = right_stick_.bounds.contains(touch.x, touch.y);
            } else {
                hit_right = (touch.x >= (layout_.width * 0.45f) && touch.x <= (layout_.width - layout_.safe_right) &&
                             touch.y >= (layout_.height * 0.20f) && touch.y <= (layout_.height - layout_.safe_bottom));
            }
            if (hit_right) {
                hit_control = VirtualControlId::RightStick;
                right_stick_.active_touch_id = touch.id;
                if (right_stick_.mode == StickMode::Floating) {
                    right_stick_.current_center.center_x = touch.x;
                    right_stick_.current_center.center_y = touch.y;
                }
                update_stick_locked(right_stick_, touch.x, touch.y, true);
            }
        }

        // Aloca slot no buffer de toques ativos
        for (auto& slot : active_touches_) {
            if (!slot.active) {
                slot.id = touch.id;
                slot.start_x = touch.x;
                slot.start_y = touch.y;
                slot.current_x = touch.x;
                slot.current_y = touch.y;
                slot.radius = touch.radius;
                slot.phase = TouchPhase::Began;
                slot.bound_control = hit_control;
                slot.active = true;
                break;
            }
        }

        sync_input_manager_locked();
        return true;
    }

    if (touch.phase == TouchPhase::Moved) {
        auto* entry = find_active_touch_locked(touch.id);
        if (!entry) return false;

        entry->current_x = touch.x;
        entry->current_y = touch.y;
        entry->radius = touch.radius;
        entry->phase = TouchPhase::Moved;

        if (entry->bound_control == VirtualControlId::LeftStick) {
            update_stick_locked(left_stick_, touch.x, touch.y, false);
        } else if (entry->bound_control == VirtualControlId::RightStick) {
            update_stick_locked(right_stick_, touch.x, touch.y, true);
        }

        sync_input_manager_locked();
        return true;
    }

    if (touch.phase == TouchPhase::Stationary) {
        auto* entry = find_active_touch_locked(touch.id);
        if (!entry) return false;
        entry->phase = TouchPhase::Stationary;
        return true;
    }

    if (touch.phase == TouchPhase::Ended || touch.phase == TouchPhase::Cancelled) {
        auto* entry = find_active_touch_locked(touch.id);
        if (!entry) return false;

        release_control_locked(entry->bound_control, touch.id);
        entry->active = false;
        entry->bound_control = VirtualControlId::None;

        sync_input_manager_locked();
        return true;
    }

    return false;
}

bool TouchControlsOverlay::on_touch(const TouchPoint& touch) {
    std::lock_guard<std::mutex> lock(mutex_);
    return on_touch_locked(touch);
}

bool TouchControlsOverlay::on_touch(std::uint64_t id, float x, float y, TouchPhase phase, float radius) {
    TouchPoint pt{};
    pt.id = id;
    pt.x = x;
    pt.y = y;
    pt.radius = radius;
    pt.phase = phase;
    return on_touch(pt);
}

bool TouchControlsOverlay::on_touch_down(std::uint64_t id, float x, float y, float radius) {
    return on_touch(id, x, y, TouchPhase::Began, radius);
}

bool TouchControlsOverlay::on_touch_move(std::uint64_t id, float x, float y, float radius) {
    return on_touch(id, x, y, TouchPhase::Moved, radius);
}

bool TouchControlsOverlay::on_touch_up(std::uint64_t id, float x, float y) {
    return on_touch(id, x, y, TouchPhase::Ended, 1.0f);
}

bool TouchControlsOverlay::on_touch_cancel(std::uint64_t id) {
    auto* entry = [this, id]() -> const ActiveTouch* {
        std::lock_guard<std::mutex> lock(mutex_);
        return find_active_touch_locked(id);
    }();
    if (!entry) return false;
    return on_touch(id, entry->current_x, entry->current_y, TouchPhase::Cancelled, entry->radius);
}

void TouchControlsOverlay::cancel_all_touches() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& t : active_touches_) {
        if (t.active) {
            release_control_locked(t.bound_control, t.id);
            t.active = false;
            t.bound_control = VirtualControlId::None;
        }
    }
    state_.buttons = 0;
    state_.left_stick_x = 0.0f;
    state_.left_stick_y = 0.0f;
    state_.right_stick_x = 0.0f;
    state_.right_stick_y = 0.0f;
    state_.l2_analog = 0.0f;
    state_.r2_analog = 0.0f;

    left_stick_.active_touch_id = 0;
    left_stick_.handle_x = left_stick_.base_center.center_x;
    left_stick_.handle_y = left_stick_.base_center.center_y;
    left_stick_.current_center = left_stick_.base_center;

    right_stick_.active_touch_id = 0;
    right_stick_.handle_x = right_stick_.base_center.center_x;
    right_stick_.handle_y = right_stick_.base_center.center_y;
    right_stick_.current_center = right_stick_.base_center;

    sync_input_manager_locked();
}

void TouchControlsOverlay::reset() {
    cancel_all_touches();
    std::lock_guard<std::mutex> lock(mutex_);
    recalculate_layout_locked();
    sync_input_manager_locked();
}

PadData TouchControlsOverlay::get_pad_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void TouchControlsOverlay::update_pad(InputManager& input) const {
    std::lock_guard<std::mutex> lock(mutex_);
    input.set_deadzone(0.0f);
    input.set_pad_state(state_, InputSource::Touch);
}

void TouchControlsOverlay::set_pad_state(InputManager& input) const {
    update_pad(input);
}

void TouchControlsOverlay::update_pad() {
    std::lock_guard<std::mutex> lock(mutex_);
    sync_input_manager_locked();
}

void TouchControlsOverlay::bind_input_manager(InputManager* input) {
    std::lock_guard<std::mutex> lock(mutex_);
    input_manager_ = input;
    if (input_manager_) {
        input_manager_->set_deadzone(0.0f);
    }
    sync_input_manager_locked();
}

InputManager* TouchControlsOverlay::get_input_manager() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return input_manager_;
}

void TouchControlsOverlay::sync_input_manager_locked() {
    if (input_manager_ != nullptr) {
        input_manager_->set_pad_state(state_, InputSource::Touch);
    }
}

void TouchControlsOverlay::set_vibration(std::uint8_t large, std::uint8_t small) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_haptic_ = HapticEvent::translate_vibration(large, small);
    haptic_history_.push_back(current_haptic_);
    if (haptic_callback_) {
        haptic_callback_(current_haptic_);
    }
}

void TouchControlsOverlay::set_vibration(const ScePadVibrationParam& param) {
    set_vibration(param.large_motor, param.small_motor);
}

HapticEvent TouchControlsOverlay::get_current_haptic() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_haptic_;
}

void TouchControlsOverlay::update_haptics_from_hle(const PadHleManager& hle, std::int32_t handle) {
    const ScePadVibrationParam vib = hle.get_vibration(handle);
    set_vibration(vib);
}

void TouchControlsOverlay::set_haptic_callback(HapticCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    haptic_callback_ = std::move(cb);
}

std::vector<HapticEvent> TouchControlsOverlay::poll_haptic_events() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HapticEvent> out = std::move(haptic_history_);
    haptic_history_.clear();
    return out;
}

void TouchControlsOverlay::set_opacity(float opacity) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_opacity_ = std::clamp(opacity, 0.0f, 1.0f);
    if (!gamepad_connected_ || !auto_hide_on_gamepad_) {
        current_opacity_ = default_opacity_;
    }
}

float TouchControlsOverlay::get_opacity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_opacity_;
}

void TouchControlsOverlay::set_visible(bool visible) {
    std::lock_guard<std::mutex> lock(mutex_);
    visible_ = visible;
    if (!visible_) {
        current_opacity_ = 0.0f;
    } else {
        current_opacity_ = (gamepad_connected_ && auto_hide_on_gamepad_) ? 0.0f : default_opacity_;
    }
}

bool TouchControlsOverlay::is_visible() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return visible_ && current_opacity_ > 0.01f;
}

void TouchControlsOverlay::set_gamepad_connected(bool connected) {
    std::lock_guard<std::mutex> lock(mutex_);
    gamepad_connected_ = connected;
    if (auto_hide_on_gamepad_) {
        current_opacity_ = connected ? 0.0f : default_opacity_;
    }
}

bool TouchControlsOverlay::is_gamepad_connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gamepad_connected_;
}

void TouchControlsOverlay::set_auto_hide_on_gamepad(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto_hide_on_gamepad_ = enable;
    if (enable && gamepad_connected_) {
        current_opacity_ = 0.0f;
    } else if (!enable && visible_) {
        current_opacity_ = default_opacity_;
    }
}

bool TouchControlsOverlay::is_auto_hide_on_gamepad() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return auto_hide_on_gamepad_;
}

void TouchControlsOverlay::fade_update(float delta_time_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    const float target = (!visible_ || (gamepad_connected_ && auto_hide_on_gamepad_)) ? 0.0f : default_opacity_;
    const float speed = 5.0f;
    if (current_opacity_ < target) {
        current_opacity_ = std::min(target, current_opacity_ + speed * delta_time_sec);
    } else if (current_opacity_ > target) {
        current_opacity_ = std::max(target, current_opacity_ - speed * delta_time_sec);
    }
}

} // namespace bb
