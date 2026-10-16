#ifndef BB_HOST_BRIDGE_H
#define BB_HOST_BRIDGE_H

#include "game_boot_coordinator.h"
#include "touch_controls.h"
#include "audio_out.h"
#include "video_out.h"
#include "pad_hle.h"
#include "input.h"
#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <map>
#include <vector>

namespace bb {

// Configuração de inicialização do HostBridge
struct HostBridgeConfig {
    std::string documents_dir;          // Pasta Documents do app iOS (~Documents)
    std::string dvdroot_dir;            // Subdiretório dos pacotes BHD5/BDT (opcional)
    std::string save_dir;               // Subdiretório de saves (opcional)
    std::string title_id{"CUSA03173"};  // Title ID do jogo
    std::string save_dir_name{"SPRJ0001"};
    int32_t user_id{0};
    std::size_t save_quota_bytes{32 * 1024 * 1024};
    std::size_t guest_memory_size{5632ULL << 20};
    std::uint64_t module_init_budget{10000000};
    std::vector<std::string> essential_asset_paths;
    bool require_essential_assets{false};
    double target_fps{60.0};            // FPS alvo para update()
    bool enable_hle{true};
    bool use_scheduler{true};
};

// Diagnósticos agregados do HostBridge
struct HostBridgeDiagnostics {
    BootState boot_state{BootState::not_started};
    std::string boot_state_name{"not_started"};

    std::uint64_t total_steps{0};
    std::uint64_t total_import_calls{0};
    std::map<std::string, std::uint64_t> import_call_counts;
    std::string last_import_called;

    std::vector<std::string> active_modules;
    std::size_t modules_count{0};

    bool assets_valid{false};
    PackageInspectionReport package_report;

    bool save_initialized{false};
    std::string save_mount_point;
    std::size_t open_audio_ports{0};

    std::uint64_t entry_point{0};
    std::uint64_t tcb_address{0};
    std::uint64_t current_rip{0};
    std::string last_stop_reason;
    std::string last_error;

    // Estatísticas de vídeo
    std::uint64_t video_flip_count{0};
    bool video_has_new_frame{false};
    std::uint32_t video_current_buffer_index{0};
    std::uint32_t video_width{0};
    std::uint32_t video_height{0};

    // Estatísticas de áudio
    std::size_t audio_frames_drained{0};
    std::size_t audio_underflows{0};

    // Estatísticas de input
    std::size_t active_touches{0};

    // Sumário consolidado
    std::string summary() const;
};

// Interface desacoplada entre engine e host (iOS/macOS/Windows)
class HostBridge {
public:
    explicit HostBridge(const HostBridgeConfig& config = HostBridgeConfig{});
    ~HostBridge();

    // Inicializa subsistemas a partir da pasta de documentos do app iOS
    // Retorna true se inicialização completa (boot Phases 1-4) bem sucedida
    bool init(const std::string& documents_dir);

    // Inicializa com configuração completa (permite sobrescrever documentos_dir)
    bool init(const HostBridgeConfig& config);

    // Executa uma fatia de passos correspondente ao frame (dt em segundos)
    // Retorna número de passos executados
    std::uint64_t update(float dt);

    // Consulta buffer atual para exibição via Metal/display
    // Preenche out_frame com informações do frame; retorna true se frame válido
    bool render_frame(HostFrameBuffer& out_frame);
    bool render_frame();

    // Despacha toque para TouchControlsOverlay
    // Sobrecargas para int e uint64_t (compatibilidade iOS/Windows)
    bool on_touch(int id, float x, float y, TouchPhase phase);
    bool on_touch(std::uint64_t id, float x, float y, TouchPhase phase);

    // Consulta de estado
    bool is_running() const;
    bool is_initialized() const;
    BootState state() const;
    HostBridgeDiagnostics diagnostics() const;
    std::string last_error() const;

    // Controle de execução
    void reset();
    void shutdown();

    // Acessos aos subsistemas integrados (para casos avançados)
    GameBootCoordinator& boot_coordinator() { return boot_coordinator_; }
    const GameBootCoordinator& boot_coordinator() const { return boot_coordinator_; }

    TouchControlsOverlay& touch_controls() { return touch_controls_; }
    const TouchControlsOverlay& touch_controls() const { return touch_controls_; }

    AudioOutManager& audio_out() { return audio_out_; }
    const AudioOutManager& audio_out() const { return audio_out_; }

    VideoOutManager& video_out() { return video_out_; }
    const VideoOutManager& video_out() const { return video_out_; }

    PadHleManager& pad_hle() { return pad_hle_; }
    const PadHleManager& pad_hle() const { return pad_hle_; }

    InputManager& input() { return input_; }
    const InputManager& input() const { return input_; }

    // Configurações de performance
    void set_steps_per_second(double sps) { steps_per_second_ = sps; }
    double steps_per_second() const { return steps_per_second_; }

    // Callback opcional para eventos de frame (para integração com run loop do host)
    using FrameCallback = std::function<void(const HostFrameBuffer&)>;
    void set_frame_callback(FrameCallback cb) { frame_callback_ = std::move(cb); }

private:
    HostBridgeConfig config_;
    GameBootCoordinator boot_coordinator_;
    InputManager input_;  // Declarado antes de touch_controls_ e pad_hle_
    TouchControlsOverlay touch_controls_;
    AudioOutManager audio_out_;
    VideoOutManager video_out_;
    PadHleManager pad_hle_;

    // VideoOutSink interno para receber flips do VideoOut
    class InternalVideoSink : public VideoOutSink {
    public:
        HostBridge* bridge = nullptr;
        explicit InternalVideoSink(HostBridge* b = nullptr) : bridge(b) {}
        void on_flip(const FlipEvent& event) override;
    } video_sink_{this};

    double steps_per_second_{30000000.0}; // ~30M passos/s (ajustável)
    std::uint64_t frame_count_{0};
    std::uint64_t total_steps_{0};
    bool initialized_{false};
    FrameCallback frame_callback_;

    // Helpers internos
    void update_audio(float dt);
    void update_video();
    void update_haptics();
    std::uint64_t calculate_steps_for_dt(float dt) const;
};

} // namespace bb

#endif // BB_HOST_BRIDGE_H
