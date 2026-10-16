// Diagnóstico local opt-in: não exporta arquivos, endereços absolutos nem trace bruto.
#include "game_boot_coordinator.h"
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <vector>
#include <string>
#include <exception>
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
bool number(std::string_view text, std::uint64_t& result) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
}

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> args;
#if defined(_WIN32)
    int wargc = 0;
    if (auto* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        for (int i = 0; i < wargc; ++i) args.emplace_back(wargv[i]);
        LocalFree(wargv);
    }
#endif
    if (args.empty()) for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    if (args.size() < 2 || args.size() > 5) {
        std::fprintf(stderr, "uso: bb-boot-diagnostic diretório [passos<=100000] [MiB<=6144] [init_passos<=1000000]\n");
        return 2;
    }
    std::uint64_t steps = 100000, mib = 5632, init = 1000000;
    if ((args.size() > 2 && !number(args[2].string(), steps)) ||
        (args.size() > 3 && !number(args[3].string(), mib)) ||
        (args.size() > 4 && !number(args[4].string(), init)) || !steps || steps > 100000 ||
        mib < 64 || mib > 6144 || !init || init > 1000000) return 2;

    bb::BootConfig config;
    config.base_dir = args[1].string();
    config.guest_memory_size = static_cast<std::size_t>(mib << 20);
    config.module_init_budget = init;
    config.use_scheduler = true;
    config.require_essential_assets = false;
    bb::GameBootCoordinator coordinator;
    bool initialized = false;
    try {
        initialized = coordinator.initialize(config);
        if (initialized) coordinator.step(steps);
    } catch (const std::exception&) {
        std::fprintf(stderr, "diagnóstico interrompido por exceção (detalhes omitidos)\n");
        return 1;
    }
    const auto d = coordinator.diagnostics();
    // Não imprimir último import, caminho, RIP nem mensagem de exceção (podem conter dados sensíveis).
    std::printf("initialized=%d state=%s steps=%llu imports=%llu modules=%zu mapped_mib=%zu peak_mib=%zu budget_mib=%zu\n",
        initialized ? 1 : 0, d.state_name.c_str(),
        static_cast<unsigned long long>(d.total_steps),
        static_cast<unsigned long long>(d.total_import_calls), d.modules_count,
        d.guest_mapped_bytes >> 20, d.guest_peak_mapped_bytes >> 20, d.guest_budget_bytes >> 20);
    return initialized ? (d.state == bb::BootState::error ? 1 : 0) : 1;
}
