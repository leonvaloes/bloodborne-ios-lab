#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>
#include "input.h"
#include "pad_hle.h"

namespace bb {

// Constantes de tela padrão para iPhone 15 Pro Max (6.7" Super Retina XDR) em orientação paisagem (Landscape)
constexpr float IPHONE_15_PRO_MAX_WIDTH_PT       = 932.0f;
constexpr float IPHONE_15_PRO_MAX_HEIGHT_PT      = 430.0f;
constexpr float IPHONE_15_PRO_MAX_SAFE_LEFT_PT   = 59.0f;  // Margem segura Dynamic Island (lado esquerdo)
constexpr float IPHONE_15_PRO_MAX_SAFE_RIGHT_PT  = 59.0f;  // Margem segura simétrica / borda direita
constexpr float IPHONE_15_PRO_MAX_SAFE_TOP_PT    = 0.0f;
constexpr float IPHONE_15_PRO_MAX_SAFE_BOTTOM_PT = 21.0f;  // Home indicator (borda inferior)

constexpr std::size_t MAX_SIMULTANEOUS_TOUCHES = 10;

// Fases de toque (compatível com UITouchPhase no iOS)
enum class TouchPhase {
    Began,
    Moved,
    Stationary,
    Ended,
    Cancelled
};

// Identificadores de controles virtuais overlay
enum class VirtualControlId {
    None = 0,
    LeftStick,
    RightStick,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    ButtonTriangle,
    ButtonCircle,
    ButtonCross,
    ButtonSquare,
    ButtonL1,
    ButtonL2,
    ButtonR1,
    ButtonR2,
    ButtonOptions,
    ButtonR3,   // Lock-on (R3)
    ButtonL3
};

// Modo de operação dos analógicos virtuais
enum class StickMode {
    Static,     // Posição central fixa na tela
    Floating    // Origem dinâmica ancorada no ponto onde o toque iniciou
};

// Tipos de feedback háptico mapeados a partir da vibração libScePad
enum class HapticType {
    None = 0,
    Light,      // UIImpactFeedbackGeneratorStyleLight (swings leves, feedback sutil)
    Medium,     // UIImpactFeedbackGeneratorStyleMedium (golpes médios, esquivas)
    Heavy,      // UIImpactFeedbackGeneratorStyleHeavy (impactos pesados, ataques viscerais, golpes de boss)
    Selection,  // UISelectionFeedbackGenerator (cliques de menu, ticks finos)
    Impact      // CoreHaptics / impacto com amplitude e nitidez (sharpness) combinadas
};

// Estruturas geométricas 2D para hit testing
struct TouchRect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};

    bool contains(float px, float py) const {
        return px >= x && px <= (x + width) && py >= y && py <= (y + height);
    }
    float center_x() const { return x + width * 0.5f; }
    float center_y() const { return y + height * 0.5f; }
};

struct TouchCircle {
    float center_x{0.0f};
    float center_y{0.0f};
    float radius{0.0f};

    bool contains(float px, float py) const {
        const float dx = px - center_x;
        const float dy = py - center_y;
        return (dx * dx + dy * dy) <= (radius * radius);
    }
};

// Configuração de dimensões de tela e safe area
struct ScreenLayoutConfig {
    float width{IPHONE_15_PRO_MAX_WIDTH_PT};
    float height{IPHONE_15_PRO_MAX_HEIGHT_PT};
    float safe_left{IPHONE_15_PRO_MAX_SAFE_LEFT_PT};
    float safe_right{IPHONE_15_PRO_MAX_SAFE_RIGHT_PT};
    float safe_top{IPHONE_15_PRO_MAX_SAFE_TOP_PT};
    float safe_bottom{IPHONE_15_PRO_MAX_SAFE_BOTTOM_PT};
    TouchRect dynamic_island_exclusion{0.0f, (IPHONE_15_PRO_MAX_HEIGHT_PT - 126.0f) * 0.5f, 40.0f, 126.0f};
    bool dynamic_island_exclusion_enabled{true};
};

// Ponto de toque independente
struct TouchPoint {
    std::uint64_t id{0};
    float x{0.0f};
    float y{0.0f};
    float radius{1.0f};
    TouchPhase phase{TouchPhase::Began};
    std::uint64_t timestamp{0};
};

// Registro de toque ativo
struct ActiveTouch {
    std::uint64_t id{0};
    float start_x{0.0f};
    float start_y{0.0f};
    float current_x{0.0f};
    float current_y{0.0f};
    float radius{1.0f};
    TouchPhase phase{TouchPhase::Began};
    VirtualControlId bound_control{VirtualControlId::None};
    bool active{false};
};

// Evento háptico tipado para iOS
struct HapticEvent {
    HapticType type{HapticType::None};
    float intensity{0.0f};      // [0.0f .. 1.0f] Intensidade/amplitude
    float sharpness{0.0f};      // [0.0f .. 1.0f] Nitidez/frequência para CoreHaptics
    float duration_ms{0.0f};    // Duração estimada em ms
    std::uint8_t large_motor{0};
    std::uint8_t small_motor{0};

    bool is_active() const { return type != HapticType::None; }
    const char* type_name() const;

    static HapticEvent translate_vibration(std::uint8_t large, std::uint8_t small);
    static HapticEvent translate_vibration(const ScePadVibrationParam& param);
};

// Estado interno de analógico virtual
struct TouchStickState {
    TouchCircle base_center{};
    TouchCircle current_center{};
    TouchCircle bounds{};
    float handle_x{0.0f};
    float handle_y{0.0f};
    float travel_radius{50.0f};
    float deadzone{0.15f};
    float sensitivity{1.0f};
    StickMode mode{StickMode::Static};
    std::uint64_t active_touch_id{0};
};

// Estado interno de botão virtual
struct VirtualButtonState {
    VirtualControlId id{VirtualControlId::None};
    TouchCircle bounds{};
    std::uint32_t pad_mask{0};
    bool is_pressed{false};
    std::uint64_t active_touch_id{0};
};

class TouchControlsOverlay {
public:
    explicit TouchControlsOverlay(InputManager* input = nullptr,
                                 const ScreenLayoutConfig& config = ScreenLayoutConfig{});
    ~TouchControlsOverlay() = default;

    // Configuração de layout e bounds
    void set_screen_layout(const ScreenLayoutConfig& config);
    ScreenLayoutConfig get_screen_layout() const;
    bool is_point_valid(float x, float y) const;
    bool is_in_exclusion_zone(float x, float y) const;

    // Modos dos analógicos e sensibilidade
    void set_left_stick_mode(StickMode mode);
    StickMode get_left_stick_mode() const;

    void set_right_stick_mode(StickMode mode);
    StickMode get_right_stick_mode() const;

    void set_camera_sensitivity(float sens);
    float get_camera_sensitivity() const;

    void set_stick_deadzone(float deadzone);
    float get_stick_deadzone() const;

    // Rastreamento multitoque
    bool on_touch(const TouchPoint& touch);
    bool on_touch(std::uint64_t id, float x, float y, TouchPhase phase, float radius = 1.0f);
    bool on_touch_down(std::uint64_t id, float x, float y, float radius = 1.0f);
    bool on_touch_move(std::uint64_t id, float x, float y, float radius = 1.0f);
    bool on_touch_up(std::uint64_t id, float x, float y);
    bool on_touch_cancel(std::uint64_t id);
    void cancel_all_touches();
    void reset();

    // Consulta de controles e estado
    TouchCircle get_control_bounds(VirtualControlId id) const;
    TouchCircle get_left_stick_bounds() const;
    TouchCircle get_right_stick_bounds() const;
    bool is_control_pressed(VirtualControlId id) const;
    std::size_t active_touch_count() const;
    bool is_touch_active(std::uint64_t id) const;
    bool get_active_touch(std::uint64_t id, ActiveTouch& out_touch) const;

    // Integração direta com InputManager
    PadData get_pad_state() const;
    void update_pad(InputManager& input) const;
    void set_pad_state(InputManager& input) const;
    void update_pad();
    void bind_input_manager(InputManager* input);
    InputManager* get_input_manager() const;

    // Feedback háptico
    void set_vibration(std::uint8_t large, std::uint8_t small);
    void set_vibration(const ScePadVibrationParam& param);
    HapticEvent get_current_haptic() const;
    void update_haptics_from_hle(const PadHleManager& hle, std::int32_t handle = 1);

    using HapticCallback = std::function<void(const HapticEvent&)>;
    void set_haptic_callback(HapticCallback cb);
    std::vector<HapticEvent> poll_haptic_events();

private:
    void recalculate_layout_locked();
    bool on_touch_locked(const TouchPoint& touch);
    void release_control_locked(VirtualControlId control, std::uint64_t touch_id);
    void update_stick_locked(TouchStickState& stick, float touch_x, float touch_y, bool is_right_stick);
    ActiveTouch* find_active_touch_locked(std::uint64_t id);
    const ActiveTouch* find_active_touch_locked(std::uint64_t id) const;
    std::size_t active_touch_count_locked() const;
    void sync_input_manager_locked();

    mutable std::mutex mutex_;
    ScreenLayoutConfig layout_{};
    InputManager* input_manager_{nullptr};

    std::array<ActiveTouch, MAX_SIMULTANEOUS_TOUCHES> active_touches_{};

    TouchStickState left_stick_{};
    TouchStickState right_stick_{};

    std::vector<VirtualButtonState> buttons_{};

    PadData state_{};

    HapticEvent current_haptic_{};
    std::vector<HapticEvent> haptic_history_{};
    HapticCallback haptic_callback_{nullptr};
};

} // namespace bb
