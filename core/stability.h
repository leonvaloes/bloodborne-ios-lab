#ifndef BB_STABILITY_H
#define BB_STABILITY_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <chrono>
#include <functional>
#include <mutex>

namespace bb {

struct FramePacingStats {
    double p50_ms{0.0};
    double p95_ms{0.0};
    double p99_ms{0.0};
    double average_fps{0.0};
    double min_ms{0.0};
    double max_ms{0.0};
    std::uint64_t total_frames{0};
};

class FramePacingTracker {
public:
    explicit FramePacingTracker(std::size_t window_size = 120);

    void record_frame_ms(double duration_ms);
    void begin_frame();
    double end_frame();

    FramePacingStats get_stats() const;
    void reset();

    // Utilitário estático para cálculo exato de percentis
    static double calculate_percentile(std::vector<double> samples, double percentile);

private:
    std::size_t window_size_;
    std::vector<double> history_;
    std::size_t head_{0};
    std::size_t count_{0};
    std::uint64_t total_frames_{0};
    std::chrono::steady_clock::time_point frame_start_{};
    mutable std::mutex mutex_;
};

class MemoryMonitor {
public:
    MemoryMonitor();

    // Consulta do consumo físico de memória (phys_footprint no iOS, WorkingSet no Windows)
    std::uint64_t query_physical_footprint_bytes();
    std::uint64_t get_peak_footprint_bytes() const;

    // Gerenciamento de pressão de memória e liberação de caches
    void set_budget_bytes(std::uint64_t budget);
    std::uint64_t get_budget_bytes() const;
    bool is_budget_exceeded() const;

    void register_trim_callback(std::function<void()> cb);
    void trigger_low_memory_warning();

private:
    std::uint64_t budget_bytes_{3ULL * 1024ULL * 1024ULL * 1024ULL}; // 3 GB padrão de segurança
    std::uint64_t current_footprint_{0};
    std::uint64_t peak_footprint_{0};
    std::vector<std::function<void()>> trim_callbacks_;
    mutable std::mutex mutex_;
};

} // namespace bb

#endif // BB_STABILITY_H
