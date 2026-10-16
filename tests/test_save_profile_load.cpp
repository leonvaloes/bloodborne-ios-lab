#include "check.h"
#include "save_data.h"
#include <filesystem>
#include <vector>
#include <cstring>

int main() {
    return run([] {
        using namespace bb;

        const std::string test_dir = "build-Release/test_profile_savedata_dir";
        std::error_code ec;
        std::filesystem::remove_all(test_dir, ec);
        std::filesystem::create_directories(test_dir, ec);

        // 1. Sessão inicial: criação e persistência do perfil do Caçador
        {
            SaveDataManager sm;
            CHECK(sm.initialize(test_dir, 4 * 1024 * 1024));
            sm.set_default_title_id("CUSA03173");

            std::string mount_pt;
            uint32_t status = 0;
            CHECK(sm.mount(0, "CUSA03173", "SPRJ0001",
                           SCE_SAVEDATA_MOUNT_MODE_RDWR | SCE_SAVEDATA_MOUNT_MODE_CREATE2,
                           8192, mount_pt, status) == SCE_SAVEDATA_OK);
            CHECK(status == SCE_SAVEDATA_MOUNT_STATUS_CREATED);

            // Grava metadados do perfil (SceSaveDataParam)
            SceSaveDataParam param{};
            std::strncpy(param.title, "Caçador - Clínica de Iosefka", sizeof(param.title) - 1);
            std::strncpy(param.subtitle, "Nível 10 - Ecos de Sangue: 2500", sizeof(param.subtitle) - 1);
            std::strncpy(param.detail, "Clínica de Iosefka / Primeiro Despertar", sizeof(param.detail) - 1);
            param.user_param = 0x0001000A; // slot 1, level 10
            CHECK(sm.set_param(mount_pt, 0, &param, sizeof(param)) == SCE_SAVEDATA_OK);

            // Grava dados binários de progresso do jogo
            struct HunterSaveState {
                uint32_t level{10};
                uint32_t blood_echoes{2500};
                uint32_t insight{1};
                float position[3]{10.5f, 2.0f, -45.0f};
                uint32_t inventory_count{8};
            } state;

            CHECK(sm.write_file(mount_pt, "game_data.bin", &state, sizeof(state), true) == SCE_SAVEDATA_OK);

            // Commit e desmontagem da sessão inicial
            CHECK(sm.umount(mount_pt, true) == SCE_SAVEDATA_OK);
            sm.terminate();
        }

        // 2. Segunda sessão: simulação de reinicialização do app (cold restart) e recarga de perfil
        {
            SaveDataManager sm;
            CHECK(sm.initialize(test_dir, 4 * 1024 * 1024));
            sm.set_default_title_id("CUSA03173");

            // Busca de diretórios de save persistidos em disco
            std::vector<SaveSearchEntry> entries;
            CHECK(sm.dir_name_search(0, "CUSA03173", "%", SCE_SAVEDATA_SORT_KEY_MTIME, SCE_SAVEDATA_SORT_ORDER_DESC, entries) == SCE_SAVEDATA_OK);
            CHECK(entries.size() == 1);
            CHECK(entries[0].dir_name == "SPRJ0001");
            CHECK(std::strcmp(entries[0].param.title, "Caçador - Clínica de Iosefka") == 0);
            CHECK(entries[0].param.user_param == 0x0001000A);

            // Monta o slot de salvamento existente (sem criar novo)
            std::string mount_pt;
            uint32_t status = 0;
            CHECK(sm.mount(0, "CUSA03173", "SPRJ0001",
                           SCE_SAVEDATA_MOUNT_MODE_RDWR,
                           8192, mount_pt, status) == SCE_SAVEDATA_OK);
            CHECK(status == SCE_SAVEDATA_MOUNT_STATUS_EXISTING);

            // Leitura e verificação de metadados
            SceSaveDataParam loaded_param{};
            CHECK(sm.get_param(mount_pt, loaded_param) == SCE_SAVEDATA_OK);
            CHECK(std::strcmp(loaded_param.title, "Caçador - Clínica de Iosefka") == 0);
            CHECK(std::strcmp(loaded_param.subtitle, "Nível 10 - Ecos de Sangue: 2500") == 0);

            // Leitura do estado binário de jogo do Caçador
            std::vector<uint8_t> data;
            CHECK(sm.read_file(mount_pt, "game_data.bin", data) == SCE_SAVEDATA_OK);

            struct HunterSaveState {
                uint32_t level;
                uint32_t blood_echoes;
                uint32_t insight;
                float position[3];
                uint32_t inventory_count;
            } loaded_state{};
            CHECK(data.size() == sizeof(HunterSaveState));
            std::memcpy(&loaded_state, data.data(), sizeof(loaded_state));

            CHECK(loaded_state.level == 10);
            CHECK(loaded_state.blood_echoes == 2500);
            CHECK(loaded_state.insight == 1);
            CHECK(loaded_state.position[0] == 10.5f);
            CHECK(loaded_state.position[1] == 2.0f);
            CHECK(loaded_state.position[2] == -45.0f);
            CHECK(loaded_state.inventory_count == 8);

            CHECK(sm.umount(mount_pt, false) == SCE_SAVEDATA_OK);
            sm.terminate();
        }

        std::filesystem::remove_all(test_dir, ec);
    });
}
