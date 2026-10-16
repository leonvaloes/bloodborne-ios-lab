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
    bb::GameBootCoordinator coordinator;
    coordinator.attach_video_out(&video_out);

    if (!coordinator.initialize(config)) return 1;

    if (coordinator.runtime()) {
        coordinator.runtime()->attach_video_out(&video_out);
        coordinator.runtime()->pad_manager().set_default_input(nullptr);
    }

    std::printf("Fast forwarding 456,000,000 steps...\n");
    coordinator.step(456000000ULL);

    std::printf("Now stepping further (456M -> 480M)...\n");
    while (coordinator.diagnostics().total_steps < 480000000ULL) {
        coordinator.step(50000);
        auto d = coordinator.diagnostics();
        if (d.state == bb::BootState::error || d.state == bb::BootState::halted) {
            std::printf("FAULT DETECTED: total_steps=%llu rip=0x%llx err=%s\n",
                        (unsigned long long)d.total_steps,
                        (unsigned long long)d.current_rip,
                        d.last_error.c_str());
            break;
        }
    }

    auto final_diag = coordinator.diagnostics();
    std::printf("\n=== Execution Summary ===\n");
    std::printf("Total steps: %llu\n", (unsigned long long)final_diag.total_steps);
    std::printf("Boot state: %s\n", bb::boot_state_to_string(final_diag.state));
    std::printf("Current RIP: 0x%llx\n", (unsigned long long)final_diag.current_rip);
    std::printf("Active threads: %zu\n", coordinator.scheduler() ? coordinator.scheduler()->thread_count() : 0);
    return 0;
}
