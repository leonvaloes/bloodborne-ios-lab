#include "check.h"
#include "save_data.h"
#include "vfs.h"
#include <filesystem>
#include <vector>
#include <string>
#include <cstring>
#include <fstream>

int main() { return run([] {
    using namespace bb;

    std::filesystem::path sandbox = std::filesystem::current_path() / "test_savedata_sandbox";
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
    std::filesystem::create_directories(sandbox, ec);

    // ========================================================================
    // 1. Validacao Estatica e Protecao de Traversal / Formato
    // ========================================================================
    CHECK(SaveDataManager::is_valid_dir_name("SPRJ0001"));
    CHECK(SaveDataManager::is_valid_dir_name("user_save_01"));
    CHECK(!SaveDataManager::is_valid_dir_name(""));
    CHECK(!SaveDataManager::is_valid_dir_name(nullptr));
    CHECK(!SaveDataManager::is_valid_dir_name(".hidden"));
    CHECK(!SaveDataManager::is_valid_dir_name(".."));
    CHECK(!SaveDataManager::is_valid_dir_name("slot/sub"));
    CHECK(!SaveDataManager::is_valid_dir_name("slot\\sub"));
    CHECK(!SaveDataManager::is_valid_dir_name("slot:bad"));
    CHECK(!SaveDataManager::is_valid_dir_name("slot*bad"));
    CHECK(!SaveDataManager::is_valid_dir_name("a_very_long_directory_name_that_exceeds_the_thirty_two_byte_limit_easily"));

    CHECK(SaveDataManager::contains_traversal("../secret"));
    CHECK(SaveDataManager::contains_traversal("data/../secret"));
    CHECK(SaveDataManager::contains_traversal("data\\..\\secret"));
    CHECK(!SaveDataManager::contains_traversal("data..file.bin"));
    CHECK(!SaveDataManager::contains_traversal("normal/path/save.bin"));

    CHECK(SaveDataManager::is_valid_title_id("CUSA03173"));
    CHECK(SaveDataManager::is_valid_title_id("CUSA00207"));
    CHECK(!SaveDataManager::is_valid_title_id("CUSA/00207"));
    CHECK(!SaveDataManager::is_valid_title_id("../CUSA"));

    CHECK(SaveDataManager::pattern_matches("SPRJ0001", "SPRJ%"));
    CHECK(SaveDataManager::pattern_matches("SPRJ0001", "SPRJ____"));
    CHECK(SaveDataManager::pattern_matches("SPRJ0001", "%0001"));
    CHECK(!SaveDataManager::pattern_matches("SPRJ0001", "OTHER%"));

    // ========================================================================
    // 2. Comportamento antes de inicializar
    // ========================================================================
    auto &mgr = SaveDataManager::instance();
    mgr.terminate(); // Garante estado nao-inicializado
    CHECK(!mgr.is_initialized());

    SceSaveDataMountPoint dummy_pt{};
    std::strncpy(dummy_pt.data, "/savedata0", sizeof(dummy_pt.data));
    CHECK(sceSaveDataUmount(&dummy_pt) == SCE_SAVEDATA_ERROR_NOT_INITIALIZED);

    SceSaveDataDirName dummy_dir{};
    std::strncpy(dummy_dir.data, "SPRJ0001", sizeof(dummy_dir.data));
    SceSaveDataMount mount_req{};
    mount_req.user = 0;
    mount_req.dir = &dummy_dir;
    mount_req.mode = SCE_SAVEDATA_MOUNT_MODE_CREATE;
    SceSaveDataMountResult mount_res{};
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_ERROR_NOT_INITIALIZED);

    // ========================================================================
    // 3. Inicializacao e Quota Configuravel
    // ========================================================================
    constexpr std::size_t test_quota = 64 * 1024; // 64 KiB para testes de limite
    CHECK(mgr.initialize(sandbox.string(), test_quota, nullptr));
    CHECK(mgr.is_initialized());
    CHECK(mgr.get_max_quota_per_slot() == test_quota);

    // ========================================================================
    // 4. Rejeicao de Traversal no Mount, Delete e Search
    // ========================================================================
    SceSaveDataDirName bad_dir{};
    std::strncpy(bad_dir.data, "../escape", sizeof(bad_dir.data));
    mount_req.dir = &bad_dir;
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_ERROR_PARAMETER);

    SceSaveDataDelete del_req{};
    del_req.user = 0;
    del_req.dir = &bad_dir;
    CHECK(sceSaveDataDelete(&del_req) == SCE_SAVEDATA_ERROR_PARAMETER);

    // Usuario invalido (negativo)
    mount_req.user = -1;
    mount_req.dir = &dummy_dir;
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_ERROR_INVALID_USER);

    // ========================================================================
    // 5. Ciclo de Montagem, Escrita e Commit Atomico
    // ========================================================================
    mount_req.user = 0;
    mount_req.dir = &dummy_dir;
    mount_req.mode = SCE_SAVEDATA_MOUNT_MODE_CREATE;
    mount_req.blocks = 64;
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);
    CHECK(std::string(mount_res.point.data) == "/savedata0");
    CHECK(mount_res.status == SCE_SAVEDATA_MOUNT_STATUS_CREATED);

    // Escrita de arquivo valido dentro da quota
    const std::string save_content_v1 = "Bloodborne Hunter Level 50 Insight 20";
    CHECK(mgr.write_file("/savedata0", "userdata0010", save_content_v1.data(), save_content_v1.size()) == SCE_SAVEDATA_OK);
    CHECK(mgr.file_exists("/savedata0", "userdata0010"));

    std::vector<uint8_t> read_buf;
    CHECK(mgr.read_file("/savedata0", "userdata0010", read_buf) == SCE_SAVEDATA_OK);
    CHECK(std::string(reinterpret_cast<char *>(read_buf.data()), read_buf.size()) == save_content_v1);

    // Tentativa de escrita com path traversal dentro do slot
    CHECK(mgr.write_file("/savedata0", "../escaped.dat", "data", 4) == SCE_SAVEDATA_ERROR_PARAMETER);
    CHECK(mgr.read_file("/savedata0", "../escaped.dat", read_buf) == SCE_SAVEDATA_ERROR_PARAMETER);

    // Parametros e metadados
    SceSaveDataParam param{};
    std::strncpy(param.title, "Bloodborne Test Save", sizeof(param.title) - 1);
    param.user_param = 100;
    CHECK(sceSaveDataSetParam(&mount_res.point, 0, &param, sizeof(param)) == SCE_SAVEDATA_OK);

    // Desmontagem com commit atomico
    CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) == SCE_SAVEDATA_OK);

    // Verifica que o arquivo persistente existe agora no disco host
    std::filesystem::path persistent_file = sandbox / "savedata" / "0" / "CUSA03173" / "SPRJ0001" / "userdata0010";
    CHECK(std::filesystem::exists(persistent_file));
    CHECK(std::filesystem::file_size(persistent_file) == save_content_v1.size());

    // ========================================================================
    // 6. Isolamento Transacional e Rollback (Umount com DISCARD)
    // ========================================================================
    // Remonta o save existente em modo leitura/escrita
    mount_req.mode = SCE_SAVEDATA_MOUNT_MODE_RDWR;
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);
    CHECK(mount_res.status == SCE_SAVEDATA_MOUNT_STATUS_EXISTING);

    // Leitura verifica o conteudo persistido anteriormente
    CHECK(mgr.read_file("/savedata0", "userdata0010", read_buf) == SCE_SAVEDATA_OK);
    CHECK(std::string(reinterpret_cast<char *>(read_buf.data()), read_buf.size()) == save_content_v1);

    // Modifica o save na copia de trabalho
    const std::string save_content_modified = "Corrupted Hunter State during Crash";
    CHECK(mgr.write_file("/savedata0", "userdata0010", save_content_modified.data(), save_content_modified.size()) == SCE_SAVEDATA_OK);

    // Desmonta com descarte / rollback (ex: falha de energia ou abort)
    CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_DISCARD) == SCE_SAVEDATA_OK);

    // Remonta o slot: deve conter a versao original (rollback bem-sucedido!)
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);
    CHECK(mgr.read_file("/savedata0", "userdata0010", read_buf) == SCE_SAVEDATA_OK);
    CHECK(std::string(reinterpret_cast<char *>(read_buf.data()), read_buf.size()) == save_content_v1);

    // Agora modifica e faz commit
    const std::string save_content_v2 = "Bloodborne Hunter Level 60 Insight 35 Echoes 100000";
    CHECK(mgr.write_file("/savedata0", "userdata0010", save_content_v2.data(), save_content_v2.size()) == SCE_SAVEDATA_OK);
    CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) == SCE_SAVEDATA_OK);

    // Remonta e confirma versao v2 persistida
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);
    CHECK(mgr.read_file("/savedata0", "userdata0010", read_buf) == SCE_SAVEDATA_OK);
    CHECK(std::string(reinterpret_cast<char *>(read_buf.data()), read_buf.size()) == save_content_v2);
    CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) == SCE_SAVEDATA_OK);

    // ========================================================================
    // 7. Protecao Estrita de Quota
    // ========================================================================
    // Quota configurada = 64 KiB
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);
    std::vector<char> large_payload(70 * 1024, 'X'); // 70 KiB > 64 KiB
    CHECK(mgr.write_file("/savedata0", "large_blob.bin", large_payload.data(), large_payload.size()) == SCE_SAVEDATA_ERROR_QUOTA_EXCEEDED);

    // Grava arquivo de 32 KiB (dentro da quota)
    std::vector<char> fit_payload(32 * 1024, 'Y');
    CHECK(mgr.write_file("/savedata0", "chunk.bin", fit_payload.data(), fit_payload.size()) == SCE_SAVEDATA_OK);

    // Outro de 40 KiB (32 + 40 > 64 KiB)
    std::vector<char> overflow_payload(40 * 1024, 'Z');
    CHECK(mgr.write_file("/savedata0", "overflow.bin", overflow_payload.data(), overflow_payload.size()) == SCE_SAVEDATA_ERROR_QUOTA_EXCEEDED);

    CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) == SCE_SAVEDATA_OK);

    // ========================================================================
    // 8. Integracao com VirtualFileSystem (VFS)
    // ========================================================================
    VirtualFileSystem vfs;
    mgr.set_vfs(&vfs);

    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);
    CHECK(vfs.is_mounted("/savedata0"));

    // Leitura via VFS
    std::vector<uint8_t> vfs_read;
    CHECK(vfs.read_file("/savedata0/userdata0010", vfs_read) == VfsError::ok);
    CHECK(std::string(reinterpret_cast<char *>(vfs_read.data()), vfs_read.size()) == save_content_v2);

    // Escrita via VFS
    const std::string vfs_extra = "VFS Direct Payload";
    CHECK(vfs.write_file("/savedata0/vfs_file.dat", vfs_extra.data(), vfs_extra.size()) == VfsError::ok);

    CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) == SCE_SAVEDATA_OK);
    CHECK(!vfs.is_mounted("/savedata0"));

    // Confirma persistencia do arquivo gravado via VFS
    std::filesystem::path vfs_persisted = sandbox / "savedata" / "0" / "CUSA03173" / "SPRJ0001" / "vfs_file.dat";
    CHECK(std::filesystem::exists(vfs_persisted));

    mgr.set_vfs(nullptr);

    // ========================================================================
    // 9. Enumeracao e Busca de Diretorios (sceSaveDataDirNameSearch)
    // ========================================================================
    // Cria saves adicionais: SPRJ0002 e SPRJ0003
    SceSaveDataDirName dir2{}, dir3{};
    std::strncpy(dir2.data, "SPRJ0002", sizeof(dir2.data));
    std::strncpy(dir3.data, "SPRJ0003", sizeof(dir3.data));

    mount_req.dir = &dir2;
    mount_req.mode = SCE_SAVEDATA_MOUNT_MODE_CREATE;
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);
    SceSaveDataParam p2{};
    std::strncpy(p2.title, "Save Slot 2", sizeof(p2.title) - 1);
    p2.user_param = 250;
    CHECK(sceSaveDataSetParam(&mount_res.point, 0, &p2, sizeof(p2)) == SCE_SAVEDATA_OK);
    CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) == SCE_SAVEDATA_OK);

    mount_req.dir = &dir3;
    mount_req.mode = SCE_SAVEDATA_MOUNT_MODE_CREATE;
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);
    SceSaveDataParam p3{};
    std::strncpy(p3.title, "Save Slot 3", sizeof(p3.title) - 1);
    p3.user_param = 150;
    CHECK(sceSaveDataSetParam(&mount_res.point, 0, &p3, sizeof(p3)) == SCE_SAVEDATA_OK);
    CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) == SCE_SAVEDATA_OK);

    // Busca geral (todos os saves)
    SceSaveDataSearchCond cond{};
    cond.user = 0;
    cond.key = SCE_SAVEDATA_SORT_KEY_DIRNAME;
    cond.order = SCE_SAVEDATA_SORT_ORDER_ASC;

    SceSaveDataDirName names[4]{};
    SceSaveDataParam params[4]{};
    SceSaveDataSearchInfo infos[4]{};
    SceSaveDataSearchResult res{};
    res.names = names;
    res.params = params;
    res.infos = infos;
    res.names_capacity = 4;

    CHECK(sceSaveDataDirNameSearch(&cond, &res) == SCE_SAVEDATA_OK);
    CHECK(res.hits == 3);
    CHECK(res.set_count == 3);
    CHECK(std::string(names[0].data) == "SPRJ0001");
    CHECK(std::string(names[1].data) == "SPRJ0002");
    CHECK(std::string(names[2].data) == "SPRJ0003");

    // Busca com filtro de padrao (wildcard)
    SceSaveDataDirName filter{};
    std::strncpy(filter.data, "%0002", sizeof(filter.data));
    cond.dir = &filter;
    CHECK(sceSaveDataDirNameSearch(&cond, &res) == SCE_SAVEDATA_OK);
    CHECK(res.hits == 1);
    CHECK(std::string(names[0].data) == "SPRJ0002");

    // Busca ordenada por user_param decrescente
    cond.dir = nullptr;
    cond.key = SCE_SAVEDATA_SORT_KEY_USER_PARAM;
    cond.order = SCE_SAVEDATA_SORT_ORDER_DESC;
    CHECK(sceSaveDataDirNameSearch(&cond, &res) == SCE_SAVEDATA_OK);
    CHECK(res.hits == 3);
    // Ordenacao desc por user_param: p2 (250) -> p3 (150) -> p1 (100)
    CHECK(std::string(names[0].data) == "SPRJ0002");
    CHECK(std::string(names[1].data) == "SPRJ0003");
    CHECK(std::string(names[2].data) == "SPRJ0001");

    // Limite de capacidade (names_capacity = 2)
    res.names_capacity = 2;
    CHECK(sceSaveDataDirNameSearch(&cond, &res) == SCE_SAVEDATA_OK);
    CHECK(res.hits == 3);
    CHECK(res.set_count == 2);

    // ========================================================================
    // 10. Remocao Segura de Save (sceSaveDataDelete)
    // ========================================================================
    // Nao pode remover enquanto montado
    mount_req.dir = &dir3;
    mount_req.mode = SCE_SAVEDATA_MOUNT_MODE_RDWR;
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);

    del_req.user = 0;
    del_req.dir = &dir3;
    CHECK(sceSaveDataDelete(&del_req) == SCE_SAVEDATA_ERROR_BAD_MOUNTED);

    CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_DISCARD) == SCE_SAVEDATA_OK);

    // Remocao bem-sucedida apos desmontagem
    CHECK(sceSaveDataDelete(&del_req) == SCE_SAVEDATA_OK);
    std::filesystem::path del_path = sandbox / "savedata" / "0" / "CUSA03173" / "SPRJ0003";
    CHECK(!std::filesystem::exists(del_path));

    // Remocao de slot ja inexistente retorna NOT_FOUND
    CHECK(sceSaveDataDelete(&del_req) == SCE_SAVEDATA_ERROR_NOT_FOUND);

    // ========================================================================
    // 11. Bloodborne Audio Hack
    // ========================================================================
    // Cria save SPRJ0005 com userdata0010 contendo flag 0x204E = 0
    SceSaveDataDirName dir_bb{};
    std::strncpy(dir_bb.data, "SPRJ0005", sizeof(dir_bb.data));
    mount_req.dir = &dir_bb;
    mount_req.mode = SCE_SAVEDATA_MOUNT_MODE_CREATE;
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);

    std::vector<char> bb_sound_data(0x2100, 0);
    bb_sound_data[0x204E] = 0; // Audio desligado inicialmente
    CHECK(mgr.write_file("/savedata0", "userdata0010", bb_sound_data.data(), bb_sound_data.size()) == SCE_SAVEDATA_OK);
    CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) == SCE_SAVEDATA_OK);

    // Remonta o save: apply_bloodborne_sound_hack deve ativar o flag na copia de trabalho
    mount_req.mode = SCE_SAVEDATA_MOUNT_MODE_RDWR;
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);
    CHECK(mgr.read_file("/savedata0", "userdata0010", read_buf) == SCE_SAVEDATA_OK);
    CHECK(read_buf.size() == bb_sound_data.size());
    CHECK(read_buf[0x204E] == 1); // Flag de som ativado automaticamente!
    CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) == SCE_SAVEDATA_OK);

    // ========================================================================
    // 12. SaveDataMemory (setup, set, get)
    // ========================================================================
    CHECK(sceSaveDataSetupSaveDataMemory(0, 1024, nullptr) == SCE_SAVEDATA_OK);
    char mem_in[] = "Bloodborne Shared Memory Block";
    CHECK(sceSaveDataSetSaveDataMemory(0, mem_in, sizeof(mem_in), 64) == SCE_SAVEDATA_OK);

    char mem_out[sizeof(mem_in)]{};
    CHECK(sceSaveDataGetSaveDataMemory(0, mem_out, sizeof(mem_out), 64) == SCE_SAVEDATA_OK);
    CHECK(std::string(mem_out) == mem_in);

    // Tentativa fora dos limites
    CHECK(sceSaveDataGetSaveDataMemory(0, mem_out, sizeof(mem_out), 2000) == SCE_SAVEDATA_ERROR_PARAMETER);

    // Finalizacao e limpeza
    CHECK(sceSaveDataTerminate() == SCE_SAVEDATA_OK);
    std::filesystem::remove_all(sandbox, ec);
}); }
