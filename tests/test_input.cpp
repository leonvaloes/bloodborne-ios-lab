#include "check.h"
#include "input.h"
#include "guest_memory.h"
#include <cmath>

int main() { return run([] {
    using namespace bb;
    InputManager input;

    // Estado inicial
    auto state = input.poll();
    CHECK(state.connected);
    CHECK(state.buttons == 0);
    CHECK(std::abs(state.left_stick_x) < 0.001f);
    CHECK(std::abs(state.left_stick_y) < 0.001f);
    CHECK(std::abs(state.right_stick_x) < 0.001f);
    CHECK(std::abs(state.right_stick_y) < 0.001f);
    CHECK(std::abs(state.l2_analog) < 0.001f);
    CHECK(std::abs(state.r2_analog) < 0.001f);

    // Botões digitais
    input.set_button(PAD_CROSS, true);
    input.set_button(PAD_R1, true);
    state = input.poll();
    CHECK((state.buttons & PAD_CROSS) != 0);
    CHECK((state.buttons & PAD_R1) != 0);
    CHECK((state.buttons & PAD_CIRCLE) == 0);

    input.set_button(PAD_CROSS, false);
    state = input.poll();
    CHECK((state.buttons & PAD_CROSS) == 0);
    CHECK((state.buttons & PAD_R1) != 0);

    // Máscara direta
    input.set_buttons_mask(PAD_OPTIONS | PAD_UP | PAD_TRIANGLE);
    state = input.poll();
    CHECK(state.buttons == (PAD_OPTIONS | PAD_UP | PAD_TRIANGLE));

    // Deadzone analógica estática
    CHECK(InputManager::apply_deadzone(0.05f, 0.15f) == 0.0f);
    CHECK(InputManager::apply_deadzone(-0.10f, 0.15f) == 0.0f);
    CHECK(InputManager::apply_deadzone(1.0f, 0.15f) == 1.0f);
    CHECK(InputManager::apply_deadzone(-1.0f, 0.15f) == -1.0f);

    float rx = 0, ry = 0;
    InputManager::apply_radial_deadzone(0.05f, 0.05f, 0.15f, rx, ry);
    CHECK(rx == 0.0f && ry == 0.0f);

    InputManager::apply_radial_deadzone(0.6f, 0.8f, 0.2f, rx, ry);
    CHECK(std::abs(std::sqrt(rx * rx + ry * ry) - 1.0f) < 0.001f);

    // Sticks com deadzone configurada no manager
    input.reset();
    input.set_deadzone(0.2f);
    input.set_left_stick(0.1f, -0.1f); // dentro da deadzone
    state = input.poll();
    CHECK(std::abs(state.left_stick_x) < 0.001f);
    CHECK(std::abs(state.left_stick_y) < 0.001f);

    input.set_left_stick(0.0f, 1.0f); // fora da deadzone
    state = input.poll();
    CHECK(std::abs(state.left_stick_y - 1.0f) < 0.001f);

    // Gatilhos analógicos
    input.set_triggers(0.85f, 0.10f);
    state = input.poll();
    CHECK(std::abs(state.l2_analog - 0.85f) < 0.001f);
    CHECK(std::abs(state.r2_analog - 0.10f) < 0.001f);
    CHECK((state.buttons & PAD_L2) != 0); // Ativado pelo limiar > 0.2
    CHECK((state.buttons & PAD_R2) == 0);

    // Serialização HLE na memória do guest
    GuestMemory mem(64);
    CHECK(mem.map(0x1000, 64, read | write) == MemoryError::ok);
    CHECK(input.write_hle_pad_data(mem, 0x1000) == MemoryError::ok);

    std::uint32_t read_buttons = 0;
    CHECK(mem.load(0x1000, &read_buttons, sizeof(read_buttons)) == MemoryError::ok);
    CHECK((read_buttons & PAD_L2) != 0);

    std::uint8_t stick_y = 0;
    CHECK(mem.load(0x1005, &stick_y, sizeof(stick_y)) == MemoryError::ok);
    CHECK(stick_y == 255); // 1.0 mapeado para 255

    std::uint32_t connected = 0;
    CHECK(mem.load(0x100C, &connected, sizeof(connected)) == MemoryError::ok);
    CHECK(connected == 1);
}); }
