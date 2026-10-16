#include "check.h"
#include "touch_controls.h"
#include "input.h"
#include <cmath>

int main() {
    return run([] {
        using namespace bb;

        InputManager input;
        TouchControlsOverlay overlay(&input);

        // 1. Estado inicial padrão: camada tátil ativa e visível
        CHECK(overlay.is_visible());
        CHECK(!overlay.is_gamepad_connected());
        CHECK(overlay.is_auto_hide_on_gamepad());
        CHECK(std::abs(overlay.get_opacity() - 0.65f) < 0.01f);

        // 2. Conexão de controle físico Bluetooth / DualSense (T022 auto-ocultação)
        overlay.set_gamepad_connected(true);
        CHECK(overlay.is_gamepad_connected());
        CHECK(!overlay.is_visible());
        CHECK(overlay.get_opacity() < 0.01f);

        // Simula comando do gamepad físico enquanto a camada tátil está auto-ocultada
        input.set_button(PAD_R1, true);
        input.set_left_stick(0.0f, -0.8f);
        auto pad_state = input.poll();
        CHECK((pad_state.buttons & PAD_R1) != 0);
        CHECK(pad_state.left_stick_y < -0.7f);

        // 3. Desconexão do controle físico: reativação imediata da camada de toque
        overlay.set_gamepad_connected(false);
        CHECK(!overlay.is_gamepad_connected());
        CHECK(overlay.is_visible());
        CHECK(std::abs(overlay.get_opacity() - 0.65f) < 0.01f);

        // Validação de entrada tátil imediata após reconexão
        input.set_button(PAD_R1, false);
        const auto r1_bounds = overlay.get_control_bounds(VirtualControlId::ButtonR1);
        CHECK(overlay.on_touch_down(10, r1_bounds.center_x, r1_bounds.center_y));
        pad_state = input.poll();
        CHECK((pad_state.buttons & PAD_R1) != 0);

        CHECK(overlay.on_touch_up(10, r1_bounds.center_x, r1_bounds.center_y));
        pad_state = input.poll();
        CHECK((pad_state.buttons & PAD_R1) == 0);

        // 4. Teste de atenuação gradual / fade_update
        overlay.set_opacity(0.80f);
        CHECK(std::abs(overlay.get_opacity() - 0.80f) < 0.01f);

        // Conecta gamepad e testa atenuação gradual (fade out)
        overlay.set_auto_hide_on_gamepad(true);
        overlay.set_gamepad_connected(true);
        // O fade_update reduz gradualmente a opacidade até 0.0f
        for (int i = 0; i < 10; ++i) {
            overlay.fade_update(0.05f); // 50 ms por passo
        }
        CHECK(overlay.get_opacity() < 0.01f);
        CHECK(!overlay.is_visible());

        // Desconecta e testa fade in gradual
        overlay.set_gamepad_connected(false);
        CHECK(overlay.is_visible());
        for (int i = 0; i < 10; ++i) {
            overlay.fade_update(0.05f);
        }
        CHECK(overlay.get_opacity() >= 0.79f);

        // 5. Override manual de auto-ocultação
        overlay.set_auto_hide_on_gamepad(false);
        overlay.set_gamepad_connected(true);
        CHECK(overlay.is_visible()); // Permanece visível por opção do usuário
        CHECK(overlay.get_opacity() >= 0.79f);

        // Restaura auto-ocultação
        overlay.set_auto_hide_on_gamepad(true);
        CHECK(!overlay.is_visible());
        CHECK(overlay.get_opacity() < 0.01f);
    });
}
