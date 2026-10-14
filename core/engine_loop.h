#ifndef BB_ENGINE_LOOP_H
#define BB_ENGINE_LOOP_H

#include <cstdint>
#include <string>
#include <memory>
#include "guest_memory.h"
#include "dispatcher.h"
#include "hle.h"
#include "vfs.h"
#include "input.h"
#include "audio.h"
#include "stability.h"

namespace bb {

struct EngineConfig {
    std::string app0_dir{"."};
    std::string savedata_dir{"./savedata"};
    double target_fps{30.0};
    std::size_t memory_pool_kb{256 * 1024}; // 256 MB para lab
};

struct EngineFrameResult {
    std::uint64_t frame_index{0};
    double frame_time_ms{0.0};
    PadData input_state{};
    AudioStats audio_stats{};
    FramePacingStats pacing_stats{};
    std::uint64_t memory_footprint_bytes{0};
    bool memory_warning{false};
};

class EngineCoordinator {
public:
    explicit EngineCoordinator(const EngineConfig &config = EngineConfig{});
    ~EngineCoordinator();

    bool initialize();
    void shutdown();

    EngineFrameResult tick(double delta_time_ms = 33.33);

    // Gestão de saves com verificação de integridade CRC32
    bool save_game_state(const std::string &slot_name, const void *state_data, std::size_t size);
    bool load_game_state(const std::string &slot_name, std::vector<std::uint8_t> &out_state);

    GuestMemory &memory() { return memory_; }
    VirtualFileSystem &vfs() { return vfs_; }
    InputManager &input() { return input_; }
    AudioManager &audio() { return audio_; }
    FramePacingTracker &pacing() { return pacing_; }
    MemoryMonitor &memory_monitor() { return mem_monitor_; }
    IndirectDispatcher &dispatcher() { return dispatcher_; }
    HleManager &hle() { return hle_; }

    std::uint64_t frame_count() const { return frame_count_; }
    bool is_initialized() const { return initialized_; }

private:
    EngineConfig config_;
    GuestMemory memory_;
    VirtualFileSystem vfs_;
    InputManager input_;
    AudioManager audio_;
    FramePacingTracker pacing_;
    MemoryMonitor mem_monitor_;
    IndirectDispatcher dispatcher_;
    HleManager hle_;

    std::uint64_t frame_count_{0};
    bool initialized_{false};
};

} // namespace bb

#endif // BB_ENGINE_LOOP_H
