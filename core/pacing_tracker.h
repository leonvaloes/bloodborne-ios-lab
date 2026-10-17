#pragma once
#include "stability.h"
#include <chrono>
#include <thread>
#include <cmath>

namespace bb {

// Constante alvo de 30 FPS para o pacing de Bloodborne no iOS (33.3333 ms por quadro)
constexpr double TARGET_FRAME_TIME_MS_30FPS = 1000.0 / 30.0;

// Níveis de pressão térmica e parametrização de carga para CPU guest e FPS (T182 / US5/AC1)
enum class ThermalThrottlingLevel {
    Nominal,
    Fair,
    Serious,
    Critical
};

struct ThermalThrottlingConfig {
    double target_fps = 30.0;
    std::uint32_t cpu_step_budget = 50000;
    std::uint32_t reschedule_delay_ms = 0;
};

inline ThermalThrottlingConfig get_thermal_throttling_config(ThermalThrottlingLevel level) {
    switch (level) {
        case ThermalThrottlingLevel::Critical:
            return {20.0, 15000, 15};
        case ThermalThrottlingLevel::Serious:
            return {24.0, 25000, 5};
        case ThermalThrottlingLevel::Fair:
            return {30.0, 40000, 0};
        case ThermalThrottlingLevel::Nominal:
        default:
            return {30.0, 50000, 0};
    }
}

class PacingTracker : public FramePacingTracker {
public:
    explicit PacingTracker(std::size_t window_size = 120, double target_fps = 30.0)
        : FramePacingTracker(window_size), target_fps_(target_fps), target_frame_ms_(1000.0 / target_fps) {}

    void set_target_fps(double fps) {
        if (fps > 0.0) {
            target_fps_ = fps;
            target_frame_ms_ = 1000.0 / fps;
        }
    }

    double target_fps() const { return target_fps_; }
    double target_frame_ms() const { return target_frame_ms_; }

    // Bloqueia e sincroniza o tempo de frame para garantir a taxa alvo estável
    void sync_frame_rate() {
        auto now = std::chrono::steady_clock::now();
        if (has_last_frame_) {
            double elapsed_ms = std::chrono::duration<double, std::milli>(now - last_sync_point_).count();
            if (elapsed_ms < target_frame_ms_) {
                double sleep_ms = target_frame_ms_ - elapsed_ms;
                if (sleep_ms >= 1.0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<std::int64_t>((sleep_ms - 0.5) * 1000.0)));
                }
                while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - last_sync_point_).count() < target_frame_ms_) {
                    std::this_thread::yield();
                }
            }
        }
        auto post_sync = std::chrono::steady_clock::now();
        if (has_last_frame_) {
            double actual_frame_ms = std::chrono::duration<double, std::milli>(post_sync - last_sync_point_).count();
            record_frame_ms(actual_frame_ms);
        }
        last_sync_point_ = post_sync;
        has_last_frame_ = true;
    }

    bool is_on_target_30fps(double tolerance_ms = 4.0) const {
        auto stats = get_stats();
        if (stats.total_frames < 5) return true;
        return (std::abs(stats.p50_ms - TARGET_FRAME_TIME_MS_30FPS) <= tolerance_ms);
    }

private:
    double target_fps_{30.0};
    double target_frame_ms_{TARGET_FRAME_TIME_MS_30FPS};
    std::chrono::steady_clock::time_point last_sync_point_{};
    bool has_last_frame_{false};
};

} // namespace bb
