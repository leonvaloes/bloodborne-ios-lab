#include "check.h"
#include "save_data.h"
#include "orbis_runtime.h"
#include "guest_memory.h"
#include "vfs.h"
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

        // 3. Sessão com OrbisRuntime HLE: teste de Continuar, auto-save e restauração completa de atributos/mundo (T041)
        struct BloodborneCharacterState {
            uint32_t level{10};
            uint32_t blood_echoes{5200};
            uint32_t insight{4};
            uint32_t hp{594};
            uint32_t stamina{95};
            float position[3]{142.5f, 3.2f, -88.7f};
            uint32_t world_map_id{2900000}; // Yharnam Central
            uint32_t right_weapon_id{1000000}; // Saw Cleaver
            uint32_t left_weapon_id{1100000};  // Hunter Pistol
            uint32_t blood_vials{20};
            uint32_t quicksilver_bullets{10};
        };

        const std::string hle_save_dir = "build-Release/test_hle_profile_dir";
        std::filesystem::remove_all(hle_save_dir, ec);
        std::filesystem::create_directories(hle_save_dir, ec);

        // Sub-sessão 1: Novo jogo cria save, grava personagem e salva com commit
        {
            GuestMemory mem(16 * 1024 * 1024);
            mem.map(0x10000, 0x100000, Permission::read | Permission::write);
            VirtualFileSystem vfs;
            vfs.mount("/savedata", hle_save_dir, false);
            OrbisRuntime rt(mem);
            rt.attach_vfs(&vfs);
            rt.save_data_manager().initialize(hle_save_dir, 16 * 1024 * 1024, &vfs);
            rt.save_data_manager().set_default_title_id("CUSA03173");

            // sceSaveDataMount (slot SPRJ0001)
            SceSaveDataMount m{};
            m.user = 0;
            SceSaveDataTitleId t_id{};
            std::memcpy(t_id.data, "CUSA03173", sizeof("CUSA03173"));
            SceSaveDataDirName d_name{"SPRJ0001"};
            m.title = &t_id;
            m.dir = &d_name;
            m.mode = SCE_SAVEDATA_MOUNT_MODE_RDWR | SCE_SAVEDATA_MOUNT_MODE_CREATE2;
            m.blocks = 8192;

            uint64_t mount_req = 0x20000;
            uint64_t mount_res = 0x21000;
            mem.store(mount_req + 200, &t_id, sizeof(t_id));
            mem.store(mount_req + 300, &d_name, sizeof(d_name));
            m.title = reinterpret_cast<SceSaveDataTitleId*>(mount_req + 200);
            m.dir = reinterpret_cast<SceSaveDataDirName*>(mount_req + 300);
            mem.store(mount_req, &m, sizeof(m));

            CpuState cpu{};
            cpu.gpr[7] = mount_req; // rdi
            cpu.gpr[6] = mount_res; // rsi
            CHECK(rt.invoke("sceSaveDataMount", cpu) == 0);

            SceSaveDataMountResult m_res{};
            mem.load(mount_res, &m_res, sizeof(m_res));
            CHECK(std::string(m_res.point.data) == "/savedata0");
            CHECK(m_res.status == SCE_SAVEDATA_MOUNT_STATUS_CREATED);

            // Grava o estado do personagem via sceKernelOpen / sceKernelWrite
            BloodborneCharacterState hunter{};
            std::string save_file = "/savedata0/userdata0000";
            uint64_t path_addr = 0x30000;
            mem.store(path_addr, save_file.c_str(), save_file.size() + 1);

            cpu.gpr[7] = path_addr;
            cpu.gpr[6] = 0x0002 | 0x0200; // O_RDWR | O_CREAT
            cpu.gpr[2] = 0644;
            int32_t fd = static_cast<int32_t>(rt.invoke("sceKernelOpen", cpu));
            CHECK(fd > 0);

            uint64_t data_addr = 0x40000;
            mem.store(data_addr, &hunter, sizeof(hunter));
            cpu.gpr[7] = fd;
            cpu.gpr[6] = data_addr;
            cpu.gpr[2] = sizeof(hunter);
            int64_t written = static_cast<int64_t>(rt.invoke("sceKernelWrite", cpu));
            CHECK(written == sizeof(hunter));

            cpu.gpr[7] = fd;
            CHECK(rt.invoke("sceKernelClose", cpu) == 0);

            // sceSaveDataUmount com commit
            uint64_t pt_addr = 0x50000;
            mem.store(pt_addr, m_res.point.data, sizeof(m_res.point.data));
            cpu.gpr[7] = pt_addr;
            cpu.gpr[6] = SCE_SAVEDATA_UMOUNT_FLAG_COMMIT;
            CHECK(rt.invoke("sceSaveDataUmount", cpu) == 0);
        }

        // Sub-sessão 2: Reinício a frio do app e seleção de "Continuar"
        {
            GuestMemory mem(16 * 1024 * 1024);
            mem.map(0x10000, 0x100000, Permission::read | Permission::write);
            VirtualFileSystem vfs;
            vfs.mount("/savedata", hle_save_dir, false);
            OrbisRuntime rt(mem);
            rt.attach_vfs(&vfs);
            rt.save_data_manager().initialize(hle_save_dir, 16 * 1024 * 1024, &vfs);
            rt.save_data_manager().set_default_title_id("CUSA03173");

            // Monta o slot existente com modo RDWR
            SceSaveDataMount m{};
            m.user = 0;
            SceSaveDataTitleId t_id{};
            std::memcpy(t_id.data, "CUSA03173", sizeof("CUSA03173"));
            SceSaveDataDirName d_name{"SPRJ0001"};
            m.mode = SCE_SAVEDATA_MOUNT_MODE_RDWR;
            m.blocks = 8192;

            uint64_t mount_req = 0x20000;
            uint64_t mount_res = 0x21000;
            mem.store(mount_req + 200, &t_id, sizeof(t_id));
            mem.store(mount_req + 300, &d_name, sizeof(d_name));
            m.title = reinterpret_cast<SceSaveDataTitleId*>(mount_req + 200);
            m.dir = reinterpret_cast<SceSaveDataDirName*>(mount_req + 300);
            mem.store(mount_req, &m, sizeof(m));

            CpuState cpu{};
            cpu.gpr[7] = mount_req;
            cpu.gpr[6] = mount_res;
            CHECK(rt.invoke("sceSaveDataMount", cpu) == 0);

            SceSaveDataMountResult m_res{};
            mem.load(mount_res, &m_res, sizeof(m_res));
            CHECK(m_res.status == SCE_SAVEDATA_MOUNT_STATUS_EXISTING);

            // Abre e lê o arquivo de progresso userdata0000
            std::string save_file = "/savedata0/userdata0000";
            uint64_t path_addr = 0x30000;
            mem.store(path_addr, save_file.c_str(), save_file.size() + 1);

            cpu.gpr[7] = path_addr;
            cpu.gpr[6] = 0x0002; // O_RDWR
            int32_t fd = static_cast<int32_t>(rt.invoke("sceKernelOpen", cpu));
            CHECK(fd > 0);

            uint64_t read_buf_addr = 0x60000;
            cpu.gpr[7] = fd;
            cpu.gpr[6] = read_buf_addr;
            cpu.gpr[2] = sizeof(BloodborneCharacterState);
            int64_t bytes_read = static_cast<int64_t>(rt.invoke("sceKernelRead", cpu));
            CHECK(bytes_read == sizeof(BloodborneCharacterState));

            BloodborneCharacterState loaded{};
            mem.load(read_buf_addr, &loaded, sizeof(loaded));

            // Validação estrita de todos os atributos, inventário, posição e mapa (T041)
            CHECK(loaded.level == 10);
            CHECK(loaded.blood_echoes == 5200);
            CHECK(loaded.insight == 4);
            CHECK(loaded.hp == 594);
            CHECK(loaded.stamina == 95);
            CHECK(loaded.position[0] == 142.5f);
            CHECK(loaded.position[1] == 3.2f);
            CHECK(loaded.position[2] == -88.7f);
            CHECK(loaded.world_map_id == 2900000);
            CHECK(loaded.right_weapon_id == 1000000);
            CHECK(loaded.left_weapon_id == 1100000);
            CHECK(loaded.blood_vials == 20);
            CHECK(loaded.quicksilver_bullets == 10);

            // Simula auto-save durante gameplay: evolução do personagem
            loaded.level = 15;
            loaded.blood_echoes = 12500;
            loaded.position[0] = 205.0f;
            loaded.position[2] = -120.0f;
            mem.store(read_buf_addr, &loaded, sizeof(loaded));

            cpu.gpr[7] = fd;
            cpu.gpr[6] = 0; // offset
            cpu.gpr[2] = 0; // SEEK_SET
            CHECK(rt.invoke("sceKernelLseek", cpu) == 0);

            cpu.gpr[7] = fd;
            cpu.gpr[6] = read_buf_addr;
            cpu.gpr[2] = sizeof(loaded);
            CHECK(static_cast<int64_t>(rt.invoke("sceKernelWrite", cpu)) == sizeof(loaded));

            cpu.gpr[7] = fd;
            CHECK(rt.invoke("sceKernelClose", cpu) == 0);

            // Commit do auto-save
            uint64_t pt_addr = 0x50000;
            mem.store(pt_addr, m_res.point.data, sizeof(m_res.point.data));
            cpu.gpr[7] = pt_addr;
            cpu.gpr[6] = SCE_SAVEDATA_UMOUNT_FLAG_COMMIT;
            CHECK(rt.invoke("sceSaveDataUmount", cpu) == 0);
        }

        // Sub-sessão 3: Reinício a frio subsequente comprovando persistência do auto-save
        {
            GuestMemory mem(16 * 1024 * 1024);
            mem.map(0x10000, 0x100000, Permission::read | Permission::write);
            VirtualFileSystem vfs;
            vfs.mount("/savedata", hle_save_dir, false);
            OrbisRuntime rt(mem);
            rt.attach_vfs(&vfs);
            rt.save_data_manager().initialize(hle_save_dir, 16 * 1024 * 1024, &vfs);
            rt.save_data_manager().set_default_title_id("CUSA03173");

            SceSaveDataMount m{};
            m.user = 0;
            SceSaveDataTitleId t_id{};
            std::memcpy(t_id.data, "CUSA03173", sizeof("CUSA03173"));
            SceSaveDataDirName d_name{"SPRJ0001"};
            m.mode = SCE_SAVEDATA_MOUNT_MODE_RDONLY;
            m.blocks = 8192;

            uint64_t mount_req = 0x20000;
            uint64_t mount_res = 0x21000;
            mem.store(mount_req + 200, &t_id, sizeof(t_id));
            mem.store(mount_req + 300, &d_name, sizeof(d_name));
            m.title = reinterpret_cast<SceSaveDataTitleId*>(mount_req + 200);
            m.dir = reinterpret_cast<SceSaveDataDirName*>(mount_req + 300);
            mem.store(mount_req, &m, sizeof(m));

            CpuState cpu{};
            cpu.gpr[7] = mount_req;
            cpu.gpr[6] = mount_res;
            CHECK(rt.invoke("sceSaveDataMount", cpu) == 0);

            std::string save_file = "/savedata0/userdata0000";
            uint64_t path_addr = 0x30000;
            mem.store(path_addr, save_file.c_str(), save_file.size() + 1);

            cpu.gpr[7] = path_addr;
            cpu.gpr[6] = 0x0000; // O_RDONLY
            int32_t fd = static_cast<int32_t>(rt.invoke("sceKernelOpen", cpu));
            CHECK(fd > 0);

            uint64_t read_buf_addr = 0x60000;
            cpu.gpr[7] = fd;
            cpu.gpr[6] = read_buf_addr;
            cpu.gpr[2] = sizeof(BloodborneCharacterState);
            CHECK(static_cast<int64_t>(rt.invoke("sceKernelRead", cpu)) == sizeof(BloodborneCharacterState));

            BloodborneCharacterState autosaved{};
            mem.load(read_buf_addr, &autosaved, sizeof(autosaved));
            CHECK(autosaved.level == 15);
            CHECK(autosaved.blood_echoes == 12500);
            CHECK(autosaved.position[0] == 205.0f);
            CHECK(autosaved.position[2] == -120.0f);

            cpu.gpr[7] = fd;
            CHECK(rt.invoke("sceKernelClose", cpu) == 0);

            SceSaveDataMountResult m_res{};
            mem.load(mount_res, &m_res, sizeof(m_res));
            uint64_t pt_addr = 0x50000;
            mem.store(pt_addr, m_res.point.data, sizeof(m_res.point.data));
            cpu.gpr[7] = pt_addr;
            cpu.gpr[6] = SCE_SAVEDATA_UMOUNT_FLAG_DISCARD;
            CHECK(rt.invoke("sceSaveDataUmount", cpu) == 0);
        }

        std::filesystem::remove_all(hle_save_dir, ec);
    });
}
