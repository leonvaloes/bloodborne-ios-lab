#include "check.h"
#include "touch_controls.h"
#include "input.h"
#include "pad_hle.h"
#include <cmath>

static void test_multitouch_hunting_actions() {
    using namespace bb;
    InputManager input;
    TouchControlsOverlay overlay(&input);

    // Estado inicial neutro
    auto initial_state = input.poll();
    CHECK(initial_state.buttons == 0);
    CHECK(std::abs(initial_state.left_stick_x) < 0.001f);
    CHECK(std::abs(initial_state.left_stick_y) < 0.001f);
    CHECK(std::abs(initial_state.right_stick_x) < 0.001f);
    CHECK(std::abs(initial_state.right_stick_y) < 0.001f);
    CHECK(overlay.active_touch_count() == 0);

    const auto left_bounds = overlay.get_left_stick_bounds();
    const auto right_bounds = overlay.get_right_stick_bounds();
    const auto circle_bounds = overlay.get_control_bounds(VirtualControlId::ButtonCircle);

    // Toque 1: Caçador anda para a frente (Stick Esquerdo empurrado para cima)
    const float touch1_x = left_bounds.center_x;
    const float touch1_y = left_bounds.center_y - 45.0f;
    CHECK(overlay.on_touch_down(1, touch1_x, touch1_y));
    CHECK(overlay.active_touch_count() == 1);

    auto state1 = input.poll();
    CHECK(state1.left_stick_y < -0.6f);
    CHECK(state1.buttons == 0);

    // Toque 2: Esquiva simultânea com botão Circle (O)
    CHECK(overlay.on_touch_down(2, circle_bounds.center_x, circle_bounds.center_y));
    CHECK(overlay.active_touch_count() == 2);

    auto state2 = input.poll();
    CHECK((state2.buttons & PAD_CIRCLE) != 0);
    CHECK(state2.left_stick_y < -0.6f); // Movimento continua ativo

    // Toque 3: Controle simultâneo de câmera para a direita (Stick Direito)
    const float touch3_x = right_bounds.center_x + 45.0f;
    const float touch3_y = right_bounds.center_y;
    CHECK(overlay.on_touch_down(3, touch3_x, touch3_y));
    CHECK(overlay.active_touch_count() == 3);

    auto state3 = input.poll();
    CHECK((state3.buttons & PAD_CIRCLE) != 0); // Esquiva
    CHECK(state3.left_stick_y < -0.6f);       // Andar
    CHECK(state3.right_stick_x > 0.6f);       // Câmera girando à direita

    // Libera Toque 2 (Esquiva concluída)
    CHECK(overlay.on_touch_up(2, circle_bounds.center_x, circle_bounds.center_y));
    CHECK(overlay.active_touch_count() == 2);

    auto state4 = input.poll();
    CHECK((state4.buttons & PAD_CIRCLE) == 0); // Botão solto
    CHECK(state4.left_stick_y < -0.6f);        // Analógicos permanecem ativos
    CHECK(state4.right_stick_x > 0.6f);

    // Libera analógicos
    CHECK(overlay.on_touch_up(1, touch1_x, touch1_y));
    CHECK(overlay.on_touch_up(3, touch3_x, touch3_y));
    CHECK(overlay.active_touch_count() == 0);

    auto state5 = input.poll();
    CHECK(state5.buttons == 0);
    CHECK(std::abs(state5.left_stick_x) < 0.001f);
    CHECK(std::abs(state5.left_stick_y) < 0.001f);
    CHECK(std::abs(state5.right_stick_x) < 0.001f);
    CHECK(std::abs(state5.right_stick_y) < 0.001f);
}

static void test_out_of_bounds_and_exclusion_rejection() {
    using namespace bb;
    TouchControlsOverlay overlay;

    // Toques fora dos limites da tela física (iPhone 15 Pro Max: 932x430 pt)
    CHECK(!overlay.on_touch_down(10, -5.0f, 100.0f));
    CHECK(!overlay.on_touch_down(11, 950.0f, 100.0f));
    CHECK(!overlay.on_touch_down(12, 100.0f, -10.0f));
    CHECK(!overlay.on_touch_down(13, 100.0f, 450.0f));
    CHECK(overlay.active_touch_count() == 0);

    // Toque dentro da área de exclusão do Dynamic Island (Landscape: [0..40] x [152..278])
    CHECK(overlay.is_in_exclusion_zone(20.0f, 215.0f));
    CHECK(!overlay.on_touch_down(14, 20.0f, 215.0f));
    CHECK(overlay.active_touch_count() == 0);

    // Toque imediatamente fora do recorte do Dynamic Island deve ser aceito
    CHECK(!overlay.is_in_exclusion_zone(50.0f, 215.0f));
    CHECK(overlay.on_touch_down(15, 50.0f, 215.0f));
    CHECK(overlay.active_touch_count() == 1);
    CHECK(overlay.on_touch_up(15, 50.0f, 215.0f));
    CHECK(overlay.active_touch_count() == 0);
}

static void test_touch_cancellation_and_release() {
    using namespace bb;
    InputManager input;
    TouchControlsOverlay overlay(&input);

    const auto square_bounds = overlay.get_control_bounds(VirtualControlId::ButtonSquare);

    // Pressiona Square ([ ])
    CHECK(overlay.on_touch_down(20, square_bounds.center_x, square_bounds.center_y));
    CHECK(overlay.is_control_pressed(VirtualControlId::ButtonSquare));
    CHECK((input.poll().buttons & PAD_SQUARE) != 0);

    // Notificação de cancelamento pelo sistema iOS (UITouchPhaseCancelled)
    CHECK(overlay.on_touch_cancel(20));
    CHECK(!overlay.is_control_pressed(VirtualControlId::ButtonSquare));
    CHECK((input.poll().buttons & PAD_SQUARE) == 0);
    CHECK(overlay.active_touch_count() == 0);

    // Ativa múltiplos controles simultaneamente
    const auto l1_bounds = overlay.get_control_bounds(VirtualControlId::ButtonL1);
    const auto r3_bounds = overlay.get_control_bounds(VirtualControlId::ButtonR3);
    const auto left_bounds = overlay.get_left_stick_bounds();

    CHECK(overlay.on_touch_down(21, l1_bounds.center_x, l1_bounds.center_y));
    CHECK(overlay.on_touch_down(22, r3_bounds.center_x, r3_bounds.center_y));
    CHECK(overlay.on_touch_down(23, left_bounds.center_x + 40.0f, left_bounds.center_y));
    CHECK(overlay.active_touch_count() == 3);

    auto state = input.poll();
    CHECK((state.buttons & PAD_L1) != 0);
    CHECK((state.buttons & PAD_R3) != 0);
    CHECK(state.left_stick_x > 0.5f);

    // Cancelamento global
    overlay.cancel_all_touches();
    CHECK(overlay.active_touch_count() == 0);

    state = input.poll();
    CHECK(state.buttons == 0);
    CHECK(std::abs(state.left_stick_x) < 0.001f);
}

static void test_haptics_conversion() {
    using namespace bb;

    // 1. Motor inativo
    auto ev_none = HapticEvent::translate_vibration(0, 0);
    CHECK(ev_none.type == HapticType::None);
    CHECK(ev_none.intensity == 0.0f);
    CHECK(ev_none.sharpness == 0.0f);
    CHECK(!ev_none.is_active());

    // 2. Clique sutil / UI Selection (pequeno motor leve)
    auto ev_sel = HapticEvent::translate_vibration(0, 30);
    CHECK(ev_sel.type == HapticType::Selection);
    CHECK(ev_sel.intensity > 0.05f && ev_sel.intensity < 0.20f);
    CHECK(ev_sel.sharpness > 0.7f);
    CHECK(ev_sel.is_active());

    // 3. Feedback leve (pequeno motor intermediário)
    auto ev_light = HapticEvent::translate_vibration(0, 100);
    CHECK(ev_light.type == HapticType::Light);
    CHECK(ev_light.intensity > 0.30f && ev_light.intensity < 0.50f);
    CHECK(ev_light.sharpness > 0.7f);

    // 4. Feedback médio (grande motor intermediário)
    auto ev_med = HapticEvent::translate_vibration(120, 0);
    CHECK(ev_med.type == HapticType::Medium);
    CHECK(ev_med.intensity > 0.40f && ev_med.intensity < 0.60f);
    CHECK(ev_med.sharpness < 0.40f);

    // 5. Impacto pesado / ataque visceral (grande motor alto)
    auto ev_heavy = HapticEvent::translate_vibration(220, 0);
    CHECK(ev_heavy.type == HapticType::Heavy);
    CHECK(ev_heavy.intensity > 0.80f);
    CHECK(ev_heavy.sharpness < 0.40f);

    // 6. Impacto composto simultâneo (CoreHaptics: tiro + impacto pesado)
    auto ev_impact = HapticEvent::translate_vibration(220, 180);
    CHECK(ev_impact.type == HapticType::Impact);
    CHECK(ev_impact.intensity > 0.80f);
    CHECK(ev_impact.sharpness > 0.65f);

    // 7. Callback e fila de eventos em TouchControlsOverlay
    TouchControlsOverlay overlay;
    HapticEvent last_cb_event{};
    overlay.set_haptic_callback([&](const HapticEvent& ev) {
        last_cb_event = ev;
    });

    overlay.set_vibration(190, 0);
    CHECK(last_cb_event.type == HapticType::Heavy);
    CHECK(overlay.get_current_haptic().type == HapticType::Heavy);

    auto polled = overlay.poll_haptic_events();
    CHECK(polled.size() == 1);
    CHECK(polled[0].type == HapticType::Heavy);

    // 8. Integração direta com PadHleManager
    PadHleManager pad_hle;
    const std::int32_t handle = pad_hle.scePadOpen(SCE_USER_SERVICE_INITIAL_USER_ID, 0, 0);
    CHECK(handle > 0);

    const ScePadVibrationParam vib{240, 200};
    CHECK(pad_hle.scePadSetVibration(handle, &vib) == 0);

    overlay.update_haptics_from_hle(pad_hle, handle);
    CHECK(overlay.get_current_haptic().type == HapticType::Impact);
}

static void test_all_buttons_and_triggers() {
    using namespace bb;
    InputManager input;
    TouchControlsOverlay overlay(&input);

    struct ButtonMapping {
        VirtualControlId id;
        std::uint32_t expected_mask;
    };

    const ButtonMapping buttons[] = {
        {VirtualControlId::DpadUp,         PAD_UP},
        {VirtualControlId::DpadDown,       PAD_DOWN},
        {VirtualControlId::DpadLeft,       PAD_LEFT},
        {VirtualControlId::DpadRight,      PAD_RIGHT},
        {VirtualControlId::ButtonTriangle, PAD_TRIANGLE},
        {VirtualControlId::ButtonCircle,   PAD_CIRCLE},
        {VirtualControlId::ButtonCross,    PAD_CROSS},
        {VirtualControlId::ButtonSquare,   PAD_SQUARE},
        {VirtualControlId::ButtonL1,       PAD_L1},
        {VirtualControlId::ButtonR1,       PAD_R1},
        {VirtualControlId::ButtonOptions,  PAD_OPTIONS},
        {VirtualControlId::ButtonR3,       PAD_R3},
        {VirtualControlId::ButtonL3,       PAD_L3}
    };

    std::uint64_t touch_id = 30;
    for (const auto& item : buttons) {
        const auto bounds = overlay.get_control_bounds(item.id);
        CHECK(bounds.radius > 0.0f);

        CHECK(overlay.on_touch_down(touch_id, bounds.center_x, bounds.center_y));
        CHECK(overlay.is_control_pressed(item.id));
        CHECK((input.poll().buttons & item.expected_mask) != 0);

        CHECK(overlay.on_touch_up(touch_id, bounds.center_x, bounds.center_y));
        CHECK(!overlay.is_control_pressed(item.id));
        CHECK((input.poll().buttons & item.expected_mask) == 0);
        ++touch_id;
    }

    // Gatilhos analógicos L2 e R2
    const auto l2_bounds = overlay.get_control_bounds(VirtualControlId::ButtonL2);
    CHECK(overlay.on_touch_down(80, l2_bounds.center_x, l2_bounds.center_y));
    auto state_l2 = input.poll();
    CHECK((state_l2.buttons & PAD_L2) != 0);
    CHECK(state_l2.l2_analog > 0.99f);

    CHECK(overlay.on_touch_up(80, l2_bounds.center_x, l2_bounds.center_y));
    state_l2 = input.poll();
    CHECK((state_l2.buttons & PAD_L2) == 0);
    CHECK(state_l2.l2_analog < 0.01f);

    const auto r2_bounds = overlay.get_control_bounds(VirtualControlId::ButtonR2);
    CHECK(overlay.on_touch_down(81, r2_bounds.center_x, r2_bounds.center_y));
    auto state_r2 = input.poll();
    CHECK((state_r2.buttons & PAD_R2) != 0);
    CHECK(state_r2.r2_analog > 0.99f);

    CHECK(overlay.on_touch_up(81, r2_bounds.center_x, r2_bounds.center_y));
    state_r2 = input.poll();
    CHECK((state_r2.buttons & PAD_R2) == 0);
    CHECK(state_r2.r2_analog < 0.01f);
}

static void test_camera_sensitivity_and_deadzone() {
    using namespace bb;
    InputManager input;
    TouchControlsOverlay overlay(&input);

    const auto right_bounds = overlay.get_right_stick_bounds();

    // 1. Zona morta radial
    overlay.set_stick_deadzone(0.25f);
    CHECK(overlay.get_stick_deadzone() == 0.25f);

    // Deslocamento de 5 pontos (abaixo do deadzone de 25% de 50pt = 12.5pt)
    CHECK(overlay.on_touch_down(90, right_bounds.center_x + 5.0f, right_bounds.center_y));
    auto state_dz = input.poll();
    CHECK(std::abs(state_dz.right_stick_x) < 0.001f);
    CHECK(overlay.on_touch_up(90, right_bounds.center_x, right_bounds.center_y));

    // 2. Sensibilidade angular da câmera
    overlay.set_stick_deadzone(0.0f); // Desativa deadzone para medição direta de escala

    // Sensibilidade padrão = 1.0f
    overlay.set_camera_sensitivity(1.0f);
    CHECK(overlay.get_camera_sensitivity() == 1.0f);
    CHECK(overlay.on_touch_down(91, right_bounds.center_x + 25.0f, right_bounds.center_y)); // 50% de deflexão
    auto state_sens1 = input.poll();
    CHECK(std::abs(state_sens1.right_stick_x - 0.50f) < 0.05f);
    CHECK(overlay.on_touch_up(91, right_bounds.center_x, right_bounds.center_y));

    // Sensibilidade aumentada = 2.0f
    overlay.set_camera_sensitivity(2.0f);
    CHECK(overlay.get_camera_sensitivity() == 2.0f);
    CHECK(overlay.on_touch_down(92, right_bounds.center_x + 25.0f, right_bounds.center_y));
    auto state_sens2 = input.poll();
    CHECK(std::abs(state_sens2.right_stick_x - 1.0f) < 0.05f); // 0.5 * 2.0 = 1.0
    CHECK(overlay.on_touch_up(92, right_bounds.center_x, right_bounds.center_y));

    // Sensibilidade reduzida = 0.5f
    overlay.set_camera_sensitivity(0.5f);
    CHECK(overlay.on_touch_down(93, right_bounds.center_x + 50.0f, right_bounds.center_y)); // 100% de deflexão
    auto state_sens3 = input.poll();
    CHECK(std::abs(state_sens3.right_stick_x - 0.50f) < 0.05f); // 1.0 * 0.5 = 0.5
    CHECK(overlay.on_touch_up(93, right_bounds.center_x, right_bounds.center_y));
}

static void test_floating_stick_mode() {
    using namespace bb;
    InputManager input;
    TouchControlsOverlay overlay(&input);

    overlay.set_left_stick_mode(StickMode::Floating);
    CHECK(overlay.get_left_stick_mode() == StickMode::Floating);

    // Toque inicial ancorado dinamicamente em uma coordenada arbitrária na metade esquerda da tela
    const float anchor_x = 220.0f;
    const float anchor_y = 260.0f;

    CHECK(overlay.on_touch_down(100, anchor_x, anchor_y));
    auto state_initial = input.poll();
    CHECK(std::abs(state_initial.left_stick_x) < 0.001f);
    CHECK(std::abs(state_initial.left_stick_y) < 0.001f);

    // Mover o dedo para cima e para a esquerda relativo à nova âncora
    CHECK(overlay.on_touch_move(100, anchor_x - 40.0f, anchor_y - 40.0f));
    auto state_moved = input.poll();
    CHECK(state_moved.left_stick_x < -0.5f);
    CHECK(state_moved.left_stick_y < -0.5f);

    // Soltar toque
    CHECK(overlay.on_touch_up(100, anchor_x - 40.0f, anchor_y - 40.0f));
    auto state_released = input.poll();
    CHECK(std::abs(state_released.left_stick_x) < 0.001f);
    CHECK(std::abs(state_released.left_stick_y) < 0.001f);
}

static void test_max_simultaneous_touches() {
    using namespace bb;
    TouchControlsOverlay overlay;

    // Registra 10 toques simultâneos
    for (std::uint64_t i = 1; i <= MAX_SIMULTANEOUS_TOUCHES; ++i) {
        const float px = 100.0f + static_cast<float>(i) * 30.0f;
        const float py = 200.0f;
        CHECK(overlay.on_touch_down(i, px, py));
        CHECK(overlay.active_touch_count() == i);
    }

    CHECK(overlay.active_touch_count() == 10);

    // O 11º toque deve ser rejeitado
    CHECK(!overlay.on_touch_down(11, 400.0f, 200.0f));
    CHECK(overlay.active_touch_count() == 10);

    // Liberar um toque
    CHECK(overlay.on_touch_up(1, 130.0f, 200.0f));
    CHECK(overlay.active_touch_count() == 9);

    // Agora um novo toque deve ser aceito
    CHECK(overlay.on_touch_down(12, 400.0f, 200.0f));
    CHECK(overlay.active_touch_count() == 10);

    overlay.cancel_all_touches();
    CHECK(overlay.active_touch_count() == 0);
}

int main() {
    return run([] {
        test_multitouch_hunting_actions();
        test_out_of_bounds_and_exclusion_rejection();
        test_touch_cancellation_and_release();
        test_haptics_conversion();
        test_all_buttons_and_triggers();
        test_camera_sensitivity_and_deadzone();
        test_floating_stick_mode();
        test_max_simultaneous_touches();
    });
}
