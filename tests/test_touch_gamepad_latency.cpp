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

        // Limpeza dos toques
        overlay.cancel_all_touches();
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        CHECK(pad_data.buttons == 0);

        // 2. Medição de latência para Gamepad Físico direto no InputManager
        t0 = std::chrono::high_resolution_clock::now();
        input.set_button(PAD_R1, true);
        input.set_left_stick(0.0f, -1.0f);
        CHECK(pad_mgr.scePadReadState(handle, &pad_data) == 0);
        t1 = std::chrono::high_resolution_clock::now();
        double gamepad_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CHECK(gamepad_latency_ms < MAX_ACCEPTABLE_LATENCY_MS);
        CHECK((pad_data.buttons & PAD_R1) != 0);
        CHECK(pad_data.left_stick_y < 50);

        CHECK(pad_mgr.scePadClose(handle) == 0);
        CHECK(user_service.sceUserServiceTerminate() == 0);
    });
}
