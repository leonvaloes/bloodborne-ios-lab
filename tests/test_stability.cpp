#include "check.h"
#include "stability.h"
#include <vector>
#include <cmath>

int main() { return run([] {
    using namespace bb;

    // Teste de cálculo exato de percentis
    std::vector<double> linear_samples;
    for (int i = 0; i <= 100; ++i) {
        linear_samples.push_back(static_cast<double>(i));
    }
    double p50 = FramePacingTracker::calculate_percentile(linear_samples, 50.0);
    double p95 = FramePacingTracker::calculate_percentile(linear_samples, 95.0);
    double p99 = FramePacingTracker::calculate_percentile(linear_samples, 99.0);
    CHECK(std::abs(p50 - 50.0) < 0.001);
    CHECK(std::abs(p95 - 95.0) < 0.001);
    CHECK(std::abs(p99 - 99.0) < 0.001);

    // Teste do FramePacingTracker
    FramePacingTracker tracker(60);
    CHECK(tracker.get_stats().total_frames == 0);

    // Simula 50 quadros a 33.33 ms (30 FPS) e 10 quadros com engasgo (jitter de 50 ms)
    for (int i = 0; i < 50; ++i) {
        tracker.record_frame_ms(33.33);
    }
    for (int i = 0; i < 10; ++i) {
        tracker.record_frame_ms(50.0);
    }

    auto stats = tracker.get_stats();
    CHECK(stats.total_frames == 60);
    CHECK(std::abs(stats.min_ms - 33.33) < 0.01);
    CHECK(std::abs(stats.max_ms - 50.0) < 0.01);
    CHECK(stats.p50_ms >= 33.0 && stats.p50_ms <= 34.0);
    CHECK(stats.p95_ms >= 49.0);
    CHECK(stats.p99_ms >= 49.0);
    CHECK(stats.average_fps > 20.0 && stats.average_fps < 32.0);

    // Teste de medição de tempo de frame em tempo real
    tracker.begin_frame();
    double elapsed = tracker.end_frame();
    CHECK(elapsed >= 0.0);
    CHECK(tracker.get_stats().total_frames == 61);

    // Teste do MemoryMonitor
    MemoryMonitor mem_mon;
    std::uint64_t footprint = mem_mon.query_physical_footprint_bytes();
    // No Windows e sistemas com suporte a OS counters, o footprint deve ser > 0
    CHECK(mem_mon.get_peak_footprint_bytes() >= footprint);

    // Teste de alívio sob aviso de pouca memória
    bool callback_executed = false;
    mem_mon.register_trim_callback([&callback_executed]() {
        callback_executed = true;
    });

    mem_mon.trigger_low_memory_warning();
    CHECK(callback_executed);

    // Orçamento de memória
    mem_mon.set_budget_bytes(1024); // Limite baixo para teste
    CHECK(mem_mon.get_budget_bytes() == 1024);
}); }
