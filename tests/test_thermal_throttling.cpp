#include "check.h"
#include "pacing_tracker.h"
#include <cmath>
#include <chrono>

int main() {
    return run([] {
        using namespace bb;

        PacingTracker tracker(60, 30.0);
        CHECK(std::abs(tracker.target_fps() - 30.0) < 0.001);
        CHECK(std::abs(tracker.target_frame_ms() - (1000.0 / 30.0)) < 0.001);

        // 1. Perfil Nominal / Fair: Alvo padrão de 30 FPS estáveis
        tracker.set_target_fps(30.0);
        for (int i = 0; i < 5; ++i) {
            tracker.record_frame_ms(33.33);
        }
        CHECK(tracker.is_on_target_30fps(1.0));

        // 2. Perfil Térmico Elevado (ThermalStateSerious): Redução de FPS para arrefecimento
        // Reduz alvo para 24.0 FPS (tempo de quadro = 41.666 ms)
        tracker.set_target_fps(24.0);
        CHECK(std::abs(tracker.target_fps() - 24.0) < 0.001);
        CHECK(std::abs(tracker.target_frame_ms() - (1000.0 / 24.0)) < 0.001);

        // 3. Perfil Térmico Crítico (ThermalStateCritical): Redução extrema para evitar shutdown
        tracker.set_target_fps(20.0);
        CHECK(std::abs(tracker.target_fps() - 20.0) < 0.001);
        CHECK(std::abs(tracker.target_frame_ms() - 50.0) < 0.001);

        // 4. Teste de sync_frame_rate sintético curto para validar bloqueio e temporização
        tracker.set_target_fps(100.0); // 10 ms por frame para teste rápido
        auto t_start = std::chrono::steady_clock::now();
        tracker.sync_frame_rate();
        tracker.sync_frame_rate();
        auto t_end = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
        CHECK(elapsed_ms >= 8.0); // Garante que houve espera respeitando o frame pacing

        // 5. Retorno ao estado nominal
        tracker.set_target_fps(30.0);
        CHECK(std::abs(tracker.target_fps() - 30.0) < 0.001);
        CHECK(std::abs(tracker.target_frame_ms() - (1000.0 / 30.0)) < 0.001);
    });
}
