#include "check.h"
#include "touch_controls.h"
#include "input.h"
#include "pad_hle.h"
#include "user_service.h"
#include <chrono>
#include <vector>
#include <cmath>

int main() {
    return run([] {
        using namespace bb;

        UserServiceManager user_service;
        CHECK(user_service.sceUserServiceInitialize(nullptr) == 0);

        InputManager input;
        TouchControlsOverlay overlay(&input);

        PadHleManager pad_mgr(&input, &user_service);
        std::int32_t handle = pad_mgr.scePadOpen(SCE_USER_SERVICE_INITIAL_USER_ID, SCE_PAD_PORT_TYPE_STANDARD, 0);
        CHECK(handle > 0);

        // 1. Medição de latência para comandos virtuais de toque na tela
        // Deve ser muito inferior ao teto de 50 ms exigido pela especificação
        constexpr double MAX_ACCEPTABLE_LATENCY_MS = 50.0;

        const auto left_bounds = overlay.get_left_stick_bounds();
        const auto r1_bounds = overlay.get_control_bounds(VirtualControlId::ButtonR1);
        const auto circle_bounds = overlay.get_control_bounds(VirtualControlId::ButtonCircle);
        const auto l2_bounds = overlay.get_control_bounds(VirtualControlId::ButtonL2);
        const auto r3_bounds = overlay.get_control_bounds(VirtualControlId::ButtonR3);

        // A. Movimento do Caçador (Analógico Esquerdo para a frente)
        auto t0 = std::chrono::high_resolution_clock::now();
        CHECK(overlay.on_touch_down(1, left_bounds.center_x, left_bounds.center_y - 45.0f));
        ScePadData pad_data{};
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        auto t1 = std::chrono::high_resolution_clock::now();
        double walk_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(walk_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK(pad_data.left_stick_y < 100); // 128 é neutro, < 100 indica empurrão para cima

        // B. Ataque R1
        t0 = std::chrono::high_resolution_clock::now();
        CHECK(overlay.on_touch_down(2, r1_bounds.center_x, r1_bounds.center_y));
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double r1_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(r1_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_R1) != 0);

        // C. Esquiva Círculo (O)
        CHECK(overlay.on_touch_up(2, r1_bounds.center_x, r1_bounds.center_y));
        t0 = std::chrono::high_resolution_clock::now();
        CHECK(overlay.on_touch_down(3, circle_bounds.center_x, circle_bounds.center_y));
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double dodge_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(dodge_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_CIRCLE) != 0);

        // D. Disparo com Arma de Fogo L2
        CHECK(overlay.on_touch_up(3, circle_bounds.center_x, circle_bounds.center_y));
        t0 = std::chrono::high_resolution_clock::now();
        CHECK(overlay.on_touch_down(4, l2_bounds.center_x, l2_bounds.center_y));
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double shoot_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(shoot_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_L2) != 0);

        // E. Travar Mira (Lock-on R3)
        t0 = std::chrono::high_resolution_clock::now();
        CHECK(overlay.on_touch_down(5, r3_bounds.center_x, r3_bounds.center_y));
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double r3_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(r3_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_R3) != 0);

        // F. Ataque Pesado R2 (SC-005)
        CHECK(overlay.on_touch_up(5, r3_bounds.center_x, r3_bounds.center_y));
        const auto r2_bounds = overlay.get_control_bounds(VirtualControlId::ButtonR2);
        t0 = std::chrono::high_resolution_clock::now();
        CHECK(overlay.on_touch_down(6, r2_bounds.center_x, r2_bounds.center_y));
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double r2_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(r2_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_R2) != 0);

        // G. Uso de Item Rápido Quadrado (SC-005)
        CHECK(overlay.on_touch_up(6, r2_bounds.center_x, r2_bounds.center_y));
        const auto square_bounds = overlay.get_control_bounds(VirtualControlId::ButtonSquare);
        t0 = std::chrono::high_resolution_clock::now();
        CHECK(overlay.on_touch_down(7, square_bounds.center_x, square_bounds.center_y));
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double square_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(square_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_SQUARE) != 0);

        // H. Troca de Armas: Direita (D-Pad Right) (SC-005)
        CHECK(overlay.on_touch_up(7, square_bounds.center_x, square_bounds.center_y));
        const auto dpad_r_bounds = overlay.get_control_bounds(VirtualControlId::DpadRight);
        t0 = std::chrono::high_resolution_clock::now();
        CHECK(overlay.on_touch_down(8, dpad_r_bounds.center_x, dpad_r_bounds.center_y));
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double dpad_r_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(dpad_r_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_RIGHT) != 0);

        // I. Troca de Armas: Esquerda (D-Pad Left) (SC-005)
        CHECK(overlay.on_touch_up(8, dpad_r_bounds.center_x, dpad_r_bounds.center_y));
        const auto dpad_l_bounds = overlay.get_control_bounds(VirtualControlId::DpadLeft);
        t0 = std::chrono::high_resolution_clock::now();
        CHECK(overlay.on_touch_down(9, dpad_l_bounds.center_x, dpad_l_bounds.center_y));
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double dpad_l_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(dpad_l_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_LEFT) != 0);

        // J. Transformação de Arma Truque (L1) e Cura (Triângulo)
        CHECK(overlay.on_touch_up(9, dpad_l_bounds.center_x, dpad_l_bounds.center_y));
        const auto l1_bounds = overlay.get_control_bounds(VirtualControlId::ButtonL1);
        t0 = std::chrono::high_resolution_clock::now();
        CHECK(overlay.on_touch_down(10, l1_bounds.center_x, l1_bounds.center_y));
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double l1_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(l1_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_L1) != 0);

        CHECK(overlay.on_touch_up(10, l1_bounds.center_x, l1_bounds.center_y));
        const auto tri_bounds = overlay.get_control_bounds(VirtualControlId::ButtonTriangle);
        t0 = std::chrono::high_resolution_clock::now();
        CHECK(overlay.on_touch_down(11, tri_bounds.center_x, tri_bounds.center_y));
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double tri_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(tri_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_TRIANGLE) != 0);

        // K. Câmera: Analógico Direito (RightStick)
        CHECK(overlay.on_touch_up(11, tri_bounds.center_x, tri_bounds.center_y));
        const auto right_bounds = overlay.get_right_stick_bounds();
        t0 = std::chrono::high_resolution_clock::now();
        CHECK(overlay.on_touch_down(12, right_bounds.center_x + 35.0f, right_bounds.center_y));
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double camera_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(camera_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK(pad_data.right_stick_x > 140); // 128 neutro, > 140 indica movimento à direita

        // Limpeza dos toques
        overlay.cancel_all_touches();
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        CHECK(pad_data.buttons == 0);

        // 2. Medição de latência para Gamepad Físico direto no InputManager (SC-005: 100% ações essenciais)
        // A. Ataque Leve R1
        t0 = std::chrono::high_resolution_clock::now();
        input.set_button(PAD_R1, true);
        input.set_left_stick(0.0f, -1.0f);
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double gamepad_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(gamepad_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_R1) != 0);
        CHECK(pad_data.left_stick_y < 50);

        // B. Ataque Pesado R2
        input.set_button(PAD_R1, false);
        t0 = std::chrono::high_resolution_clock::now();
        input.set_button(PAD_R2, true);
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double gp_r2_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(gp_r2_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_R2) != 0);

        // C. Esquiva Círculo
        input.set_button(PAD_R2, false);
        t0 = std::chrono::high_resolution_clock::now();
        input.set_button(PAD_CIRCLE, true);
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double gp_circle_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(gp_circle_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_CIRCLE) != 0);

        // D. Disparo L2
        input.set_button(PAD_CIRCLE, false);
        t0 = std::chrono::high_resolution_clock::now();
        input.set_button(PAD_L2, true);
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double gp_l2_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(gp_l2_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_L2) != 0);

        // E. Uso de Item Quadrado
        input.set_button(PAD_L2, false);
        t0 = std::chrono::high_resolution_clock::now();
        input.set_button(PAD_SQUARE, true);
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double gp_square_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(gp_square_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_SQUARE) != 0);

        // F. Troca de Armas: Direita (PAD_RIGHT) e Esquerda (PAD_LEFT)
        input.set_button(PAD_SQUARE, false);
        t0 = std::chrono::high_resolution_clock::now();
        input.set_button(PAD_RIGHT, true);
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double gp_dright_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(gp_dright_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_RIGHT) != 0);

        input.set_button(PAD_RIGHT, false);
        t0 = std::chrono::high_resolution_clock::now();
        input.set_button(PAD_LEFT, true);
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double gp_dleft_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(gp_dleft_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_LEFT) != 0);

        // G. Câmera analógica direita
        input.set_button(PAD_LEFT, false);
        t0 = std::chrono::high_resolution_clock::now();
        input.set_right_stick(1.0f, 0.0f);
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double gp_camera_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(gp_camera_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK(pad_data.right_stick_x > 200);

        CHECK(pad_mgr.scePadClose(handle) == 0);
        CHECK(user_service.sceUserServiceTerminate() == 0);
    });
}
