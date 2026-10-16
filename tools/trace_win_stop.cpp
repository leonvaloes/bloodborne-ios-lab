#include "game_boot_coordinator.h"
#include "orbis_runtime.h"
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

    bb::GameBootCoordinator coordinator;
    if (!coordinator.initialize(config)) return 1;

    // Fast-forward to 452,000,000 steps
    coordinator.step(452000000ULL);

    // Now step 100 steps at a time to catch right before crash
    while (coordinator.diagnostics().total_steps < 452200000ULL) {
        coordinator.step(500);
        auto d = coordinator.diagnostics();
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
