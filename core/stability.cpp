#include "stability.h"
#include <algorithm>
#include <numeric>
#include <cmath>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#include <fstream>
#endif

namespace bb {

FramePacingTracker::FramePacingTracker(std::size_t window_size)
    : window_size_(window_size > 0 ? window_size : 120),
      history_(window_size_ > 0 ? window_size_ : 120, 0.0) {}

void FramePacingTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    count_ = 0;
    total_frames_ = 0;
    std::fill(history_.begin(), history_.end(), 0.0);
}

void FramePacingTracker::begin_frame() {
    frame_start_ = std::chrono::steady_clock::now();
}

double FramePacingTracker::end_frame() {
    auto now = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - frame_start_).count();
    record_frame_ms(ms);
    return ms;
}

void FramePacingTracker::record_frame_ms(double duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    history_[head_] = duration_ms;
    head_ = (head_ + 1) % window_size_;
    if (count_ < window_size_) ++count_;
    ++total_frames_;
}

double FramePacingTracker::calculate_percentile(std::vector<double> samples, double percentile) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());

    double rank = (percentile / 100.0) * static_cast<double>(samples.size() - 1);
    std::size_t low = static_cast<std::size_t>(std::floor(rank));
    std::size_t high = static_cast<std::size_t>(std::ceil(rank));
    double frac = rank - static_cast<double>(low);

    if (low >= samples.size()) return samples.back();
    if (high >= samples.size()) return samples.back();
    if (low == high) return samples[low];

    return samples[low] + frac * (samples[high] - samples[low]);
}

FramePacingStats FramePacingTracker::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    FramePacingStats stats{};
    stats.total_frames = total_frames_;
    if (count_ == 0) return stats;

    std::vector<double> active_samples;
    active_samples.reserve(count_);
    double sum = 0.0;
    double min_v = history_[0];
    double max_v = history_[0];

    for (std::size_t i = 0; i < count_; ++i) {
        double v = history_[i];
        active_samples.push_back(v);
        sum += v;
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
    }

    stats.min_ms = min_v;
    stats.max_ms = max_v;
    stats.p50_ms = calculate_percentile(active_samples, 50.0);
    stats.p95_ms = calculate_percentile(active_samples, 95.0);
    stats.p99_ms = calculate_percentile(active_samples, 99.0);

    double avg_ms = sum / static_cast<double>(count_);
    if (avg_ms > 0.00001) {
        stats.average_fps = 1000.0 / avg_ms;
    }

    return stats;
}

MemoryMonitor::MemoryMonitor() = default;

std::uint64_t MemoryMonitor::query_physical_footprint_bytes() {
    std::uint64_t footprint = 0;

#if defined(__APPLE__)
    task_vm_info_data_t vm_info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm_info), &count) == KERN_SUCCESS) {
        footprint = static_cast<std::uint64_t>(vm_info.phys_footprint);
    } else {
        mach_task_basic_info info;
        mach_msg_type_number_t basic_count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &basic_count) == KERN_SUCCESS) {
            footprint = static_cast<std::uint64_t>(info.resident_size);
        }
    }
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc))) {
        footprint = static_cast<std::uint64_t>(pmc.WorkingSetSize);
    }
#elif defined(__linux__)
    long rss_pages = 0;
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        long dummy = 0;
        statm >> dummy >> rss_pages;
        long page_size = sysconf(_SC_PAGESIZE);
        footprint = static_cast<std::uint64_t>(rss_pages * page_size);
    }
#endif

    std::lock_guard<std::mutex> lock(mutex_);
    current_footprint_ = footprint;
    if (footprint > peak_footprint_) {
        peak_footprint_ = footprint;
    }

    if (budget_bytes_ > 0 && current_footprint_ > budget_bytes_) {
        // Dispara liberação de emergência se excedeu orçamento
        for (const auto &cb : trim_callbacks_) {
            if (cb) cb();
        }
    }

    return footprint;
}

std::uint64_t MemoryMonitor::get_peak_footprint_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peak_footprint_;
}

void MemoryMonitor::set_budget_bytes(std::uint64_t budget) {
    std::lock_guard<std::mutex> lock(mutex_);
    budget_bytes_ = budget;
}

std::uint64_t MemoryMonitor::get_budget_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return budget_bytes_;
}

bool MemoryMonitor::is_budget_exceeded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return budget_bytes_ > 0 && current_footprint_ > budget_bytes_;
}

void MemoryMonitor::register_trim_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    trim_callbacks_.push_back(std::move(cb));
}

void MemoryMonitor::trigger_low_memory_warning() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks = trim_callbacks_;
    }
    for (const auto &cb : callbacks) {
        if (cb) cb();
    }
}

} // namespace bb
