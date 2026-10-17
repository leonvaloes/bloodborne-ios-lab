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

        // 5. Teste de resiliência e preservação do arquivo anterior nas 4 fases críticas de falha (T042)
        std::vector<uint8_t> corrupted_attempt(16384, 0xEE);

        // Fase 1: Falha na criação/escrita do temporário .tmp
        sm.set_test_write_failure_phase(SaveDataManager::WriteFailurePhase::TmpWrite);
        CHECK(sm.write_file(mount_pt, "userdata0000", corrupted_attempt.data(), corrupted_attempt.size(), true) == SCE_SAVEDATA_ERROR_INTERNAL);
        readback.clear();
        CHECK(sm.read_file(mount_pt, "userdata0000", readback) == SCE_SAVEDATA_OK);
        CHECK(readback == updated_payload);

        // Fase 2: Falha na validação de integridade / checksum CRC32 do .tmp
        sm.set_test_write_failure_phase(SaveDataManager::WriteFailurePhase::Checksum);
        CHECK(sm.write_file(mount_pt, "userdata0000", corrupted_attempt.data(), corrupted_attempt.size(), true) == SCE_SAVEDATA_ERROR_INTERNAL);
        readback.clear();
        CHECK(sm.read_file(mount_pt, "userdata0000", readback) == SCE_SAVEDATA_OK);
        CHECK(readback == updated_payload);

        // Fase 3: Falha na fase de backup do destino (.bak)
        sm.set_test_write_failure_phase(SaveDataManager::WriteFailurePhase::Backup);
        CHECK(sm.write_file(mount_pt, "userdata0000", corrupted_attempt.data(), corrupted_attempt.size(), true) == SCE_SAVEDATA_ERROR_INTERNAL);
        readback.clear();
        CHECK(sm.read_file(mount_pt, "userdata0000", readback) == SCE_SAVEDATA_OK);
        CHECK(readback == updated_payload);

        // Fase 4: Falha na fase de substituição atômica / rename com restauração a partir do backup
        sm.set_test_write_failure_phase(SaveDataManager::WriteFailurePhase::Replacement);
        CHECK(sm.write_file(mount_pt, "userdata0000", corrupted_attempt.data(), corrupted_attempt.size(), true) == SCE_SAVEDATA_ERROR_INTERNAL);
        readback.clear();
        CHECK(sm.read_file(mount_pt, "userdata0000", readback) == SCE_SAVEDATA_OK);
        CHECK(readback == updated_payload);

        // Restaura operação normal e verifica que escrita bem-sucedida subsequente funciona
        sm.set_test_write_failure_phase(SaveDataManager::WriteFailurePhase::None);
        std::vector<uint8_t> valid_final_payload(8192, 0x99);
        CHECK(sm.write_file(mount_pt, "userdata0000", valid_final_payload.data(), valid_final_payload.size(), true) == SCE_SAVEDATA_OK);
        readback.clear();
        CHECK(sm.read_file(mount_pt, "userdata0000", readback) == SCE_SAVEDATA_OK);
        CHECK(readback == valid_final_payload);

        // 6. Commit e desmontagem
        CHECK(sm.umount(mount_pt, true) == SCE_SAVEDATA_OK);
        CHECK(sm.find_slot(mount_pt) == -1);

        // 7. T111: Rollback e recuperacao deterministica na falha de metadados apos substituicao de dados
        // 7a. Rollback de dados quando atualizacao de metadados falha
        ret = sm.mount(0, "CUSA03173", "SPRJ0001", SCE_SAVEDATA_MOUNT_MODE_RDWR, 4096, mount_pt, status);
        CHECK(ret == SCE_SAVEDATA_OK);
        std::vector<uint8_t> new_aborted_payload(8192, 0xEE);
        CHECK(sm.write_file(mount_pt, "userdata0000", new_aborted_payload.data(), new_aborted_payload.size(), true) == SCE_SAVEDATA_OK);

        // Injeta falha na fase de atualizacao de metadados durante o umount(commit=true)
        sm.set_test_write_failure_phase(SaveDataManager::WriteFailurePhase::MetadataUpdate);
        CHECK(sm.umount(mount_pt, true) == SCE_SAVEDATA_ERROR_INTERNAL);

        // Rollback restaura dados persistentes, mas mantém o trabalho montado para
        // retry; descartar explicitamente antes de abrir o slot em modo leitura.
        sm.set_test_write_failure_phase(SaveDataManager::WriteFailurePhase::None);
        CHECK(sm.umount(mount_pt, false) == SCE_SAVEDATA_OK);
        ret = sm.mount(0, "CUSA03173", "SPRJ0001", SCE_SAVEDATA_MOUNT_MODE_RDONLY, 4096, mount_pt, status);
        CHECK(ret == SCE_SAVEDATA_OK);
        readback.clear();
        CHECK(sm.read_file(mount_pt, "userdata0000", readback) == SCE_SAVEDATA_OK);
        CHECK(readback == valid_final_payload);
        CHECK(sm.umount(mount_pt, false) == SCE_SAVEDATA_OK);

        // 7b. Recuperacao deterministica entre reinicios se falha ocorreu antes da remocao do backup
        // Simula commit interrompido onde o slot SPRJ0001 contem dados novos corrompidos ou divergentes
        // e SPRJ0001.bak contem o backup consistente com o checksum em SPRJ0001.sce_sys
        const auto slot_dir = std::filesystem::path(test_dir) / "savedata" / "0" / "CUSA03173" / "SPRJ0001";
        const auto bak_slot_dir = std::filesystem::path(test_dir) / "savedata" / "0" / "CUSA03173" / "SPRJ0001.bak.12345678";
        std::filesystem::copy(slot_dir, bak_slot_dir, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
        // Altera slot_dir para que fique divergente do checksum registrado
        {
            std::ofstream bad_file(slot_dir / "userdata0000", std::ios::binary | std::ios::trunc);
            bad_file.write("DIVERGENT_UNCOMMITTED_DATA", 26);
        }

        // Reinicia o manager: recover_interrupted_commits deve detectar inconsistencia e restaurar do .bak
        sm.terminate();
        SaveDataManager recovered_sm;
        CHECK(recovered_sm.initialize(test_dir, 1024 * 1024));

        ret = recovered_sm.mount(0, "CUSA03173", "SPRJ0001", SCE_SAVEDATA_MOUNT_MODE_RDONLY, 4096, mount_pt, status);
        CHECK(ret == SCE_SAVEDATA_OK);
        readback.clear();
        CHECK(recovered_sm.read_file(mount_pt, "userdata0000", readback) == SCE_SAVEDATA_OK);
        CHECK(readback == valid_final_payload);
        CHECK(recovered_sm.umount(mount_pt, false) == SCE_SAVEDATA_OK);

        recovered_sm.terminate();
        std::filesystem::remove_all(test_dir, ec);
    });
}
