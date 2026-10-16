#include "game_boot_coordinator.h"
#include "orbis_runtime.h"
#include <cstdio>

int main() {
    bb::BootConfig config;
    config.base_dir = "../bloodborne_portPC/blodboorn 3171/Bloodborne™/Image0";
    config.guest_memory_size = static_cast<std::size_t>(5632ULL << 20);
    config.module_init_budget = 1000000;
    config.use_scheduler = true;
    config.require_essential_assets = false;
    config.enable_hle = true;

    bb::GameBootCoordinator coordinator;
    if (!coordinator.initialize(config)) {
        std::printf("Init failed\n");
        return 1;
    }

    std::printf("Fast forwarding to 425M...\n");
    coordinator.step(425000000ULL);
    std::printf("Reached %llu steps.\n", (unsigned long long)coordinator.diagnostics().total_steps);

    for (int i = 0; i < 40; ++i) {
        coordinator.step(1000000ULL);
        auto d = coordinator.diagnostics();
        std::printf("Step %dM: total_steps=%llu rip=0x%llx state=%s\n",
                    426 + i, (unsigned long long)d.total_steps,
                    (unsigned long long)d.current_rip, d.state_name.c_str());
        if (d.state == bb::BootState::error || d.state == bb::BootState::halted) {
            std::printf("Stopped: %s\n", d.last_error.c_str());
            break;
        }
    }
    return 0;
}
