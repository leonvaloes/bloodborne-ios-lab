#ifndef BB_GAME_BOOT_COORDINATOR_H
#define BB_GAME_BOOT_COORDINATOR_H

#include "vfs.h"
#include "game_package.h"
#include "save_data.h"
#include "audio_out.h"
#include "ps4_loader.h"
#include "ps4_bootstrap.h"
#include "orbis_runtime.h"
#include "x86_interp.h"
#include "guest_memory.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace bb {

enum class BootState {
    not_started = 0,
    packages_mounted,
    modules_initialized,
    running,
    halted,
    error
};

const char* boot_state_to_string(BootState state);

struct BootConfig {
    // Diretório base da aplicação (ex: Image0 ou Documents do iOS)
    std::string base_dir;

    // Subdiretório ou caminho de pacotes BHD5/BDT (se vazio, usa base_dir / "dvdroot_ps4")
    std::string dvdroot_dir;

    // Diretório persistente de saves (se vazio, usa base_dir / "savedata")
    std::string save_dir;

    // Identificador de título do jogo (padrão: "CUSA03173")
    std::string title_id{"CUSA03173"};

    // Nome do diretório do slot padrão de save (padrão: "SPRJ0001")
    std::string save_dir_name{"SPRJ0001"};

    // ID do usuário para o slot de save (padrão: 0)
    int32_t user_id{0};

    // Quota máxima por slot de save data (padrão: 32 MiB)
    std::size_t save_quota_bytes{32 * 1024 * 1024};

    // Orçamento de memória virtual do guest (padrão: 768 MiB)
    std::size_t guest_memory_size{768u << 20};

    // Limite de passos para inicializadores de módulo em Fase 4
    std::uint64_t module_init_budget{10000000};

    // Lista de caminhos de ativos essenciais (se vazia, usa default_essential_paths())
    std::vector<std::string> essential_asset_paths;

    // Se true, falha em Fase 1 se faltarem ativos essenciais
    bool require_essential_assets{true};

    // Pontos de montagem no VFS para os pacotes BHD5/BDT
    std::vector<std::string> package_mount_points{"/dvdroot", "/app0/dvdroot_ps4"};

    // Habilita resolução HLE de imports durante execução
    bool enable_hle{true};
};

struct BootDiagnostics {
    BootState state{BootState::not_started};
    std::string state_name{"not_started"};

    // Passos executados e contadores de import
    std::uint64_t total_steps{0};
    std::uint64_t total_import_calls{0};
    std::map<std::string, std::uint64_t> import_call_counts;
    std::string last_import_called;

    // Módulos ativos
    std::vector<std::string> active_modules;
    std::size_t modules_count{0};

    // Status de assets e pacotes
    bool assets_valid{false};
    PackageInspectionReport package_report;

    // Status de saves e I/O
    bool save_initialized{false};
    std::string save_mount_point;
    std::size_t open_audio_ports{0};

    // CPU, registradores e endereços de boot
    std::uint64_t entry_point{0};
    std::uint64_t tcb_address{0};
    std::uint64_t current_rip{0};
    std::string last_stop_reason;
    std::string last_error;

    // Sumário consolidado estruturado
    std::string summary() const;
};

class GameBootCoordinator {
public:
    GameBootCoordinator();
    ~GameBootCoordinator();

    // Limpa subsistemas e redefine status para not_started
    void reset();

    // Sequência unificada completa de inicialização (Fases 1 a 4)
    bool initialize(const BootConfig& config);

    // Fases granulares de inicialização
    // Fase 1: Varre e monta pacotes de dados, verifica integridade de assets essenciais
    bool phase1_mount_packages(const BootConfig& config);

    // Fase 2: Inicializa e monta slot padrão de save data em /savedata0
    bool phase2_setup_saves(const BootConfig& config);

    // Fase 3: Lê eboot.bin e módulos sce_module/*.prx, mapeia na memória guest e prepara TCB/TLS
    bool phase3_load_modules(const BootConfig& config);

    // Fase 4: Inicializa OrbisRuntime, procparam, traps e inicializadores de módulos na ordem de dependência
    bool phase4_initialize_runtime(const BootConfig& config);

    // Controle de execução unificado
    // Executa um slice de instruções no interpretador x86-64 e despacha traps HLE
    std::uint64_t step(std::uint64_t max_steps);

    // Consulta de estado e diagnóstico
    BootState state() const;
    BootDiagnostics diagnostics() const;

    // Acessos aos subsistemas integrados
    VirtualFileSystem& vfs();
    const VirtualFileSystem& vfs() const;

    GamePackageManager& package_manager();
    const GamePackageManager& package_manager() const;

    SaveDataManager& save_manager();
    const SaveDataManager& save_manager() const;

    AudioOutManager& audio_out();

    void attach_video_out(VideoOutManager* video_out) {
        video_out_ = video_out;
        if (runtime_) runtime_->attach_video_out(video_out_);
    }

    GuestMemory* guest_memory();
    const GuestMemory* guest_memory() const;

    X86Interpreter* cpu();
    const X86Interpreter* cpu() const;

    OrbisRuntime* runtime();
    const OrbisRuntime* runtime() const;

    const Ps4Image& image() const;
    const BootConfig& config() const;
    const PackageInspectionReport& package_report() const;
    const std::string& last_error() const;

private:
    BootState state_{BootState::not_started};
    BootConfig config_;

    std::unique_ptr<VirtualFileSystem> vfs_;
    GamePackageManager package_mgr_;
    PackageInspectionReport package_report_;

    bool save_initialized_{false};
    std::string save_mount_point_;

    std::unique_ptr<GuestMemory> guest_memory_;
    Ps4Image image_;
    std::unique_ptr<X86Interpreter> cpu_;
    std::unique_ptr<OrbisRuntime> runtime_;
    VideoOutManager* video_out_{nullptr};
    std::uint64_t tcb_address_{0};

    std::uint64_t total_steps_{0};
    std::uint64_t total_import_calls_{0};
    std::map<std::string, std::uint64_t> import_call_counts_;
    std::string last_import_called_;
    std::string last_stop_reason_;
    std::string error_message_;
};

} // namespace bb

#endif // BB_GAME_BOOT_COORDINATOR_H
