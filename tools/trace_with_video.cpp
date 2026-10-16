#include "game_boot_coordinator.h"
#include "orbis_runtime.h"
#include "video_out.h"
#include "pad_hle.h"
#include <cstdio>
#include <filesystem>

int main() {
    bb::BootConfig config;
    config.base_dir = "build-Release/only_eboot";
    config.guest_memory_size = static_cast<std::size_t>(5632ULL << 20);
    config.module_init_budget = 1000000;
    config.use_scheduler = true;
    config.require_essential_assets = false;
    config.enable_hle = true;

    bb::VideoOutManager video_out;
    bb::PadHleManager pad_mgr;

    bb::GameBootCoordinator coordinator;
    coordinator.attach_video_out(&video_out);

    if (!coordinator.initialize(config)) return 1;

    if (coordinator.runtime()) {
        coordinator.runtime()->attach_video_out(&video_out);
        coordinator.runtime()->pad_manager().set_default_input(nullptr);
    }

    std::printf("Stepping 450M steps...\n");
    coordinator.step(450000000ULL);

    while (coordinator.diagnostics().total_steps < 460000000ULL) {
        coordinator.step(50000);
        auto d = coordinator.diagnostics();
        if (d.total_steps % 1000000 < 50000) {
            std::printf("Steps: %llu (rip=0x%llx, state=%s)\n",
                        (unsigned long long)d.total_steps,
                        (unsigned long long)d.current_rip,
                        d.state_name.c_str());
        }
        if (d.state == bb::BootState::error || d.state == bb::BootState::halted) {
            std::printf("STOPPED: total_steps=%llu rip=0x%llx err=%s\n",
                        (unsigned long long)d.total_steps,
                        (unsigned long long)d.current_rip,
                        d.last_error.c_str());
            break;
        }
    }
    return 0;
}
