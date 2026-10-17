#include "check.h"
#include "save_data.h"
#include "vfs.h"
#include <filesystem>
#include <vector>
#include <string>
#include <cstring>
#include <fstream>

// Falhas controladas no limite do filesystem, independentes das ACL do Windows.
struct FailingDeleteFilesystem : bb::SaveDeleteFilesystem {
    int fail_exists_at{0};
    int fail_remove_at{0};
    int exists_calls{0};
    int remove_calls{0};
    bool exists(const std::filesystem::path &path, std::error_code &ec) override {
        if (++exists_calls == fail_exists_at) {
            ec = std::make_error_code(std::errc::permission_denied);
            return false;
        }
        return std::filesystem::exists(path, ec);
    }
    std::uintmax_t remove_all(const std::filesystem::path &path, std::error_code &ec) override {
        if (++remove_calls == fail_remove_at) {
            ec = std::make_error_code(std::errc::permission_denied);
            return 0;
        }
        return std::filesystem::remove_all(path, ec);
    }
};

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
    // Um arquivo no lugar de savedata impede a criação do diretório de trabalho.
    // A falha não deve publicar estado inicializado ou substituir a configuração anterior.
    {
        const auto blocked = sandbox / "blocked";
        std::filesystem::create_directories(blocked);
        { std::ofstream file(blocked / "savedata", std::ios::binary); file << "sentinel"; }
        bb::SaveDataManager failed;
        CHECK(!failed.initialize(blocked.string(), test_quota, nullptr));
        CHECK(!failed.is_initialized());
        CHECK(failed.get_base_directory().empty());
        CHECK(!std::filesystem::exists(blocked / "savedata" / "_work"));
        std::ifstream file(blocked / "savedata", std::ios::binary);
        std::string content;
        file >> content;
        CHECK(content == "sentinel");
    }
    {
        const auto blocked = sandbox / "blocked_work";
        std::filesystem::create_directories(blocked / "savedata");
        { std::ofstream file(blocked / "savedata" / "_work"); file << "sentinel"; }
        bb::SaveDataManager failed;
        CHECK(!failed.initialize(blocked.string(), test_quota, nullptr));
        CHECK(!failed.is_initialized());
        CHECK(failed.get_base_directory().empty());
        CHECK(std::filesystem::is_regular_file(blocked / "savedata" / "_work"));
    }
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

    // Ponteiro nulo com tamanho positivo não pode truncar nem substituir o save.
    for (bool atomic : {true, false}) {
        CHECK(mgr.write_file("/savedata0", "userdata0010", nullptr, save_content_v1.size(), atomic) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(mgr.read_file("/savedata0", "userdata0010", read_buf) == SCE_SAVEDATA_OK);
        CHECK(std::string(reinterpret_cast<char *>(read_buf.data()), read_buf.size()) == save_content_v1);
        CHECK(mgr.write_file("/savedata0", "absent.bin", nullptr, 1, atomic) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(!mgr.file_exists("/savedata0", "absent.bin"));
    }
    CHECK(mgr.write_file("/savedata0", "empty.bin", nullptr, 0) == SCE_SAVEDATA_OK);
    CHECK(mgr.read_file("/savedata0", "empty.bin", read_buf) == SCE_SAVEDATA_OK);
    CHECK(read_buf.empty());

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

    // Falhas deterministicas de filesystem: diretório no lugar do arquivo de metadados
    // e metadados inacessíveis durante a exclusão não podem gerar sucesso falso.
    {
        SaveDataManager failure_sm;
        const auto failure_root = sandbox / "failure_cases";
        CHECK(failure_sm.initialize(failure_root.string(), test_quota, nullptr));
        std::string pt;
        uint32_t status = 0;
        CHECK(failure_sm.mount(0, "CUSA03173", "SPRJ0090", SCE_SAVEDATA_MOUNT_MODE_CREATE2,
                               64, pt, status) == SCE_SAVEDATA_OK);
        const auto meta = failure_root / "savedata" / "0" / "CUSA03173" / "SPRJ0090.sce_sys";
        SceSaveDataParam test_param{};
        CHECK(failure_sm.set_param(pt, 0, &test_param, sizeof(test_param)) == SCE_SAVEDATA_OK);
        CHECK(failure_sm.umount(pt, true) == SCE_SAVEDATA_OK);
        CHECK(failure_sm.mount(0, "CUSA03173", "SPRJ0090", SCE_SAVEDATA_MOUNT_MODE_RDONLY,
                               64, pt, status) == SCE_SAVEDATA_OK);
        const char icon[] = "fixture";
        CHECK(failure_sm.set_param(pt, 0, &test_param, sizeof(test_param)) != SCE_SAVEDATA_OK);
        CHECK(failure_sm.save_icon(pt, icon, sizeof(icon)) != SCE_SAVEDATA_OK);
        CHECK(failure_sm.umount(pt, false) == SCE_SAVEDATA_OK);

        CHECK(std::filesystem::remove(meta / "param.bin"));
        CHECK(std::filesystem::create_directory(meta / "param.bin"));
        CHECK(failure_sm.mount(0, "CUSA03173", "SPRJ0090", SCE_SAVEDATA_MOUNT_MODE_RDWR,
                               64, pt, status) == SCE_SAVEDATA_OK);
        CHECK(failure_sm.set_param(pt, 0, &test_param, sizeof(test_param)) == SCE_SAVEDATA_ERROR_INTERNAL);
        CHECK(failure_sm.umount(pt, false) == SCE_SAVEDATA_OK);
        const auto saved = failure_root / "savedata" / "0" / "CUSA03173" / "SPRJ0090";
        for (int fail_exists : {1, 2}) {
            FailingDeleteFilesystem fs;
            fs.fail_exists_at = fail_exists;
            failure_sm.set_delete_filesystem_for_testing(&fs);
            CHECK(failure_sm.delete_save(0, "CUSA03173", "SPRJ0090") == SCE_SAVEDATA_ERROR_INTERNAL);
            CHECK(fs.remove_calls == 0);
            CHECK(std::filesystem::exists(saved));
            failure_sm.set_delete_filesystem_for_testing(nullptr);
        }
        for (int fail_remove : {1, 2}) {
            FailingDeleteFilesystem fs;
            fs.fail_remove_at = fail_remove;
            failure_sm.set_delete_filesystem_for_testing(&fs);
            CHECK(failure_sm.delete_save(0, "CUSA03173", "SPRJ0090") == SCE_SAVEDATA_ERROR_INTERNAL);
            CHECK(fs.remove_calls == fail_remove);
            failure_sm.set_delete_filesystem_for_testing(nullptr);
            // T148: Rollback transacional assegura que 'saved' existe em ambos os casos de falha!
            CHECK(std::filesystem::exists(saved));
            CHECK(std::filesystem::exists(meta));
        }
        CHECK(failure_sm.delete_save(0, "CUSA03173", "SPRJ0090") == SCE_SAVEDATA_OK);
        failure_sm.terminate();
    }

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

    // Montar e recarregar sem opt-in preserva cada byte, inclusive em RDONLY.
    for (uint32_t mode : {SCE_SAVEDATA_MOUNT_MODE_RDWR, SCE_SAVEDATA_MOUNT_MODE_RDONLY}) {
        mount_req.mode = mode;
        CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);
        CHECK(mgr.read_file("/savedata0", "userdata0010", read_buf) == SCE_SAVEDATA_OK);
        CHECK(read_buf == std::vector<uint8_t>(bb_sound_data.begin(), bb_sound_data.end()));
        CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) == SCE_SAVEDATA_OK);
    }
    // Funcionalidade anterior ainda pode ser solicitada explicitamente em montagem gravável.
    mgr.set_sound_hack_enabled(true);
    mount_req.mode = SCE_SAVEDATA_MOUNT_MODE_RDWR;
    CHECK(sceSaveDataMount(&mount_req, &mount_res) == SCE_SAVEDATA_OK);
    CHECK(mgr.read_file("/savedata0", "userdata0010", read_buf) == SCE_SAVEDATA_OK);
    CHECK(read_buf.size() == bb_sound_data.size());
    CHECK(read_buf[0x204E] == 1);
    CHECK(sceSaveDataUmount(&mount_res.point, SCE_SAVEDATA_UMOUNT_FLAG_COMMIT) == SCE_SAVEDATA_OK);
    mgr.set_sound_hack_enabled(false);

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

    // ========================================================================
    // 13. Convencao de Documents/savedata/ e resolve_savedata_root (T054)
    // ========================================================================
    {
        SaveDataManager custom_sm;
        // Caminho sem 'savedata' no final -> anexa 'savedata'
        custom_sm.set_base_directory((sandbox / "Documents").string());
        CHECK(custom_sm.resolve_savedata_root() == (sandbox / "Documents" / "savedata"));

        // Caminho ja terminando em 'savedata' -> nao duplica
        custom_sm.set_base_directory((sandbox / "Documents" / "savedata").string());
        CHECK(custom_sm.resolve_savedata_root() == (sandbox / "Documents" / "savedata"));

        // Caminho com barra final 'savedata/' -> nao duplica
        custom_sm.set_base_directory((sandbox / "Documents" / "savedata/").string());
        CHECK(custom_sm.resolve_savedata_root() == (sandbox / "Documents" / "savedata"));

        // Teste de busca, montagem, gravacao e recarga no caminho real
        std::filesystem::path real_doc_save = sandbox / "Documents" / "savedata";
        std::filesystem::remove_all(sandbox / "Documents", ec);
        CHECK(custom_sm.initialize(real_doc_save.string(), 32 * 1024 * 1024, nullptr));

        std::string m_pt;
        uint32_t m_stat = 0;
        CHECK(custom_sm.mount(0, "CUSA03173", "SPRJ0010", SCE_SAVEDATA_MOUNT_MODE_CREATE2, 32768, m_pt, m_stat) == SCE_SAVEDATA_OK);

        const std::string payload = "Hunter Progress Level 99 Blood Echoes 500000";
        CHECK(custom_sm.write_file(m_pt, "userdata0010", payload.data(), payload.size()) == SCE_SAVEDATA_OK);
        CHECK(custom_sm.file_exists(m_pt, "userdata0010"));

        SceSaveDataParam meta_param{};
        std::strncpy(meta_param.title, "Yharnam Sunrise", sizeof(meta_param.title) - 1);
        meta_param.user_param = 999;
        CHECK(custom_sm.set_param(m_pt, 0, &meta_param, sizeof(meta_param)) == SCE_SAVEDATA_OK);

        CHECK(custom_sm.umount(m_pt, true) == SCE_SAVEDATA_OK);

        // Busca o slot recem-persistido
        std::vector<SaveSearchEntry> found_entries;
        CHECK(custom_sm.dir_name_search(0, "CUSA03173", "%", SCE_SAVEDATA_SORT_KEY_MTIME, SCE_SAVEDATA_SORT_ORDER_DESC, found_entries) == SCE_SAVEDATA_OK);
        CHECK(!found_entries.empty());
        CHECK(found_entries[0].dir_name == "SPRJ0010");
        CHECK(std::string(found_entries[0].param.title) == "Yharnam Sunrise");

        // Recarga / remontagem do slot encontrado
        std::string reload_pt;
        uint32_t reload_stat = 0;
        CHECK(custom_sm.mount(0, "CUSA03173", found_entries[0].dir_name.c_str(), SCE_SAVEDATA_MOUNT_MODE_RDWR, 32768, reload_pt, reload_stat) == SCE_SAVEDATA_OK);
        CHECK(reload_stat == SCE_SAVEDATA_MOUNT_STATUS_EXISTING);

        std::vector<uint8_t> reloaded_data;
        CHECK(custom_sm.read_file(reload_pt, "userdata0010", reloaded_data) == SCE_SAVEDATA_OK);
        CHECK(std::string(reinterpret_cast<char *>(reloaded_data.data()), reloaded_data.size()) == payload);

        CHECK(custom_sm.umount(reload_pt, false) == SCE_SAVEDATA_OK);
        CHECK(custom_sm.terminate());
    }

    // T146: termination must leave a failed commit mounted and recoverable.
    {
        SaveDataManager sm;
        const auto root = sandbox / "terminate_failure";
        CHECK(sm.initialize(root.string(), 1024, nullptr));
        std::string pt;
        uint32_t status = 0;
        CHECK(sm.mount(0, "CUSA03173", "SPRJ0011", SCE_SAVEDATA_MOUNT_MODE_CREATE2,
                       1024, pt, status) == SCE_SAVEDATA_OK);
        const std::string payload = "keep pending changes";
        CHECK(sm.write_file(pt, "userdata0010", payload.data(), payload.size()) == SCE_SAVEDATA_OK);
        sm.set_test_write_failure_phase(SaveDataManager::WriteFailurePhase::MetadataUpdate);
        CHECK(!sm.terminate());
        CHECK(sm.is_initialized());
        std::vector<uint8_t> pending;
        CHECK(sm.read_file(pt, "userdata0010", pending) == SCE_SAVEDATA_OK);
        CHECK(std::string(pending.begin(), pending.end()) == payload);
        sm.set_test_write_failure_phase(SaveDataManager::WriteFailurePhase::None);
        CHECK(sm.terminate());
        SaveDataManager restarted;
        CHECK(restarted.initialize(root.string(), 1024, nullptr));
        CHECK(restarted.mount(0, "CUSA03173", "SPRJ0011", SCE_SAVEDATA_MOUNT_MODE_RDWR,
                              1024, pt, status) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> persisted;
        CHECK(restarted.read_file(pt, "userdata0010", persisted) == SCE_SAVEDATA_OK);
        CHECK(persisted == pending);
        CHECK(restarted.terminate());
    }

    // T146: failures at quota, commit and checksum must retain staged bytes.
    for (auto phase : {SaveDataManager::WriteFailurePhase::TmpWrite,
                       SaveDataManager::WriteFailurePhase::Checksum,
                       SaveDataManager::WriteFailurePhase::Backup,
                       SaveDataManager::WriteFailurePhase::Replacement}) {
        SaveDataManager sm;
        const auto root = sandbox / ("terminate_phase_" + std::to_string(static_cast<int>(phase)));
        CHECK(sm.initialize(root.string(), 4096, nullptr));
        std::string pt;
        uint32_t status = 0;
        CHECK(sm.mount(0, "CUSA03173", "SPRJ0012", SCE_SAVEDATA_MOUNT_MODE_CREATE2,
                       4096, pt, status) == SCE_SAVEDATA_OK);
        const std::string previous = "previous";
        CHECK(sm.write_file(pt, "userdata0010", previous.data(), previous.size()) == SCE_SAVEDATA_OK);
        CHECK(sm.umount(pt, true) == SCE_SAVEDATA_OK);
        CHECK(sm.mount(0, "CUSA03173", "SPRJ0012", SCE_SAVEDATA_MOUNT_MODE_RDWR,
                       4096, pt, status) == SCE_SAVEDATA_OK);
        const std::string changed = "pending change";
        CHECK(sm.write_file(pt, "userdata0010", changed.data(), changed.size()) == SCE_SAVEDATA_OK);
        sm.set_test_write_failure_phase(phase);
        CHECK(!sm.terminate());
        CHECK(sm.is_initialized());
        std::vector<uint8_t> pending;
        CHECK(sm.read_file(pt, "userdata0010", pending) == SCE_SAVEDATA_OK);
        CHECK(std::string(pending.begin(), pending.end()) == changed);
        sm.set_test_write_failure_phase(SaveDataManager::WriteFailurePhase::None);
        CHECK(sm.terminate());
        SaveDataManager restarted;
        CHECK(restarted.initialize(root.string(), 4096, nullptr));
        CHECK(restarted.mount(0, "CUSA03173", "SPRJ0012", SCE_SAVEDATA_MOUNT_MODE_RDWR,
                                4096, pt, status) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> persisted;
        CHECK(restarted.read_file(pt, "userdata0010", persisted) == SCE_SAVEDATA_OK);
        CHECK(persisted == pending);
        CHECK(restarted.terminate());
    }
    {
        SaveDataManager sm;
        const auto root = sandbox / "terminate_quota";
        CHECK(sm.initialize(root.string(), 4096, nullptr));
        std::string pt;
        uint32_t status = 0;
        CHECK(sm.mount(0, "CUSA03173", "SPRJ0013", SCE_SAVEDATA_MOUNT_MODE_CREATE2,
                       4096, pt, status) == SCE_SAVEDATA_OK);
        const std::string changed = "pending after quota";
        CHECK(sm.write_file(pt, "userdata0010", changed.data(), changed.size()) == SCE_SAVEDATA_OK);
        sm.set_max_quota_per_slot(1);
        CHECK(!sm.terminate());
        CHECK(sm.is_initialized());
        std::vector<uint8_t> pending;
        CHECK(sm.read_file(pt, "userdata0010", pending) == SCE_SAVEDATA_OK);
        CHECK(std::string(pending.begin(), pending.end()) == changed);
        sm.set_max_quota_per_slot(4096);
        CHECK(sm.terminate());
    }

    // ========================================================================
    // 14. Confinamento Estrito de Slot e Protecao contra Symlinks (T055)
    // ========================================================================
    {
        SaveDataManager sec_sm;
        std::filesystem::path sec_dir = sandbox / "sec_savedata";
        std::filesystem::remove_all(sec_dir, ec);
        CHECK(sec_sm.initialize(sec_dir.string(), 16 * 1024 * 1024, nullptr));

        std::string sec_pt;
        uint32_t sec_stat = 0;
        CHECK(sec_sm.mount(0, "CUSA03173", "SPRJ0020", SCE_SAVEDATA_MOUNT_MODE_CREATE2, 16384, sec_pt, sec_stat) == SCE_SAVEDATA_OK);

        // Arquivo valido dentro do slot
        const std::string valid_data = "Valid Slot Data Content";
        CHECK(sec_sm.write_file(sec_pt, "slot_file.dat", valid_data.data(), valid_data.size()) == SCE_SAVEDATA_OK);
        CHECK(sec_sm.file_exists(sec_pt, "slot_file.dat"));

        std::vector<uint8_t> valid_read;
        CHECK(sec_sm.read_file(sec_pt, "slot_file.dat", valid_read) == SCE_SAVEDATA_OK);
        CHECK(std::string(reinterpret_cast<char *>(valid_read.data()), valid_read.size()) == valid_data);

        // 1. Rejeicao de tentativas com path traversal no read, write e remove
        CHECK(sec_sm.write_file(sec_pt, "../outside.bin", "hack", 4) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(sec_sm.write_file(sec_pt, "..\\outside.bin", "hack", 4) == SCE_SAVEDATA_ERROR_PARAMETER);
        std::vector<uint8_t> out_read;
        CHECK(sec_sm.read_file(sec_pt, "../outside.bin", out_read) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(sec_sm.read_file(sec_pt, "..\\outside.bin", out_read) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(sec_sm.remove_file(sec_pt, "../outside.bin") == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(!sec_sm.file_exists(sec_pt, "../outside.bin"));

        // 2. Rejeicao de caminhos absolutos
        std::filesystem::path abs_path = sandbox / "secret_host_file.txt";
        {
            std::ofstream f(abs_path);
            f << "Host confidential credentials";
        }
        CHECK(sec_sm.write_file(sec_pt, abs_path.string(), "hack", 4) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(sec_sm.read_file(sec_pt, abs_path.string(), out_read) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(sec_sm.remove_file(sec_pt, abs_path.string()) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(!sec_sm.file_exists(sec_pt, abs_path.string()));

        // 3. Rejeicao de symlink apontando para fora do slot
        const SaveSlotInfo *slot_info = sec_sm.get_slot(sec_sm.find_slot(sec_pt));
        CHECK(slot_info != nullptr);
        std::filesystem::path symlink_inside_work = std::filesystem::path(slot_info->work_path) / "symlink_escape";

        std::error_code sym_ec;
        std::filesystem::create_symlink(abs_path, symlink_inside_work, sym_ec);
        if (!sym_ec) {
            // Symlink foi criado com sucesso: verifica que as APIs de save rejeitam operacoes sobre ele
            CHECK(sec_sm.read_file(sec_pt, "symlink_escape", out_read) == SCE_SAVEDATA_ERROR_PARAMETER);
            CHECK(sec_sm.write_file(sec_pt, "symlink_escape", "attack", 6) == SCE_SAVEDATA_ERROR_PARAMETER);
            CHECK(sec_sm.remove_file(sec_pt, "symlink_escape") == SCE_SAVEDATA_ERROR_PARAMETER);
            CHECK(!sec_sm.file_exists(sec_pt, "symlink_escape"));

            // Confirma que o arquivo externo alvo nao foi corrompido nem alterado
            std::ifstream check_host(abs_path);
            std::string host_str;
            std::getline(check_host, host_str);
            CHECK(host_str == "Host confidential credentials");
        }

        // 4. Confirma que os dados validos do slot nao foram degradados
        CHECK(sec_sm.read_file(sec_pt, "slot_file.dat", valid_read) == SCE_SAVEDATA_OK);
        CHECK(std::string(reinterpret_cast<char *>(valid_read.data()), valid_read.size()) == valid_data);

        // Remocao legitima de arquivo dentro do slot
        CHECK(sec_sm.remove_file(sec_pt, "slot_file.dat") == SCE_SAVEDATA_OK);
        CHECK(!sec_sm.file_exists(sec_pt, "slot_file.dat"));

        CHECK(sec_sm.umount(sec_pt, false) == SCE_SAVEDATA_OK);
        CHECK(sec_sm.terminate());
    }

    // ========================================================================
    // 15. Segregacao por usuario da memoria de save e persistencia (T086 / FR-008)
    // ========================================================================
    {
        SaveDataManager mem_sm;
        std::filesystem::path mem_sandbox = sandbox / "user_memory_test";
        std::filesystem::remove_all(mem_sandbox, ec);
        CHECK(mem_sm.initialize(mem_sandbox.string(), 16 * 1024 * 1024, nullptr));

        // 1. Rejeição de usuário negativo
        std::vector<uint8_t> dummy_buf(64, 0);
        CHECK(mem_sm.get_memory(-1, dummy_buf.data(), 64, 0) == SCE_SAVEDATA_ERROR_INVALID_USER);
        CHECK(mem_sm.set_memory(-1, dummy_buf.data(), 64, 0) == SCE_SAVEDATA_ERROR_INVALID_USER);
        CHECK(mem_sm.setup_memory(-1, 64, nullptr) == SCE_SAVEDATA_ERROR_INVALID_USER);

        // 2. Consulta de usuário sem memória configurada
        CHECK(mem_sm.get_memory(1, dummy_buf.data(), 64, 0) == SCE_SAVEDATA_ERROR_MEMORY_NOT_READY);
        CHECK(mem_sm.set_memory(1, dummy_buf.data(), 64, 0) == SCE_SAVEDATA_ERROR_MEMORY_NOT_READY);

        // 3. Configuração de memórias independentes para Usuário 1 (128 bytes) e Usuário 2 (256 bytes)
        CHECK(mem_sm.setup_memory(1, 128, nullptr) == SCE_SAVEDATA_OK);
        CHECK(mem_sm.setup_memory(2, 256, nullptr) == SCE_SAVEDATA_OK);

        // Usuário 3 continua sem memória
        CHECK(mem_sm.get_memory(3, dummy_buf.data(), 64, 0) == SCE_SAVEDATA_ERROR_MEMORY_NOT_READY);

        // 4. Gravação e leitura isoladas (alternância sem vazamento cruzado)
        std::vector<uint8_t> u1_write(128, 0x11);
        std::vector<uint8_t> u2_write(256, 0x22);

        CHECK(mem_sm.set_memory(1, u1_write.data(), 128, 0) == SCE_SAVEDATA_OK);
        CHECK(mem_sm.set_memory(2, u2_write.data(), 256, 0) == SCE_SAVEDATA_OK);

        // Usuário 1 não pode escrever ou ler além da sua cota alocada (128 bytes)
        CHECK(mem_sm.set_memory(1, u1_write.data(), 64, 100) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(mem_sm.get_memory(1, dummy_buf.data(), 64, 100) == SCE_SAVEDATA_ERROR_PARAMETER);

        // Leitura e verificação de dados do Usuário 1
        std::vector<uint8_t> u1_read(128, 0);
        CHECK(mem_sm.get_memory(1, u1_read.data(), 128, 0) == SCE_SAVEDATA_OK);
        CHECK(u1_read == u1_write);

        // Leitura e verificação de dados do Usuário 2 (256 bytes)
        std::vector<uint8_t> u2_read(256, 0);
        CHECK(mem_sm.get_memory(2, u2_read.data(), 256, 0) == SCE_SAVEDATA_OK);
        CHECK(u2_read == u2_write);

        // 5. Persistência após reinicialização do SaveDataManager
        CHECK(mem_sm.terminate());

        SaveDataManager reloaded_mem_sm;
        CHECK(reloaded_mem_sm.initialize(mem_sandbox.string(), 16 * 1024 * 1024, nullptr));

        // Usuário 1 e Usuário 2 devem conseguir ler seus dados persistidos sem chamada explícita a setup_memory
        std::vector<uint8_t> u1_reloaded(128, 0);
        CHECK(reloaded_mem_sm.get_memory(1, u1_reloaded.data(), 128, 0) == SCE_SAVEDATA_OK);
        CHECK(u1_reloaded == u1_write);

        std::vector<uint8_t> u2_reloaded(256, 0);
        CHECK(reloaded_mem_sm.get_memory(2, u2_reloaded.data(), 256, 0) == SCE_SAVEDATA_OK);
        CHECK(u2_reloaded == u2_write);

        // Usuário 3 continua sem memória mesmo após reinicialização
        CHECK(reloaded_mem_sm.get_memory(3, dummy_buf.data(), 64, 0) == SCE_SAVEDATA_ERROR_MEMORY_NOT_READY);

        CHECK(reloaded_mem_sm.terminate());
        std::filesystem::remove_all(mem_sandbox, ec);
    }

    // ========================================================================
    // 16. T120: Validacao de limites de SaveDataMemory sem overflow (FR-008/FR-009)
    // ========================================================================
    {
        SaveDataManager of_sm;
        std::filesystem::path of_sandbox = sandbox / "overflow_test";
        std::filesystem::remove_all(of_sandbox, ec);
        CHECK(of_sm.initialize(of_sandbox.string(), 16 * 1024 * 1024, nullptr));
        CHECK(of_sm.setup_memory(1, 1024, nullptr) == SCE_SAVEDATA_OK);

        std::vector<uint8_t> dummy_buf(64, 0);
        // Offset negativo
        CHECK(of_sm.get_memory(1, dummy_buf.data(), 64, -1) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(of_sm.set_memory(1, dummy_buf.data(), 64, -1) == SCE_SAVEDATA_ERROR_PARAMETER);

        // Tamanho maior que max streamsize
        CHECK(of_sm.get_memory(1, dummy_buf.data(), UINT64_MAX, 0) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(of_sm.set_memory(1, dummy_buf.data(), UINT64_MAX, 0) == SCE_SAVEDATA_ERROR_PARAMETER);

        // Offset adversarial com overflow de 64 bits (offset + size transborda)
        CHECK(of_sm.get_memory(1, dummy_buf.data(), 100, INT64_MAX) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(of_sm.set_memory(1, dummy_buf.data(), 100, INT64_MAX) == SCE_SAVEDATA_ERROR_PARAMETER);

        // Offset além da capacidade
        CHECK(of_sm.get_memory(1, dummy_buf.data(), 10, 1025) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(of_sm.set_memory(1, dummy_buf.data(), 10, 1025) == SCE_SAVEDATA_ERROR_PARAMETER);

        // Offset + size além da capacidade
        CHECK(of_sm.get_memory(1, dummy_buf.data(), 30, 1000) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(of_sm.set_memory(1, dummy_buf.data(), 30, 1000) == SCE_SAVEDATA_ERROR_PARAMETER);

        // Buffer nulo com tamanho > 0
        CHECK(of_sm.get_memory(1, nullptr, 10, 0) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(of_sm.set_memory(1, nullptr, 10, 0) == SCE_SAVEDATA_ERROR_PARAMETER);

        // Operação válida dentro dos limites
        CHECK(of_sm.set_memory(1, dummy_buf.data(), 64, 0) == SCE_SAVEDATA_OK);
        CHECK(of_sm.get_memory(1, dummy_buf.data(), 64, 0) == SCE_SAVEDATA_OK);

        CHECK(of_sm.terminate());
        std::filesystem::remove_all(of_sandbox, ec);
    }

    // ========================================================================
    // 17. T122: Deteccao de corrupcao e integridade SHA-256 entre sessoes
    // ========================================================================
    {
        SaveDataManager sha_sm;
        std::filesystem::path sha_sandbox = sandbox / "sha256_test";
        std::filesystem::remove_all(sha_sandbox, ec);
        CHECK(sha_sm.initialize(sha_sandbox.string(), 16 * 1024 * 1024, nullptr));

        std::string pt;
        uint32_t stat = 0;
        CHECK(sha_sm.mount(0, "CUSA03173", "SPRJ0050", SCE_SAVEDATA_MOUNT_MODE_CREATE2, 16384, pt, stat) == SCE_SAVEDATA_OK);

        const std::string golden_data = "AUTHENTIC_HUNTER_SAVE_DATA_SHA256";
        CHECK(sha_sm.write_file(pt, "userdata0010", golden_data.data(), golden_data.size()) == SCE_SAVEDATA_OK);
        CHECK(sha_sm.umount(pt, true) == SCE_SAVEDATA_OK);

        // Confirma que checksum.sha256 foi gerado
        std::filesystem::path chk_path = sha_sandbox / "savedata" / "0" / "CUSA03173" / "SPRJ0050.sce_sys" / "checksum.sha256";
        CHECK(std::filesystem::exists(chk_path));
        CHECK(std::filesystem::file_size(chk_path) >= 64);

        // Remontagem legítima bem-sucedida
        CHECK(sha_sm.mount(0, "CUSA03173", "SPRJ0050", SCE_SAVEDATA_MOUNT_MODE_RDWR, 16384, pt, stat) == SCE_SAVEDATA_OK);
        CHECK(sha_sm.umount(pt, false) == SCE_SAVEDATA_OK);

        // Corrupção física no arquivo host
        std::filesystem::path host_save = sha_sandbox / "savedata" / "0" / "CUSA03173" / "SPRJ0050" / "userdata0010";
        CHECK(std::filesystem::exists(host_save));
        {
            std::fstream fs(host_save, std::ios::in | std::ios::out | std::ios::binary);
            CHECK(fs.is_open());
            char corrupt_byte = 'X';
            fs.seekp(5);
            fs.write(&corrupt_byte, 1);
        }

        // Tentativa de montagem após corrupção DEVE falhar com SCE_SAVEDATA_ERROR_BROKEN
        CHECK(sha_sm.mount(0, "CUSA03173", "SPRJ0050", SCE_SAVEDATA_MOUNT_MODE_RDWR, 16384, pt, stat) == SCE_SAVEDATA_ERROR_BROKEN);

        // Restaura dado autêntico e confirma que montagem volta a ter sucesso
        {
            std::ofstream fs(host_save, std::ios::binary | std::ios::trunc);
            fs.write(golden_data.data(), golden_data.size());
        }
        CHECK(sha_sm.mount(0, "CUSA03173", "SPRJ0050", SCE_SAVEDATA_MOUNT_MODE_RDWR, 16384, pt, stat) == SCE_SAVEDATA_OK);
        CHECK(sha_sm.umount(pt, false) == SCE_SAVEDATA_OK);

        CHECK(sha_sm.terminate());
        std::filesystem::remove_all(sha_sandbox, ec);
    }

    // ========================================================================
    // 18. T139: Rollback de Metadados e Slots Incompletos no Umount com DISCARD
    // ========================================================================
    {
        SaveDataManager rb_sm;
        std::filesystem::path rb_sandbox = sandbox / "rollback_test";
        std::filesystem::remove_all(rb_sandbox, ec);
        CHECK(rb_sm.initialize(rb_sandbox.string(), 16 * 1024 * 1024, nullptr));

        std::string pt;
        uint32_t stat = 0;
        CHECK(rb_sm.mount(0, "CUSA03173", "SPRJ0060", SCE_SAVEDATA_MOUNT_MODE_CREATE2, 16384, pt, stat) == SCE_SAVEDATA_OK);

        SceSaveDataParam p_orig{};
        std::strncpy(p_orig.title, "Original Title", sizeof(p_orig.title) - 1);
        p_orig.user_param = 111;
        CHECK(rb_sm.set_param(pt, 0, &p_orig, sizeof(p_orig)) == SCE_SAVEDATA_OK);
        const char orig_icon[] = "ORIGINAL_ICON_DATA";
        CHECK(rb_sm.save_icon(pt, orig_icon, sizeof(orig_icon)) == SCE_SAVEDATA_OK);
        CHECK(rb_sm.umount(pt, true) == SCE_SAVEDATA_OK);

        // Remonta em modo RDWR
        CHECK(rb_sm.mount(0, "CUSA03173", "SPRJ0060", SCE_SAVEDATA_MOUNT_MODE_RDWR, 16384, pt, stat) == SCE_SAVEDATA_OK);
        SceSaveDataParam p_mod{};
        std::strncpy(p_mod.title, "Modified Title Never Saved", sizeof(p_mod.title) - 1);
        p_mod.user_param = 999;
        CHECK(rb_sm.set_param(pt, 0, &p_mod, sizeof(p_mod)) == SCE_SAVEDATA_OK);
        const char mod_icon[] = "MODIFIED_ICON_DATA";
        CHECK(rb_sm.save_icon(pt, mod_icon, sizeof(mod_icon)) == SCE_SAVEDATA_OK);

        // Desmonta com DISCARD (commit=false)
        CHECK(rb_sm.umount(pt, false) == SCE_SAVEDATA_OK);

        // Verifica que os metadados persistidos em disco não foram alterados
        std::filesystem::path meta_dir = rb_sandbox / "savedata" / "0" / "CUSA03173" / "SPRJ0060.sce_sys";
        std::ifstream param_in(meta_dir / "param.bin", std::ios::binary);
        SceSaveDataParam check_param{};
        param_in.read(reinterpret_cast<char*>(&check_param), sizeof(check_param));
        CHECK(std::string(check_param.title) == "Original Title");
        CHECK(check_param.user_param == 111);

        std::ifstream icon_in(meta_dir / "icon0.png", std::ios::binary);
        std::string check_icon((std::istreambuf_iterator<char>(icon_in)), std::istreambuf_iterator<char>());
        CHECK(check_icon == std::string(orig_icon, sizeof(orig_icon)));

        // Rollback de slot criado: se criado e desmontado com DISCARD, deve ser excluído
        std::string pt_new;
        CHECK(rb_sm.mount(0, "CUSA03173", "SPRJ0061", SCE_SAVEDATA_MOUNT_MODE_CREATE2, 16384, pt_new, stat) == SCE_SAVEDATA_OK);
        CHECK(rb_sm.write_file(pt_new, "discard_me.dat", "temp", 4) == SCE_SAVEDATA_OK);
        CHECK(rb_sm.umount(pt_new, false) == SCE_SAVEDATA_OK);

        std::filesystem::path slot61_dir = rb_sandbox / "savedata" / "0" / "CUSA03173" / "SPRJ0061";
        std::filesystem::path slot61_meta = rb_sandbox / "savedata" / "0" / "CUSA03173" / "SPRJ0061.sce_sys";
        CHECK(!std::filesystem::exists(slot61_dir));
        CHECK(!std::filesystem::exists(slot61_meta));

        CHECK(rb_sm.terminate());
        std::filesystem::remove_all(rb_sandbox, ec);
    }

    // ========================================================================
    // 19. T128: Confinamento Canonico Estrito (is_confined_to_savedata_root)
    // ========================================================================
    {
        SaveDataManager conf_sm;
        std::filesystem::path conf_sandbox = sandbox / "confinement_test";
        std::filesystem::remove_all(conf_sandbox, ec);
        CHECK(conf_sm.initialize(conf_sandbox.string(), 16 * 1024 * 1024, nullptr));

        const auto root = conf_sm.resolve_savedata_root();
        const auto valid_slot = root / "0" / "CUSA03173" / "SPRJ0001";
        std::filesystem::create_directories(valid_slot);
        CHECK(conf_sm.is_confined_to_savedata_root(valid_slot));

        const auto escaped_slot = root / ".." / "outside_dir";
        std::filesystem::create_directories(escaped_slot);
        CHECK(!conf_sm.is_confined_to_savedata_root(escaped_slot));

        // Rejeição de symlink apontando para fora do root
        std::filesystem::path outside_target = conf_sandbox / "outside_target";
        std::filesystem::create_directories(outside_target);
        std::filesystem::path symlink_slot = root / "0" / "CUSA03173" / "symlink_escape";
        std::filesystem::create_symlink(outside_target, symlink_slot, ec);
        if (!ec) {
            CHECK(!conf_sm.is_confined_to_savedata_root(symlink_slot));
        }

        CHECK(conf_sm.terminate());
        std::filesystem::remove_all(conf_sandbox, ec);
    }

    // ========================================================================
    // 20. T127: Recuperacao de Falhas e Limpeza (recover_interrupted_commits)
    // ========================================================================
    {
        SaveDataManager rec_sm;
        std::filesystem::path rec_sandbox = sandbox / "recovery_test";
        std::filesystem::remove_all(rec_sandbox, ec);
        const auto rec_root = rec_sandbox / "savedata" / "0" / "CUSA03173";
        std::filesystem::create_directories(rec_root);

        // Cenário a: commit interrompido deixou SPRJ0070.bak mas SPRJ0070 não existe
        const auto bak_dir = rec_root / "SPRJ0070.bak";
        std::filesystem::create_directories(bak_dir);
        {
            std::ofstream f(bak_dir / "userdata0010", std::ios::binary);
            f.write("RECOVERED_SLOT_CONTENT", 22);
        }

        // Cenário b: commit incompleto deixou SPRJ0070.tmp
        const auto tmp_dir = rec_root / "SPRJ0070.tmp";
        std::filesystem::create_directories(tmp_dir);
        {
            std::ofstream f(tmp_dir / "partial.dat", std::ios::binary);
            f.write("PARTIAL_DATA", 12);
        }

        // Cenário c: arquivo tmp isolado SPRJ0071.tmp
        const auto tmp_dir2 = rec_root / "SPRJ0071.tmp";
        std::filesystem::create_directories(tmp_dir2);

        // Inicializa o manager (que invoca recover_interrupted_commits)
        CHECK(rec_sm.initialize(rec_sandbox.string(), 16 * 1024 * 1024, nullptr));

        // SPRJ0070 deve ter sido restaurado a partir de SPRJ0070.bak
        const auto recovered_slot = rec_root / "SPRJ0070";
        CHECK(std::filesystem::exists(recovered_slot));
        CHECK(std::filesystem::exists(recovered_slot / "userdata0010"));
        CHECK(!std::filesystem::exists(bak_dir));
        CHECK(!std::filesystem::exists(tmp_dir));
        CHECK(!std::filesystem::exists(tmp_dir2));

        // Confirma leitura normal do slot recuperado via mount
        std::string pt;
        uint32_t stat = 0;
        CHECK(rec_sm.mount(0, "CUSA03173", "SPRJ0070", SCE_SAVEDATA_MOUNT_MODE_RDONLY, 16384, pt, stat) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> rec_data;
        CHECK(rec_sm.read_file(pt, "userdata0010", rec_data) == SCE_SAVEDATA_OK);
        CHECK(std::string(reinterpret_cast<char*>(rec_data.data()), rec_data.size()) == "RECOVERED_SLOT_CONTENT");
        CHECK(rec_sm.umount(pt, false) == SCE_SAVEDATA_OK);

        CHECK(rec_sm.terminate());
        std::filesystem::remove_all(rec_sandbox, ec);
    }

    // ========================================================================
    // T146: Recuperacao de Metadados Corrompidos (.sce_sys.bak) e Convivencia
    // de _work com Slots Ativos (Constituição I / US4/AC1)
    // ========================================================================
    {
        SaveDataManager t146_sm;
        std::filesystem::path t146_sandbox = sandbox / "t146_recovery_test";
        std::filesystem::remove_all(t146_sandbox, ec);
        const auto t146_root = t146_sandbox / "savedata" / "0" / "CUSA03173";
        std::filesystem::create_directories(t146_root);

        // 1. Cenário: .sce_sys incompleto (sem param.bin) mas com backup válido .sce_sys.bak
        const auto corrupt_meta = t146_root / "SPRJ0072.sce_sys";
        std::filesystem::create_directories(corrupt_meta);
        // Deixa sem param.bin e sem checksum.sha256 (corrupto/incompleto)

        const auto valid_meta_bak = t146_root / "SPRJ0072.sce_sys.bak.999999";
        std::filesystem::create_directories(valid_meta_bak);
        SceSaveDataParam p_bak{};
        std::strncpy(p_bak.title, "Recovered Title T146", sizeof(p_bak.title) - 1);
        p_bak.user_param = 777;
        {
            std::ofstream f(valid_meta_bak / "param.bin", std::ios::binary);
            f.write(reinterpret_cast<const char*>(&p_bak), sizeof(p_bak));
        }
        {
            std::ofstream f(valid_meta_bak / "checksum.sha256", std::ios::binary);
            f << "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
        }

        // Também cria a pasta de dados correspondente
        const auto data_dir = t146_root / "SPRJ0072";
        std::filesystem::create_directories(data_dir);
        {
            std::ofstream f(data_dir / "userdata0010", std::ios::binary);
            f << "DATA_CONTENT_T146";
        }

        // Inicializa o manager (que invoca recover_interrupted_commits)
        CHECK(t146_sm.initialize(t146_sandbox.string(), 16 * 1024 * 1024, nullptr));

        // Metadados devem ter sido restaurados a partir de .sce_sys.bak
        CHECK(std::filesystem::exists(corrupt_meta / "param.bin"));
        CHECK(std::filesystem::exists(corrupt_meta / "checksum.sha256"));
        CHECK(!std::filesystem::exists(valid_meta_bak));

        SceSaveDataParam restored_param{};
        {
            std::ifstream f(corrupt_meta / "param.bin", std::ios::binary);
            f.read(reinterpret_cast<char*>(&restored_param), sizeof(restored_param));
        }
        CHECK(std::string(restored_param.title) == "Recovered Title T146");
        CHECK(restored_param.user_param == 777);

        // 2. Cenário: Convivência com _work ativo.
        std::string pt;
        uint32_t status = 0;
        CHECK(t146_sm.mount(0, "CUSA03173", "SPRJ0073", SCE_SAVEDATA_MOUNT_MODE_CREATE2, 4096, pt, status) == SCE_SAVEDATA_OK);
        CHECK(t146_sm.write_file(pt, "active.dat", "ACTIVE_WORK_DATA", 16) == SCE_SAVEDATA_OK);

        // Cria manualmente uma pasta órfã no _work (ex: slot_orphaned de crash anterior)
        const auto orphaned_work = t146_sandbox / "savedata" / "_work" / "slot_orphaned_99";
        std::filesystem::create_directories(orphaned_work);
        {
            std::ofstream f(orphaned_work / "orphan.dat", std::ios::binary);
            f << "ORPHAN";
        }

        // Chama recover_interrupted_commits manualmente com slot ativo
        t146_sm.recover_interrupted_commits();

        // Pasta órfã deve ter sido removida, mas o work do slot ativo permanece intacto!
        CHECK(!std::filesystem::exists(orphaned_work));
        std::vector<uint8_t> active_read;
        CHECK(t146_sm.read_file(pt, "active.dat", active_read) == SCE_SAVEDATA_OK);
        CHECK(std::string(active_read.begin(), active_read.end()) == "ACTIVE_WORK_DATA");

        CHECK(t146_sm.umount(pt, true) == SCE_SAVEDATA_OK);
        CHECK(t146_sm.terminate());
        std::filesystem::remove_all(t146_sandbox, ec);
    }

    // ========================================================================
    // 21. T147/T156: Preservacao byte a byte de memory.dat em setup_memory
    // e Confinamento Canonico de SaveDataMemory (FR-008, Constituicao I)
    // ========================================================================
    {
        SaveDataManager mem_test_sm;
        std::filesystem::path mem_test_box = sandbox / "mem_preservation_test";
        std::filesystem::remove_all(mem_test_box, ec);
        CHECK(mem_test_sm.initialize(mem_test_box.string(), 16 * 1024 * 1024, nullptr));

        std::vector<uint8_t> dummy_buf(64, 0);

        // 1. T156: Rejeicao de default_title_id_ malformado ou com travessia
        mem_test_sm.set_default_title_id("../escape");
        CHECK(mem_test_sm.setup_memory(1, 128, nullptr) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(mem_test_sm.get_memory(1, dummy_buf.data(), 64, 0) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(mem_test_sm.set_memory(1, dummy_buf.data(), 64, 0) == SCE_SAVEDATA_ERROR_PARAMETER);

        mem_test_sm.set_default_title_id("CUSA03173/evil");
        CHECK(mem_test_sm.setup_memory(1, 128, nullptr) == SCE_SAVEDATA_ERROR_PARAMETER);

        // Restaura title_id valido
        mem_test_sm.set_default_title_id("CUSA03173");

        // 2. Setup inicial: 64 bytes
        CHECK(mem_test_sm.setup_memory(1, 64, nullptr) == SCE_SAVEDATA_OK);

        // Escreve dados autenticos conhecidos
        std::vector<uint8_t> original_mem_data(64);
        for (std::size_t i = 0; i < 64; ++i) {
            original_mem_data[i] = static_cast<uint8_t>(0xA0 + (i & 0x1F));
        }
        CHECK(mem_test_sm.set_memory(1, original_mem_data.data(), 64, 0) == SCE_SAVEDATA_OK);

        // 3. T147: Chamada subsequente a setup_memory com o MESMO tamanho NÃO deve truncar nem zerar
        CHECK(mem_test_sm.setup_memory(1, 64, nullptr) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> read_back(64, 0);
        CHECK(mem_test_sm.get_memory(1, read_back.data(), 64, 0) == SCE_SAVEDATA_OK);
        CHECK(read_back == original_mem_data);

        // 4. T147: Chamada subsequente com tamanho MAIOR (128 bytes):
        // Os primeiros 64 bytes DEVEM permanecer intactos; os novos 64 bytes devem ser zero
        CHECK(mem_test_sm.setup_memory(1, 128, nullptr) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> read_expanded(128, 0xFF);
        CHECK(mem_test_sm.get_memory(1, read_expanded.data(), 128, 0) == SCE_SAVEDATA_OK);
        // Primeiro bloco identico
        std::vector<uint8_t> first_half(read_expanded.begin(), read_expanded.begin() + 64);
        CHECK(first_half == original_mem_data);
        // Segundo bloco zerado
        for (std::size_t i = 64; i < 128; ++i) {
            CHECK(read_expanded[i] == 0);
        }

        // 5. T147: Chamada subsequente com tamanho MENOR (32 bytes):
        // Os primeiros 32 bytes DEVEM permanecer intactos
        CHECK(mem_test_sm.setup_memory(1, 32, nullptr) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> read_shrunk(32, 0);
        CHECK(mem_test_sm.get_memory(1, read_shrunk.data(), 32, 0) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> expected_shrunk(original_mem_data.begin(), original_mem_data.begin() + 32);
        CHECK(read_shrunk == expected_shrunk);

        // 6. T147: Reinicializacao e reconfiguracao preservam os dados
        CHECK(mem_test_sm.terminate());

        SaveDataManager reinit_mem_sm;
        CHECK(reinit_mem_sm.initialize(mem_test_box.string(), 16 * 1024 * 1024, nullptr));
        reinit_mem_sm.set_default_title_id("CUSA03173");

        // setup_memory apos reinicializacao preserva o conteudo
        CHECK(reinit_mem_sm.setup_memory(1, 32, nullptr) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> reinit_read(32, 0);
        CHECK(reinit_mem_sm.get_memory(1, reinit_read.data(), 32, 0) == SCE_SAVEDATA_OK);
        CHECK(reinit_read == expected_shrunk);

        CHECK(reinit_mem_sm.terminate());
        std::filesystem::remove_all(mem_test_box, ec);
    }

    // ========================================================================
    // 22. T178: Calculo de Quota sem Underflow/Overflow e Validacao de Filesystem (FR-008/FR-009)
    // ========================================================================
    {
        SaveDataManager quota_sm;
        std::filesystem::path quota_box = sandbox / "quota_safe_test";
        std::filesystem::remove_all(quota_box, ec);
        const std::size_t test_quota = 1024 * 1024; // 1 MiB
        CHECK(quota_sm.initialize(quota_box.string(), test_quota, nullptr));

        std::string pt;
        uint32_t stat = 0;
        CHECK(quota_sm.mount(0, "CUSA03173", "SPRJ0080", SCE_SAVEDATA_MOUNT_MODE_CREATE2, 1024, pt, stat) == SCE_SAVEDATA_OK);

        // 1. Escrita de arquivo de 100 KiB
        std::vector<uint8_t> buf_100k(100 * 1024, 0x55);
        CHECK(quota_sm.write_file(pt, "file_a.dat", buf_100k.data(), buf_100k.size()) == SCE_SAVEDATA_OK);

        // 2. Sobrescrita com tamanho que excederia quota se somasse sem abater old_size
        // Slot tem 100 KiB. Quota é 1 MiB. Nova escrita de 950 KiB no mesmo arquivo file_a.dat.
        // Se old_size (100 KiB) for abatido: total resultante = 950 KiB <= 1 MiB (OK).
        // Se somasse cegamente: 100 KiB + 950 KiB = 1050 KiB > 1 MiB (falharia indevidamente).
        std::vector<uint8_t> buf_950k(950 * 1024, 0x66);
        CHECK(quota_sm.write_file(pt, "file_a.dat", buf_950k.data(), buf_950k.size()) == SCE_SAVEDATA_OK);

        // 3. Rejeicao de escrita com tamanho absurdo (overflow potencial de 64 bits)
        std::size_t huge_size = std::numeric_limits<std::size_t>::max() - 10;
        uint8_t dummy_byte = 0xAA;
        CHECK(quota_sm.write_file(pt, "huge.dat", &dummy_byte, huge_size) == SCE_SAVEDATA_ERROR_QUOTA_EXCEEDED);

        // 4. Rejeicao de novo arquivo que ultrapassa quota
        std::vector<uint8_t> buf_100k_extra(100 * 1024, 0x77);
        // Total atual ja é 950 KiB. Mais 100 KiB daria 1050 KiB > 1 MiB
        CHECK(quota_sm.write_file(pt, "file_b.dat", buf_100k_extra.data(), buf_100k_extra.size()) == SCE_SAVEDATA_ERROR_QUOTA_EXCEEDED);

        CHECK(quota_sm.umount(pt, false) == SCE_SAVEDATA_OK);
        CHECK(quota_sm.terminate());
        std::filesystem::remove_all(quota_box, ec);
    }

    // ========================================================================
    // 23. T165: Propagacao de Falhas no Checksum Inicial ao Criar Slot (FR-008)
    // ========================================================================
    {
        SaveDataManager t165_sm;
        std::filesystem::path t165_sandbox = sandbox / "t165_test";
        std::filesystem::remove_all(t165_sandbox, ec);
        CHECK(t165_sm.initialize(t165_sandbox.string(), 16 * 1024 * 1024, nullptr));

        // 1. Falha induzida na geracao do checksum durante o mount de slot novo
        t165_sm.set_test_write_failure_phase(SaveDataManager::WriteFailurePhase::Checksum);
        std::string pt;
        uint32_t status = 0;
        int32_t ret = t165_sm.mount(0, "CUSA03173", "SPRJ0165", SCE_SAVEDATA_MOUNT_MODE_CREATE2, 4096, pt, status);
        CHECK(ret == SCE_SAVEDATA_ERROR_INTERNAL);
        CHECK(pt.empty());
        CHECK(t165_sm.find_slot(pt) < 0);

        // O slot incompleto foi descartado e nao resta diretorio orfao
        const auto slot_host = t165_sandbox / "savedata" / "0" / "CUSA03173" / "SPRJ0165";
        const auto slot_meta = t165_sandbox / "savedata" / "0" / "CUSA03173" / "SPRJ0165.sce_sys";
        CHECK(!std::filesystem::exists(slot_host));
        CHECK(!std::filesystem::exists(slot_meta));

        // 2. Recuperacao e remontagem sem falha induzida
        t165_sm.set_test_write_failure_phase(SaveDataManager::WriteFailurePhase::None);
        ret = t165_sm.mount(0, "CUSA03173", "SPRJ0165", SCE_SAVEDATA_MOUNT_MODE_CREATE2, 4096, pt, status);
        CHECK(ret == SCE_SAVEDATA_OK);
        CHECK(status == SCE_SAVEDATA_MOUNT_STATUS_CREATED);
        CHECK(!pt.empty());

        const std::string payload = "Authentic T165 Payload";
        CHECK(t165_sm.write_file(pt, "userdata0010", payload.data(), payload.size()) == SCE_SAVEDATA_OK);
        CHECK(t165_sm.umount(pt, true) == SCE_SAVEDATA_OK);

        // 3. Confirmar que o checksum real foi gravado e protege o save contra corrupcao
        const auto chk_file = slot_meta / "checksum.sha256";
        CHECK(std::filesystem::exists(chk_file));
        CHECK(std::filesystem::file_size(chk_file) >= 64);

        // Remontagem legitima com integridade valida
        CHECK(t165_sm.mount(0, "CUSA03173", "SPRJ0165", SCE_SAVEDATA_MOUNT_MODE_RDONLY, 4096, pt, status) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> readback;
        CHECK(t165_sm.read_file(pt, "userdata0010", readback) == SCE_SAVEDATA_OK);
        CHECK(std::string(readback.begin(), readback.end()) == payload);
        CHECK(t165_sm.umount(pt, false) == SCE_SAVEDATA_OK);

        // Corrupcao fisica para verificar que nao ha sucesso de integridade ficticio
        {
            std::ofstream cfs(slot_host / "userdata0010", std::ios::binary);
            cfs << "CORRUPTED_TAMPERED_DATA";
        }
        CHECK(t165_sm.mount(0, "CUSA03173", "SPRJ0165", SCE_SAVEDATA_MOUNT_MODE_RDONLY, 4096, pt, status) == SCE_SAVEDATA_ERROR_BROKEN);

        CHECK(t165_sm.terminate());
        std::filesystem::remove_all(t165_sandbox, ec);
    }

    // ========================================================================
    // 24. T166: Confinamento Canonico de dir_name_search e Leitura de Metadados (FR-008)
    // ========================================================================
    {
        SaveDataManager t166_sm;
        std::filesystem::path t166_sandbox = sandbox / "t166_test";
        std::filesystem::remove_all(t166_sandbox, ec);
        CHECK(t166_sm.initialize(t166_sandbox.string(), 16 * 1024 * 1024, nullptr));

        std::vector<SaveSearchEntry> entries;

        // 1. Rejeicao de travessia explicita no parametro title
        CHECK(t166_sm.dir_name_search(0, "../escape", "%", 0, 0, entries) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(t166_sm.dir_name_search(0, "..\\escape", "%", 0, 0, entries) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(t166_sm.dir_name_search(0, "CUSA03173/sub", "%", 0, 0, entries) == SCE_SAVEDATA_ERROR_PARAMETER);

        // 2. Rejeicao com default_title_id_ malformado / com travessia
        t166_sm.set_default_title_id("../escape");
        CHECK(t166_sm.dir_name_search(0, nullptr, "%", 0, 0, entries) == SCE_SAVEDATA_ERROR_PARAMETER);
        CHECK(t166_sm.dir_name_search(0, "", "%", 0, 0, entries) == SCE_SAVEDATA_ERROR_PARAMETER);
        t166_sm.set_default_title_id("CUSA03173");

        // 3. Criacao de slot legitimo
        std::string pt;
        uint32_t status = 0;
        CHECK(t166_sm.mount(0, "CUSA03173", "SPRJ0001", SCE_SAVEDATA_MOUNT_MODE_CREATE2, 4096, pt, status) == SCE_SAVEDATA_OK);
        SceSaveDataParam p1{};
        std::strncpy(p1.title, "Legit Slot", sizeof(p1.title) - 1);
        p1.user_param = 100;
        CHECK(t166_sm.set_param(pt, 0, &p1, sizeof(p1)) == SCE_SAVEDATA_OK);
        CHECK(t166_sm.umount(pt, true) == SCE_SAVEDATA_OK);

        // 4. Insercao de diretorio externo fora do root e tentativa de enumeracao via symlink
        auto outside_dir = t166_sandbox / "outside_dir";
        std::filesystem::create_directories(outside_dir);
        auto title_dir = t166_sm.resolve_savedata_root() / "0" / "CUSA03173";

        std::filesystem::create_directory_symlink(outside_dir, title_dir / "SPRJ_SYMLINK", ec);
        // Cria tambem symlink para o meta dir apontando fora
        std::filesystem::create_directory_symlink(outside_dir, title_dir / "SPRJ_FAKE.sce_sys", ec);

        // 5. Executa dir_name_search: apenas SPRJ0001 legitimo deve ser retornado
        CHECK(t166_sm.dir_name_search(0, "CUSA03173", "%", 0, 0, entries) == SCE_SAVEDATA_OK);
        CHECK(entries.size() == 1);
        CHECK(entries[0].dir_name == "SPRJ0001");
        CHECK(std::string(entries[0].param.title) == "Legit Slot");

        CHECK(t166_sm.terminate());
        std::filesystem::remove_all(t166_sandbox, ec);
    }

    // ========================================================================
    // 25. T148 / T162: Transacionalidade de delete_save e Verificacao em Blocos
    // ========================================================================
    {
        SaveDataManager t148_sm;
        std::filesystem::path t148_sandbox = sandbox / "t148_t162_test";
        std::filesystem::remove_all(t148_sandbox, ec);
        CHECK(t148_sm.initialize(t148_sandbox.string(), 16 * 1024 * 1024, nullptr));

        // 1. T162: Escrita de arquivo grande (2 MiB) com verificacao em blocos
        std::string pt;
        uint32_t status = 0;
        CHECK(t148_sm.mount(0, "CUSA03173", "SPRJ0148", SCE_SAVEDATA_MOUNT_MODE_CREATE2, 4096, pt, status) == SCE_SAVEDATA_OK);

        std::vector<uint8_t> large_payload(2 * 1024 * 1024);
        for (std::size_t i = 0; i < large_payload.size(); ++i) {
            large_payload[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
        }
        CHECK(t148_sm.write_file(pt, "large_data.bin", large_payload.data(), large_payload.size()) == SCE_SAVEDATA_OK);

        // Validar leitura com paridade total
        std::vector<uint8_t> read_large;
        CHECK(t148_sm.read_file(pt, "large_data.bin", read_large) == SCE_SAVEDATA_OK);
        CHECK(read_large == large_payload);

        // T162: Falha induzida na fase de verificacao em blocos descarta .tmp e preserva estado
        t148_sm.set_test_write_failure_phase(bb::SaveDataManager::WriteFailurePhase::Checksum);
        std::vector<uint8_t> corrupt_attempt = {1, 2, 3, 4};
        CHECK(t148_sm.write_file(pt, "fail_verify.bin", corrupt_attempt.data(), corrupt_attempt.size()) == SCE_SAVEDATA_ERROR_INTERNAL);
        t148_sm.set_test_write_failure_phase(bb::SaveDataManager::WriteFailurePhase::None);

        std::vector<uint8_t> not_written;
        CHECK(t148_sm.read_file(pt, "fail_verify.bin", not_written) == SCE_SAVEDATA_ERROR_NOT_FOUND);

        CHECK(t148_sm.umount(pt, true) == SCE_SAVEDATA_OK);

        const auto save_dir = t148_sm.resolve_savedata_root() / "0" / "CUSA03173" / "SPRJ0148";
        const auto meta_dir = t148_sm.resolve_savedata_root() / "0" / "CUSA03173" / "SPRJ0148.sce_sys";
        CHECK(std::filesystem::exists(save_dir / "large_data.bin"));
        CHECK(std::filesystem::exists(meta_dir));

        // 2. T148: Teste de falha intermediaria na exclusao
        struct FailMetaDeleteFs : bb::SaveDeleteFilesystem {
            int remove_count{0};
            bool exists(const std::filesystem::path &p, std::error_code &ec) override {
                return std::filesystem::exists(p, ec);
            }
            std::uintmax_t remove_all(const std::filesystem::path &p, std::error_code &ec) override {
                ++remove_count;
                if (remove_count == 2) {
                    // Falha induzida na remocao de metadados
                    ec = std::make_error_code(std::errc::permission_denied);
                    return 0;
                }
                return std::filesystem::remove_all(p, ec);
            }
        } fail_fs;

        t148_sm.set_delete_filesystem_for_testing(&fail_fs);
        int32_t del_ret = t148_sm.delete_save(0, "CUSA03173", "SPRJ0148");
        CHECK(del_ret == SCE_SAVEDATA_ERROR_INTERNAL);
        CHECK(fail_fs.remove_count == 2);
        t148_sm.set_delete_filesystem_for_testing(nullptr);

        // Rollback transacional recompos o save completo!
        CHECK(std::filesystem::exists(save_dir / "large_data.bin"));
        CHECK(std::filesystem::exists(meta_dir));

        // Confirmar que o save recomposto pode ser montado e lido perfeitamente
        CHECK(t148_sm.mount(0, "CUSA03173", "SPRJ0148", SCE_SAVEDATA_MOUNT_MODE_RDONLY, 4096, pt, status) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> rollback_read;
        CHECK(t148_sm.read_file(pt, "large_data.bin", rollback_read) == SCE_SAVEDATA_OK);
        CHECK(rollback_read == large_payload);
        CHECK(t148_sm.umount(pt, false) == SCE_SAVEDATA_OK);

        // 3. T148: Recuperacao no reinicio a partir de .delbak
        // Simular um crash que ocorreu apos apagar save_dir mas antes de apagar meta_dir
        std::filesystem::remove_all(save_dir, ec);
        const auto delbak_path = t148_sm.resolve_savedata_root() / "0" / "CUSA03173" / "SPRJ0148.delbak.123456789";
        std::filesystem::create_directories(delbak_path);
        {
            std::ofstream f(delbak_path / "large_data.bin", std::ios::binary);
            f.write(reinterpret_cast<const char*>(large_payload.data()), large_payload.size());
        }
        CHECK(!std::filesystem::exists(save_dir));
        CHECK(std::filesystem::exists(delbak_path));
        CHECK(std::filesystem::exists(meta_dir));

        // Reinicializar o SaveDataManager: recover_interrupted_commits deve restaurar o save a partir de .delbak
        CHECK(t148_sm.terminate());
        SaveDataManager t148_sm_reinit;
        CHECK(t148_sm_reinit.initialize(t148_sandbox.string(), 16 * 1024 * 1024, nullptr));

        CHECK(std::filesystem::exists(save_dir / "large_data.bin"));
        CHECK(!std::filesystem::exists(delbak_path));

        // Confirmar leitura integra do save restaurado apos crash simulado
        CHECK(t148_sm_reinit.mount(0, "CUSA03173", "SPRJ0148", SCE_SAVEDATA_MOUNT_MODE_RDONLY, 4096, pt, status) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> crash_rec_read;
        CHECK(t148_sm_reinit.read_file(pt, "large_data.bin", crash_rec_read) == SCE_SAVEDATA_OK);
        CHECK(crash_rec_read == large_payload);
        CHECK(t148_sm_reinit.umount(pt, false) == SCE_SAVEDATA_OK);

        // Agora exclusao limpa com sucesso
        CHECK(t148_sm_reinit.delete_save(0, "CUSA03173", "SPRJ0148") == SCE_SAVEDATA_OK);
        CHECK(!std::filesystem::exists(save_dir));
        CHECK(!std::filesystem::exists(meta_dir));

        CHECK(t148_sm_reinit.terminate());
        std::filesystem::remove_all(t148_sandbox, ec);
    }

    // ========================================================================
    // 26. T146: Retenção de Backups em I/O e Ordem Completa de Interrupções de Crash
    // ========================================================================
    {
        SaveDataManager t146_sm;
        std::filesystem::path t146_sandbox = sandbox / "t146_adversarial_test";
        std::filesystem::remove_all(t146_sandbox, ec);
        CHECK(t146_sm.initialize(t146_sandbox.string(), 16 * 1024 * 1024, nullptr));

        std::string pt;
        uint32_t status = 0;
        CHECK(t146_sm.mount(0, "CUSA03173", "SPRJ146A", SCE_SAVEDATA_MOUNT_MODE_CREATE2, 4096, pt, status) == SCE_SAVEDATA_OK);
        const std::string golden_data = "GOLDEN_SAVE_PAYLOAD_V1";
        CHECK(t146_sm.write_file(pt, "game.dat", golden_data.data(), golden_data.size()) == SCE_SAVEDATA_OK);
        CHECK(t146_sm.umount(pt, true) == SCE_SAVEDATA_OK);

        const auto save_a = t146_sm.resolve_savedata_root() / "0" / "CUSA03173" / "SPRJ146A";
        const auto meta_a = t146_sm.resolve_savedata_root() / "0" / "CUSA03173" / "SPRJ146A.sce_sys";
        CHECK(std::filesystem::exists(save_a / "game.dat"));
        CHECK(std::filesystem::exists(meta_a / "checksum.sha256"));

        // 1. Simulação de crash após commit de dados mas com metadados/checksum desatualizados:
        // Criar um .bak com o save íntegro e simular dados corrompidos no diretório ativo
        const auto bak_dir = t146_sm.resolve_savedata_root() / "0" / "CUSA03173" / "SPRJ146A.bak.999999999";
        std::filesystem::copy(save_a, bak_dir, std::filesystem::copy_options::recursive, ec);
        CHECK(std::filesystem::exists(bak_dir / "game.dat"));
        {
            // Corromper o arquivo no diretório ativo para quebrar o checksum
            std::ofstream f(save_a / "game.dat", std::ios::binary);
            f << "CORRUPTED_PARTIAL_WRITE";
        }

        // Reinicialização: recover_interrupted_commits deve detectar que o checksum não bate
        // e restaurar os dados válidos a partir do .bak
        CHECK(t146_sm.terminate());
        SaveDataManager t146_rec1;
        CHECK(t146_rec1.initialize(t146_sandbox.string(), 16 * 1024 * 1024, nullptr));
        CHECK(std::filesystem::exists(save_a / "game.dat"));
        CHECK(!std::filesystem::exists(bak_dir)); // Backup consumido e limpo

        CHECK(t146_rec1.mount(0, "CUSA03173", "SPRJ146A", SCE_SAVEDATA_MOUNT_MODE_RDONLY, 4096, pt, status) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> rec1_data;
        CHECK(t146_rec1.read_file(pt, "game.dat", rec1_data) == SCE_SAVEDATA_OK);
        CHECK(std::string(rec1_data.begin(), rec1_data.end()) == golden_data);
        CHECK(t146_rec1.umount(pt, false) == SCE_SAVEDATA_OK);

        // 2. Simulação de crash em metadados: .sce_sys corrompido (sem param.bin) e presença de .sce_sys.bak
        const auto meta_bak = t146_rec1.resolve_savedata_root() / "0" / "CUSA03173" / "SPRJ146A.sce_sys.bak.888888888";
        std::filesystem::copy(meta_a, meta_bak, std::filesystem::copy_options::recursive, ec);
        CHECK(std::filesystem::exists(meta_bak / "param.bin"));
        std::filesystem::remove(meta_a / "param.bin", ec);
        CHECK(!std::filesystem::exists(meta_a / "param.bin"));

        // Reinicialização: recover_interrupted_commits deve restaurar .sce_sys a partir de .sce_sys.bak
        CHECK(t146_rec1.terminate());
        SaveDataManager t146_rec2;
        CHECK(t146_rec2.initialize(t146_sandbox.string(), 16 * 1024 * 1024, nullptr));
        CHECK(std::filesystem::exists(meta_a / "param.bin"));
        CHECK(!std::filesystem::exists(meta_bak)); // Consumido

        // 3. Simulação de crash durante delete: save_dir removido, mas .delbak e meta_dir ainda presentes
        const auto delbak_dir = t146_rec2.resolve_savedata_root() / "0" / "CUSA03173" / "SPRJ146A.delbak.777777777";
        std::filesystem::rename(save_a, delbak_dir, ec);
        CHECK(!std::filesystem::exists(save_a));
        CHECK(std::filesystem::exists(delbak_dir));
        CHECK(std::filesystem::exists(meta_a));

        // Reinicialização: recover_interrupted_commits deve reverter a deleção incompleta restaurando save_a
        CHECK(t146_rec2.terminate());
        SaveDataManager t146_rec3;
        CHECK(t146_rec3.initialize(t146_sandbox.string(), 16 * 1024 * 1024, nullptr));
        CHECK(std::filesystem::exists(save_a / "game.dat"));
        CHECK(!std::filesystem::exists(delbak_dir));

        CHECK(t146_rec3.mount(0, "CUSA03173", "SPRJ146A", SCE_SAVEDATA_MOUNT_MODE_RDONLY, 4096, pt, status) == SCE_SAVEDATA_OK);
        std::vector<uint8_t> rec3_data;
        CHECK(t146_rec3.read_file(pt, "game.dat", rec3_data) == SCE_SAVEDATA_OK);
        CHECK(std::string(rec3_data.begin(), rec3_data.end()) == golden_data);
        CHECK(t146_rec3.umount(pt, false) == SCE_SAVEDATA_OK);

        // 4. Preservação de staging ativo em _work e limpeza exclusiva de órfãos
        const auto work_root = t146_rec3.resolve_savedata_root() / "_work";
        const auto orphan_work = work_root / "slot_orphan_999";
        std::filesystem::create_directories(orphan_work);
        {
            std::ofstream f(orphan_work / "orphan.tmp", std::ios::binary);
            f << "ORPHAN";
        }
        CHECK(std::filesystem::exists(orphan_work / "orphan.tmp"));

        // Montar um slot legítimo (que possui _work ativo)
        std::string pt_active;
        CHECK(t146_rec3.mount(0, "CUSA03173", "SPRJ146A", SCE_SAVEDATA_MOUNT_MODE_RDWR, 4096, pt_active, status) == SCE_SAVEDATA_OK);
        CHECK(t146_rec3.write_file(pt_active, "game.dat", "UPDATED", 7) == SCE_SAVEDATA_OK);

        // Chamada direta a recover_interrupted_commits com slot montado ativo:
        // O staging do slot ativo NÃO deve ser removido, mas o órfão DEVE ser purgado!
        t146_rec3.recover_interrupted_commits();
        CHECK(!std::filesystem::exists(orphan_work)); // Órfão purgado

        std::vector<uint8_t> active_check;
        CHECK(t146_rec3.read_file(pt_active, "game.dat", active_check) == SCE_SAVEDATA_OK);
        CHECK(std::string(active_check.begin(), active_check.end()) == "UPDATED"); // Ativo preservado

        CHECK(t146_rec3.umount(pt_active, true) == SCE_SAVEDATA_OK);
        CHECK(t146_rec3.terminate());
        std::filesystem::remove_all(t146_sandbox, ec);
    }

    // Finalizacao e limpeza
    CHECK(sceSaveDataTerminate() == SCE_SAVEDATA_OK);
    std::filesystem::remove_all(sandbox, ec);
}); }
