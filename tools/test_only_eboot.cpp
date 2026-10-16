#include "game_boot_coordinator.h"
#include "orbis_runtime.h"
#include <cstdio>
#include <filesystem>

int main() {
    std::filesystem::create_directories("build-Release/only_eboot/sce_module");
    if (!std::filesystem::exists("build-Release/only_eboot/eboot.bin")) {
        std::filesystem::copy_file("../bloodborne_portPC/blodboorn 3171/Bloodborne™/Image0/eboot.bin",
                                  "build-Release/only_eboot/eboot.bin");
    }
    for (const auto& entry : std::filesystem::directory_iterator("../bloodborne_portPC/blodboorn 3171/Bloodborne™/Image0/sce_module")) {
        std::filesystem::copy_file(entry.path(),
                                  "build-Release/only_eboot/sce_module" / entry.path().filename(),
                                  std::filesystem::copy_options::overwrite_existing);
    }

    bb::BootConfig config;
    config.base_dir = "build-Release/only_eboot";
    config.guest_memory_size = static_cast<std::size_t>(5632ULL << 20);
    config.module_init_budget = 1000000;
    config.use_scheduler = true;
    config.require_essential_assets = false;
    config.enable_hle = true;

    bb::GameBootCoordinator coordinator;
    if (!coordinator.initialize(config)) {
        std::printf("Init failed: %s\n", coordinator.diagnostics().last_error.c_str());
        return 1;
    }

    std::printf("Fast forwarding to 450M...\n");
    coordinator.step(450000000ULL);
    std::printf("Reached %llu steps. State=%s\n",
                (unsigned long long)coordinator.diagnostics().total_steps,
                coordinator.diagnostics().state_name.c_str());

    for (int i = 0; i < 10; ++i) {
        coordinator.step(1000000ULL);
        auto d = coordinator.diagnostics();
        std::printf("Step %dM: total_steps=%llu rip=0x%llx state=%s\n",
                    451 + i, (unsigned long long)d.total_steps,
                    (unsigned long long)d.current_rip, d.state_name.c_str());
        if (d.state == bb::BootState::error || d.state == bb::BootState::halted) {
            std::printf("Stopped: %s\n", d.last_error.c_str());
            break;
        }
    }
    return 0;
}
