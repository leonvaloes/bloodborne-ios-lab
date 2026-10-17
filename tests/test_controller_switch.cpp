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

        // 6. T040: TouchPad virtual e fluxo de desconexão de controle
        overlay.set_gamepad_connected(false);
        CHECK(overlay.is_visible());

        const auto tp_bounds = overlay.get_control_bounds(VirtualControlId::ButtonTouchPad);
        CHECK(tp_bounds.radius > 0.0f);
        CHECK(overlay.on_touch_down(42, tp_bounds.center_x, tp_bounds.center_y));
        CHECK(overlay.is_control_pressed(VirtualControlId::ButtonTouchPad));
        auto tp_state = input.poll();
        CHECK((tp_state.buttons & PAD_TOUCH_PAD) != 0);

        CHECK(overlay.on_touch_up(42, tp_bounds.center_x, tp_bounds.center_y));
        CHECK(!overlay.is_control_pressed(VirtualControlId::ButtonTouchPad));
        tp_state = input.poll();
        CHECK((tp_state.buttons & PAD_TOUCH_PAD) == 0);

        // Simula controle conectado com botões e touchpad retidos
        overlay.set_gamepad_connected(true);
        input.set_button(PAD_TOUCH_PAD | PAD_R2 | PAD_CROSS, true, InputSource::Gamepad);
        input.set_left_stick(0.8f, -0.6f, InputSource::Gamepad);
        CHECK((input.poll().buttons & PAD_TOUCH_PAD) != 0);

        // Desconexão: limpa entradas retidas do gamepad e reativa overlay de toque
        input.clear_source(InputSource::Gamepad);
        input.set_connected(false, InputSource::Gamepad);
        overlay.set_gamepad_connected(false);
        auto disconnected_state = input.poll();
        CHECK(disconnected_state.buttons == 0);
        CHECK(std::abs(disconnected_state.left_stick_x) < 0.001f);
        CHECK(std::abs(disconnected_state.left_stick_y) < 0.001f);
        CHECK(overlay.is_visible());

        // 7. T113: Rastreamento por identidade e múltiplos controles (FR-007)
        input.reset();
        overlay.reset();
        CHECK(input.connected_controllers_count() == 0);
        CHECK(input.active_controller_id() == 0);

        // 7a. Conexão do primeiro controle (DualSense primário, id=101)
        const std::uint64_t controller1_id = 101;
        bool activated1 = input.register_controller(controller1_id);
        CHECK(activated1);
        CHECK(input.is_controller_active(controller1_id));
        CHECK(input.active_controller_id() == controller1_id);
        CHECK(input.connected_controllers_count() == 1);
        overlay.set_gamepad_connected(true);
        CHECK(!overlay.is_visible());

        // Entrada enviada pelo controle primário
        input.set_button(PAD_CROSS, true, InputSource::Gamepad);
        CHECK((input.poll().buttons & PAD_CROSS) != 0);

        // 7b. Conexão de um segundo controle (MFi secundário, id=202)
        const std::uint64_t controller2_id = 202;
        bool activated2 = input.register_controller(controller2_id);
        CHECK(!activated2); // Não substitui o controle ativo
        CHECK(input.is_controller_active(controller1_id)); // Primário ainda ativo
        CHECK(input.connected_controllers_count() == 2);
        CHECK(!overlay.is_visible());

        // 7c. Desconexão do segundo controle (não ativo):
        // NÃO deve limpar entradas do gamepad, NÃO deve desconectar fonte e NÃO deve exibir controles na tela!
        bool changed_on_dc2 = input.unregister_controller(controller2_id);
        CHECK(!changed_on_dc2); // Ativo não mudou
        CHECK(input.is_controller_active(controller1_id));
        CHECK(input.connected_controllers_count() == 1);
        // Entradas do controle ativo permanecem íntegras
        CHECK((input.poll().buttons & PAD_CROSS) != 0);
        CHECK(!overlay.is_visible());

        // 7d. Reconecta controle 2 e agora desconecta o controle primário (id=101)
        input.register_controller(controller2_id);
        CHECK(input.connected_controllers_count() == 2);

        // Desconecta primário: alterna automaticamente para o controle 2 remanescente
        bool changed_on_dc1 = input.unregister_controller(controller1_id);
        CHECK(changed_on_dc1);
        CHECK(input.is_controller_active(controller2_id));
        CHECK(input.active_controller_id() == controller2_id);
        CHECK(input.connected_controllers_count() == 1);
        // Como ainda há um controle ativo, overlay permanece oculto
        CHECK(!overlay.is_visible());

        // 7e. Desconecta o último controle restante: desconexão completa
        bool changed_on_dc_last = input.unregister_controller(controller2_id);
        CHECK(changed_on_dc_last);
        CHECK(input.active_controller_id() == 0);
        CHECK(input.connected_controllers_count() == 0);
        overlay.set_gamepad_connected(false);
        // Entradas do gamepad foram limpas e camada tátil reativada
        CHECK(input.poll().buttons == 0);
        CHECK(overlay.is_visible());

        // ========================================================================
        // 8. T161: Limpeza do estado agregado ao trocar controle ativo com entradas retidas
        // ========================================================================
        input.reset();
        overlay.reset();

        const std::uint64_t padA = 501;
        const std::uint64_t padB = 502;
        CHECK(input.register_controller(padA));
        CHECK(!input.register_controller(padB)); // Nao ativa de imediato pois padA já está ativo
        CHECK(input.is_controller_active(padA));
        CHECK(input.connected_controllers_count() == 2);

        // Fonte de toque possui entrada ativa concorrente (L1)
        input.set_button(PAD_L1, true, InputSource::Touch);
        CHECK((input.poll().buttons & PAD_L1) != 0);

        // Controle A (ativo) envia comandos retidos: Cross, Triangle e stick (-0.8, 0.5)
        input.set_button(PAD_CROSS | PAD_TRIANGLE, true, InputSource::Gamepad);
        input.set_left_stick(-0.8f, 0.5f, InputSource::Gamepad);

        auto state_a = input.poll();
        CHECK((state_a.buttons & PAD_L1) != 0); // Toque preservado
        CHECK((state_a.buttons & PAD_CROSS) != 0);
        CHECK((state_a.buttons & PAD_TRIANGLE) != 0);
        CHECK(state_a.left_stick_x < -0.6f);

        // Troca direta da identidade do controle ativo para padB
        CHECK(input.set_active_controller(padB));
        CHECK(input.is_controller_active(padB));
        CHECK(input.active_controller_id() == padB);

        // O estado do gamepad foi limpo: botoes e eixos retidos do padA foram descartados
        auto state_switched = input.poll();
        CHECK((state_switched.buttons & PAD_CROSS) == 0);
        CHECK((state_switched.buttons & PAD_TRIANGLE) == 0);
        CHECK(std::abs(state_switched.left_stick_x) < 0.001f);
        // A entrada da outra fonte (Toque) permanece estritamente preservada!
        CHECK((state_switched.buttons & PAD_L1) != 0);

        // Novo controle (padB) envia suas proprias entradas
        input.set_button(PAD_CIRCLE, true, InputSource::Gamepad);
        input.set_left_stick(0.7f, 0.0f, InputSource::Gamepad);
        auto state_b = input.poll();
        CHECK((state_b.buttons & PAD_L1) != 0);
        CHECK((state_b.buttons & PAD_CIRCLE) != 0);
        CHECK((state_b.buttons & PAD_CROSS) == 0);
        CHECK(state_b.left_stick_x > 0.5f);

        // Troca de volta para padA: limpa o estado agregado de padB
        CHECK(input.set_active_controller(padA));
        auto state_back_a = input.poll();
        CHECK((state_back_a.buttons & PAD_CIRCLE) == 0);
        CHECK(std::abs(state_back_a.left_stick_x) < 0.001f);
        CHECK((state_back_a.buttons & PAD_L1) != 0); // Toque intacto

        // Limpeza
        input.set_button(PAD_L1, false, InputSource::Touch);
        CHECK(input.poll().buttons == 0);
    });
}
