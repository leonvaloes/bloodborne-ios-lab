#include <filesystem>
#include "engine_loop.h"
#include <cstring>
#include <vector>

namespace bb {

namespace {
constexpr std::uint32_t SAVE_MAGIC = 0x56534242; // "BBSV"
struct SaveHeader {
    std::uint32_t magic{SAVE_MAGIC};
    std::uint32_t payload_size{0};
    std::uint32_t crc32{0};
    std::uint32_t version{1};
};
}

EngineCoordinator::EngineCoordinator(const EngineConfig &config)
    : config_(config),
      memory_(config.memory_pool_kb),
      pacing_(120) {}

EngineCoordinator::~EngineCoordinator() {
    shutdown();
}

bool EngineCoordinator::initialize() {
    if (initialized_) return true;

    // Monta sistemas de arquivos vitais da sandbox
    vfs_.mount("/app0", config_.app0_dir, true);
    vfs_.mount("/user/home/savedata", config_.savedata_dir, false);
    std::error_code sec;
    std::filesystem::path tmp_base = std::filesystem::temp_directory_path(sec);
    if (sec || tmp_base.empty()) tmp_base = "./tmp";
    vfs_.mount("/temp", tmp_base.string(), false);

    // Inicializa subsistema de áudio
    AudioConfig audio_cfg{};
    audio_.init(audio_cfg);

    // Inicializa despachante e chamadas de sistema HLE
    hle_ = HleManager::create_standard();

    // Registra callback de alívio de memória sob pressão
    mem_monitor_.register_trim_callback([this]() {
        // Alívio de emergência: descarte de buffers transitórios de áudio
        audio_.shutdown();
        AudioConfig cfg{};
        audio_.init(cfg);
    });

    initialized_ = true;
    return true;
}

void EngineCoordinator::shutdown() {
    if (!initialized_) return;
    audio_.shutdown();
    initialized_ = false;
}

EngineFrameResult EngineCoordinator::tick(double delta_time_ms) {
    if (!initialized_) {
        initialize();
    }

    pacing_.record_frame_ms(delta_time_ms);
    ++frame_count_;

    EngineFrameResult result{};
    result.frame_index = frame_count_;
    result.frame_time_ms = delta_time_ms;
    result.input_state = input_.poll();
    result.audio_stats = audio_.get_stats();
    result.pacing_stats = pacing_.get_stats();
    result.memory_footprint_bytes = mem_monitor_.query_physical_footprint_bytes();
    result.memory_warning = mem_monitor_.is_budget_exceeded();

    return result;
}

bool EngineCoordinator::save_game_state(const std::string &slot_name, const void *state_data, std::size_t size) {
    if (!state_data || size == 0) return false;

    SaveHeader header{};
    header.payload_size = static_cast<std::uint32_t>(size);
    header.crc32 = VirtualFileSystem::compute_crc32(state_data, size);

    std::vector<std::uint8_t> buffer(sizeof(SaveHeader) + size);
    std::memcpy(buffer.data(), &header, sizeof(SaveHeader));
    std::memcpy(buffer.data() + sizeof(SaveHeader), state_data, size);

    std::string path = "/user/home/savedata/" + slot_name + ".sav";
    VfsError err = vfs_.write_file(path, buffer.data(), buffer.size(), true);
    return err == VfsError::ok;
}

bool EngineCoordinator::load_game_state(const std::string &slot_name, std::vector<std::uint8_t> &out_state) {
    std::string path = "/user/home/savedata/" + slot_name + ".sav";
    std::vector<std::uint8_t> buffer;
    VfsError err = vfs_.read_file(path, buffer);
    if (err != VfsError::ok || buffer.size() < sizeof(SaveHeader)) {
        return false;
    }

    SaveHeader header{};
    std::memcpy(&header, buffer.data(), sizeof(SaveHeader));
    if (header.magic != SAVE_MAGIC || header.payload_size != buffer.size() - sizeof(SaveHeader)) {
        return false;
    }

    const void *payload = buffer.data() + sizeof(SaveHeader);
    std::uint32_t computed_crc = VirtualFileSystem::compute_crc32(payload, header.payload_size);
    if (computed_crc != header.crc32) {
        return false; // Integridade corrompida!
    }

    out_state.resize(header.payload_size);
    std::memcpy(out_state.data(), payload, header.payload_size);
    return true;
}

} // namespace bb
