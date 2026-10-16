#include "check.h"
#include "save_data.h"
#include "vfs.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>

int main() {
    return run([] {
        using namespace bb;

        const std::string test_dir = "build-Release/test_atomic_savedata_dir";
        std::error_code ec;
        std::filesystem::remove_all(test_dir, ec);
        std::filesystem::create_directories(test_dir, ec);

        SaveDataManager sm;
        CHECK(sm.initialize(test_dir, 1024 * 1024)); // 1 MiB quota
        CHECK(sm.is_initialized());

        std::string mount_pt;
        uint32_t status = 0;
        int32_t ret = sm.mount(0, "CUSA03173", "SPRJ0001",
                               SCE_SAVEDATA_MOUNT_MODE_RDWR | SCE_SAVEDATA_MOUNT_MODE_CREATE2,
                               4096, mount_pt, status);
        CHECK(ret == SCE_SAVEDATA_OK);
        CHECK(status == SCE_SAVEDATA_MOUNT_STATUS_CREATED);

        // 1. Gravação válida com protocolo atômico em 4 fases (tmp -> checksum -> backup -> rename)
        std::vector<uint8_t> save_payload(16384, 0x42);
        CHECK(sm.write_file(mount_pt, "userdata0000", save_payload.data(), save_payload.size(), true) == SCE_SAVEDATA_OK);

        std::vector<uint8_t> readback;
        CHECK(sm.read_file(mount_pt, "userdata0000", readback) == SCE_SAVEDATA_OK);
        CHECK(readback.size() == save_payload.size());
        CHECK(readback == save_payload);

        // 2. Simulação de interrupção de escrita (falha abrupta deixando lixo temporário)
        int slot_idx = sm.find_slot(mount_pt);
        CHECK(slot_idx >= 0);
        const auto* slot = sm.get_slot(slot_idx);
        CHECK(slot != nullptr);

        // Cria arquivo corrompido / truncado simulando crash no meio de uma escrita anterior
        std::filesystem::path fake_tmp = std::filesystem::path(slot->work_path) / "userdata0000.tmp.999999";
        {
            std::ofstream out(fake_tmp, std::ios::binary);
            out.write("CORRUPTED_PARTIAL_DATA", 22);
        }
        CHECK(std::filesystem::exists(fake_tmp, ec));

        // O arquivo original userdata0000 continua 100% íntegro e inalterado!
        readback.clear();
        CHECK(sm.read_file(mount_pt, "userdata0000", readback) == SCE_SAVEDATA_OK);
        CHECK(readback == save_payload);

        // 3. Atualização atômica subsequente substitui os dados com integridade
        std::vector<uint8_t> updated_payload(32768, 0x77);
        CHECK(sm.write_file(mount_pt, "userdata0000", updated_payload.data(), updated_payload.size(), true) == SCE_SAVEDATA_OK);

        readback.clear();
        CHECK(sm.read_file(mount_pt, "userdata0000", readback) == SCE_SAVEDATA_OK);
        CHECK(readback.size() == updated_payload.size());
        CHECK(readback == updated_payload);

        // 4. Teste de rejeição por quota excedida sem corromper o arquivo existente
        std::vector<uint8_t> huge_payload(2 * 1024 * 1024, 0xFF); // 2 MB > 1 MB quota
        CHECK(sm.write_file(mount_pt, "userdata0000", huge_payload.data(), huge_payload.size(), true) == SCE_SAVEDATA_ERROR_QUOTA_EXCEEDED);

        // Após falha de quota, os dados continuam intactos com o payload anterior
        readback.clear();
        CHECK(sm.read_file(mount_pt, "userdata0000", readback) == SCE_SAVEDATA_OK);
        CHECK(readback == updated_payload);

        // 5. Commit e desmontagem
        CHECK(sm.umount(mount_pt, true) == SCE_SAVEDATA_OK);
        CHECK(sm.find_slot(mount_pt) == -1);

        sm.terminate();
        std::filesystem::remove_all(test_dir, ec);
    });
}
