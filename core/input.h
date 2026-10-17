#ifndef BB_INPUT_H
#define BB_INPUT_H

#include <cstdint>
#include <mutex>
#include <array>
#include "guest_memory.h"

namespace bb {

// Origem da entrada (Touch virtual x Gamepad físico)
enum class InputSource : std::uint8_t {
    Touch = 0,
    Gamepad = 1,
    Count = 2
};

// Códigos de botões padrão DualShock 4 / PS4
enum PadButtons : std::uint32_t {
    PAD_L3        = 0x00000002,
    PAD_R3        = 0x00000004,
    PAD_OPTIONS   = 0x00000008,
    PAD_UP        = 0x00000010,
    PAD_RIGHT     = 0x00000020,
    PAD_DOWN      = 0x00000040,
    PAD_LEFT      = 0x00000080,
    PAD_L2        = 0x00000100,
    PAD_R2        = 0x00000200,
    PAD_L1        = 0x00000400,
    PAD_R1        = 0x00000800,
    PAD_TRIANGLE  = 0x00001000,
    PAD_CIRCLE    = 0x00002000,
    PAD_CROSS     = 0x00004000,
    PAD_SQUARE    = 0x00008000,
    PAD_TOUCH_PAD = 0x00100000
};

// Estrutura de estado de entrada do controle
struct PadData {
    std::uint32_t buttons{0};
    float left_stick_x{0.0f};
    float left_stick_y{0.0f};
    float right_stick_x{0.0f};
    float right_stick_y{0.0f};
    float l2_analog{0.0f};
    float r2_analog{0.0f};
    bool connected{true};
};

class InputManager {
public:
    InputManager();

    void set_button(std::uint32_t button, bool pressed, InputSource source = InputSource::Gamepad);
    void set_buttons_mask(std::uint32_t mask, InputSource source = InputSource::Gamepad);
    void set_left_stick(float x, float y, InputSource source = InputSource::Gamepad);
    void set_right_stick(float x, float y, InputSource source = InputSource::Gamepad);
    void set_triggers(float l2, float r2, InputSource source = InputSource::Gamepad);
    void set_connected(bool connected);
    void set_connected(bool connected, InputSource source);
    void set_deadzone(float deadzone);
    void set_pad_state(const PadData &state, InputSource source = InputSource::Gamepad);
    void clear_source(InputSource source);

    PadData poll() const;
    PadData poll_source(InputSource source) const;
    void reset();

    // HLE: Escreve estrutura ScePadData formatada para a memória guest
    MemoryError write_hle_pad_data(GuestMemory &mem, std::uint64_t addr) const;

    // Helpers estáticos de zona morta (deadzone)
    static float apply_deadzone(float val, float threshold);
    static void apply_radial_deadzone(float in_x, float in_y, float threshold, float &out_x, float &out_y);

    // Multi-controller identity tracking (T113 / FR-007)
    bool register_controller(std::uint64_t controller_id);
    bool unregister_controller(std::uint64_t controller_id);
    bool set_active_controller(std::uint64_t controller_id);
    std::uint64_t active_controller_id() const;
    std::size_t connected_controllers_count() const;
    bool is_controller_active(std::uint64_t controller_id) const;

private:
    mutable std::mutex mutex_;
    std::array<PadData, static_cast<std::size_t>(InputSource::Count)> sources_{};
    std::vector<std::uint64_t> registered_controllers_;
    std::uint64_t active_controller_id_{0};
    bool connected_{true};
    float deadzone_{0.15f};
};

} // namespace bb

#endif // BB_INPUT_H
