#include "check.h"
#include "stability.h"
#include "flver_parser.h"
#include "guest_memory.h"
#include "audio_out.h"
#include "gnm_metal_bridge.h"
#include <vector>
#include <cstdint>
#include <memory>

int main() {
    return run([] {
        using namespace bb;

        MemoryMonitor monitor;

        // Limite máximo de memória física residente (RSS) conforme especificação: 3.5 GB
        constexpr std::uint64_t RSS_LIMIT_3_5_GB = static_cast<std::uint64_t>(3.5 * 1024.0 * 1024.0 * 1024.0); // 3,758,096,384 bytes
        monitor.set_budget_bytes(RSS_LIMIT_3_5_GB);
        CHECK(monitor.get_budget_bytes() == RSS_LIMIT_3_5_GB);

        // Mede footprint inicial
        std::uint64_t initial_footprint = monitor.query_physical_footprint_bytes();
        CHECK(initial_footprint < RSS_LIMIT_3_5_GB);

        // Configuração de cache com mecanismo de trim (descarte sob pressão de memória)
        std::vector<std::vector<std::uint8_t>> area_cached_assets;
        bool cache_trimmed = false;

        monitor.register_trim_callback([&]() {
            area_cached_assets.clear();
            cache_trimmed = true;
        });

        // Simula carregamento de modelos 3D e buffers da área da Clínica de Iosefka
        constexpr std::size_t num_models = 50;
        auto cube_flver = FlverParser::create_synthetic_flver_cube();
        std::vector<MetalMeshBuffer> area_gpu_buffers;
        area_gpu_buffers.reserve(num_models);

        for (std::size_t i = 0; i < num_models; ++i) {
            FlverModel model{};
            std::string err;
            CHECK(FlverParser::parse(cube_flver.data(), cube_flver.size(), model, err));
            area_gpu_buffers.push_back(FlverParser::convert_to_metal_buffers(model));

            // Simula buffer de cache de textura da área (64 KB por asset)
            area_cached_assets.push_back(std::vector<std::uint8_t>(65536, static_cast<std::uint8_t>(i & 0xFF)));
        }

        CHECK(area_gpu_buffers.size() == num_models);
        CHECK(!area_cached_assets.empty());

        // Aloca 32 MB de memória guest mapeada para buffers de cena
        GuestMemory scene_memory(32 * 1024 * 1024);
        CHECK(scene_memory.map(0x10000, 31 * 1024 * 1024, read | write) == MemoryError::ok);

        // Verifica footprint após carga de cena
        std::uint64_t loaded_footprint = monitor.query_physical_footprint_bytes();
        CHECK(loaded_footprint < RSS_LIMIT_3_5_GB);
        CHECK(monitor.get_peak_footprint_bytes() < RSS_LIMIT_3_5_GB);
        CHECK(!monitor.is_budget_exceeded());

        // Dispara aviso de pressão de memória para validar o trim automático
        monitor.trigger_low_memory_warning();
        CHECK(cache_trimmed);
        CHECK(area_cached_assets.empty());

        // Após trim, o sistema continua estável dentro dos limites
        CHECK(!monitor.is_budget_exceeded());
        CHECK(monitor.get_peak_footprint_bytes() < RSS_LIMIT_3_5_GB);

        // 2. Seção T043 / FR-009: Orçamento de 5,5 GiB (iPhone 15 Pro com 8 GB) e purga coordenada de áudio e texturas
        {
            MemoryMonitor ios15pro_monitor;
            constexpr std::uint64_t BUDGET_5_5_GIB = 5632ULL << 20; // 5,905,580,032 bytes
            ios15pro_monitor.set_budget_bytes(BUDGET_5_5_GIB);
            CHECK(ios15pro_monitor.get_budget_bytes() == BUDGET_5_5_GIB);

            // Valida paginação sob demanda virtual em GuestMemory
            GuestMemory virtual_guest_mem(BUDGET_5_5_GIB);
            CHECK(virtual_guest_mem.budget() == BUDGET_5_5_GIB);

            // Mapeia regiões de código (0x400000) e heap
            CHECK(virtual_guest_mem.map(0x400000, 64 * 1024 * 1024, read | write) == MemoryError::ok);

            // Registra múltiplos callbacks coordenados de trim sob pressão Jetsam
            bool textures_purged = false;
            bool audio_purged = false;

            std::vector<std::vector<std::uint8_t>> active_textures;
            active_textures.push_back(std::vector<std::uint8_t>(1024 * 1024, 0xAA));

            // Configura áudio com porta e buffers ativos
            auto& audio_mgr = AudioOutManager::instance();
            std::int32_t audio_port = audio_mgr.open_port(0, 0, 0, 2048, 48000, 2);
            CHECK(audio_port >= 0);

            bool metal_cache_cleared = false;
            ios15pro_monitor.register_trim_callback([&]() {
                gnm_bridge_clear_caches();
                metal_cache_cleared = true;
            });

            ios15pro_monitor.register_trim_callback([&]() {
                active_textures.clear();
                textures_purged = true;
            });

            ios15pro_monitor.register_trim_callback([&]() {
                audio_mgr.purge_inactive_buffers();
                audio_purged = true;
            });

            // Dispara aviso de pressão de memória (didReceiveMemoryWarning)
            ios15pro_monitor.trigger_low_memory_warning();

            CHECK(metal_cache_cleared);
            CHECK(textures_purged);
            CHECK(audio_purged);
            CHECK(active_textures.empty());

            // Limpa porta de áudio
            CHECK(audio_mgr.close_port(audio_port) == 0);

            // Mede footprint no orçamento de 5,5 GiB
            std::uint64_t post_trim_footprint = ios15pro_monitor.query_physical_footprint_bytes();
            CHECK(post_trim_footprint < BUDGET_5_5_GIB);
            CHECK(ios15pro_monitor.get_peak_footprint_bytes() < BUDGET_5_5_GIB);
            CHECK(!ios15pro_monitor.is_budget_exceeded());
        }
    });
}
