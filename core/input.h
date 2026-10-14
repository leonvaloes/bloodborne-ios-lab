#ifndef BB_INPUT_H
#define BB_INPUT_H

#include <cstdint>
#include <mutex>
#include "guest_memory.h"

namespace bb {

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

    void set_button(std::uint32_t button, bool pressed);
    void set_buttons_mask(std::uint32_t mask);
    void set_left_stick(float x, float y);
    void set_right_stick(float x, float y);
    void set_triggers(float l2, float r2);
    void set_connected(bool connected);
    void set_deadzone(float deadzone);

    PadData poll() const;
    void reset();

    // HLE: Escreve estrutura ScePadData formatada para a memória guest
    MemoryError write_hle_pad_data(GuestMemory &mem, std::uint64_t addr) const;

    // Helpers estáticos de zona morta (deadzone)
    static float apply_deadzone(float val, float threshold);
    static void apply_radial_deadzone(float in_x, float in_y, float threshold, float &out_x, float &out_y);

private:
    mutable std::mutex mutex_;
    PadData state_;
    float deadzone_{0.15f};
};

} // namespace bb

#endif // BB_INPUT_H
