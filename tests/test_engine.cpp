#include "check.h"
#include "engine_loop.h"
#include <filesystem>
#include <vector>
#include <cstring>

struct PlayerSaveData {
    std::uint32_t level;
    std::uint32_t blood_echoes;
    std::uint32_t hp;
    std::uint32_t stamina;
    char weapon_name[32];
};

int main() { return run([] {
    using namespace bb;

    std::filesystem::path base_test_dir = std::filesystem::current_path() / "test_engine_sandbox";
    std::filesystem::path app_dir = base_test_dir / "app0";
    std::filesystem::path save_dir = base_test_dir / "savedata";
    std::error_code ec;
    std::filesystem::create_directories(app_dir, ec);
    std::filesystem::create_directories(save_dir, ec);

    EngineConfig config{};
    config.app0_dir = app_dir.string();
    config.savedata_dir = save_dir.string();
    config.target_fps = 30.0;
    config.memory_pool_kb = 4096;

    EngineCoordinator engine(config);
    CHECK(engine.initialize());
    CHECK(engine.is_initialized());

    // Executa loop de 60 quadros simulados com entrada
    for (int frame = 0; frame < 60; ++frame) {
        if (frame == 10) {
            engine.input().set_button(PAD_CROSS, true); // Esquiva / Rolar
        } else if (frame == 15) {
            engine.input().set_button(PAD_CROSS, false);
            engine.input().set_button(PAD_R1, true); // Ataque com a arma
        } else if (frame == 20) {
            engine.input().set_button(PAD_R1, false);
            engine.input().set_left_stick(0.7f, 0.7f); // Movimentação analógica
        }

        auto result = engine.tick(33.33);
        CHECK(result.frame_index == static_cast<std::uint64_t>(frame + 1));
        CHECK(result.frame_time_ms == 33.33);

        if (frame >= 20) {
            CHECK(result.input_state.left_stick_x > 0.0f);
        }
    }

    CHECK(engine.frame_count() == 60);
    auto pacing = engine.pacing().get_stats();
    CHECK(pacing.total_frames == 60);
    CHECK(pacing.p50_ms >= 33.0 && pacing.p50_ms <= 34.0);

    // Teste de salvamento de estado do jogador com integridade CRC32
    PlayerSaveData player{};
    player.level = 35;
    player.blood_echoes = 42500;
    player.hp = 850;
    player.stamina = 120;
    std::strncpy(player.weapon_name, "Saw Cleaver +3", sizeof(player.weapon_name) - 1);

    CHECK(engine.save_game_state("slot_001", &player, sizeof(player)));

    // Leitura e verificação de integridade
    std::vector<std::uint8_t> loaded_bytes;
    CHECK(engine.load_game_state("slot_001", loaded_bytes));
    CHECK(loaded_bytes.size() == sizeof(PlayerSaveData));

    PlayerSaveData loaded_player{};
    std::memcpy(&loaded_player, loaded_bytes.data(), sizeof(PlayerSaveData));
    CHECK(loaded_player.level == 35);
    CHECK(loaded_player.blood_echoes == 42500);
    CHECK(loaded_player.hp == 850);
    CHECK(loaded_player.stamina == 120);
    CHECK(std::strcmp(loaded_player.weapon_name, "Saw Cleaver +3") == 0);

    // Teste de rejeição de save corrompido (tamper de dados)
    std::vector<std::uint8_t> corrupted_file_data;
    CHECK(engine.vfs().read_file("/user/home/savedata/slot_001.sav", corrupted_file_data) == VfsError::ok);

    // Modifica um byte do payload sem atualizar CRC
    corrupted_file_data.back() ^= 0xFF;
    CHECK(engine.vfs().write_file("/user/home/savedata/slot_001.sav", corrupted_file_data.data(), corrupted_file_data.size(), true) == VfsError::ok);

    // O carregamento deve detectar CRC32 incorreto e falhar com segurança
    std::vector<std::uint8_t> corrupt_load;
    CHECK(!engine.load_game_state("slot_001", corrupt_load));

    engine.shutdown();
    std::filesystem::remove_all(base_test_dir, ec);
}); }
