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

        // =====================================================================
        // 6. T182 / US5/AC1: Limitação de Orçamento e Reagendamento da CPU Guest
        // =====================================================================
        auto cfg_nom = get_thermal_throttling_config(ThermalThrottlingLevel::Nominal);
        CHECK(std::abs(cfg_nom.target_fps - 30.0) < 0.001);
        CHECK(cfg_nom.cpu_step_budget == 50000);
        CHECK(cfg_nom.reschedule_delay_ms == 0);

        auto cfg_fair = get_thermal_throttling_config(ThermalThrottlingLevel::Fair);
        CHECK(std::abs(cfg_fair.target_fps - 30.0) < 0.001);
        CHECK(cfg_fair.cpu_step_budget == 40000);
        CHECK(cfg_fair.reschedule_delay_ms == 0);

        auto cfg_ser = get_thermal_throttling_config(ThermalThrottlingLevel::Serious);
        CHECK(std::abs(cfg_ser.target_fps - 24.0) < 0.001);
        CHECK(cfg_ser.cpu_step_budget == 25000);
        CHECK(cfg_ser.reschedule_delay_ms == 5);

        auto cfg_crit = get_thermal_throttling_config(ThermalThrottlingLevel::Critical);
        CHECK(std::abs(cfg_crit.target_fps - 20.0) < 0.001);
        CHECK(cfg_crit.cpu_step_budget == 15000);
        CHECK(cfg_crit.reschedule_delay_ms == 15);

        // Redução monótona estrita do orçamento de passos guest conforme elevação térmica
        CHECK(cfg_nom.cpu_step_budget > cfg_fair.cpu_step_budget);
        CHECK(cfg_fair.cpu_step_budget > cfg_ser.cpu_step_budget);
        CHECK(cfg_ser.cpu_step_budget > cfg_crit.cpu_step_budget);

        // Incremento do atraso de reagendamento para arrefecimento
        CHECK(cfg_nom.reschedule_delay_ms <= cfg_fair.reschedule_delay_ms);
        CHECK(cfg_ser.reschedule_delay_ms > cfg_fair.reschedule_delay_ms);
        CHECK(cfg_crit.reschedule_delay_ms > cfg_ser.reschedule_delay_ms);

        // Simulação de transição de estado térmico (Nominal -> Serious -> Critical -> Recuperação)
        ThermalThrottlingLevel levels[] = {
            ThermalThrottlingLevel::Nominal,
            ThermalThrottlingLevel::Serious,
            ThermalThrottlingLevel::Critical,
            ThermalThrottlingLevel::Serious,
            ThermalThrottlingLevel::Nominal
        };
        for (auto lvl : levels) {
            auto c = get_thermal_throttling_config(lvl);
            tracker.set_target_fps(c.target_fps);
            CHECK(tracker.target_fps() >= 20.0 && tracker.target_fps() <= 30.0);
            CHECK(c.cpu_step_budget >= 15000 && c.cpu_step_budget <= 50000);
        }
        CHECK(std::abs(tracker.target_fps() - 30.0) < 0.001);
    });
}
