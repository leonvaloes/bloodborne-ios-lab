#include "check.h"
#include "fios2_hle.h"
#include "vfs.h"
#include "guest_memory.h"
#include <filesystem>
#include <vector>
#include <string>
#include <cstring>
#include <fstream>
#include <thread>
#include <array>
#include <atomic>

int main() { return run([] {
    using namespace bb;

    std::filesystem::path sandbox = std::filesystem::current_path() / "test_fios2_sandbox";
    std::filesystem::path app_dir = sandbox / "app0";
    std::filesystem::path patch_dir = sandbox / "patch";
    std::filesystem::path save_dir = sandbox / "savedata";

    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
    std::filesystem::create_directories(app_dir, ec);
    std::filesystem::create_directories(patch_dir, ec);
    std::filesystem::create_directories(save_dir, ec);

    // ========================================================================
    // Preparação do VFS e Arquivos de Teste
    // ========================================================================
    VirtualFileSystem vfs;
    CHECK(vfs.mount("/app0", app_dir.string(), true));
    CHECK(vfs.mount("/patch", patch_dir.string(), true));
    CHECK(vfs.mount("/user/home/savedata", save_dir.string(), false));

    // Arquivos estáticos em /app0
    {
        std::ofstream f(app_dir / "param.sfo", std::ios::binary);
        f.write("SFOTEST001", 10);
    }
    {
        std::ofstream f(app_dir / "game_data.bin", std::ios::binary);
        // Cria arquivo com 256 bytes incrementais (0..255)
        std::vector<std::uint8_t> data(256);
        for (std::size_t i = 0; i < 256; ++i) {
            data[i] = static_cast<std::uint8_t>(i);
        }
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
    }
    {
        std::ofstream f(patch_dir / "param.sfo", std::ios::binary);
        f.write("SFOPATCH99", 10);
    }

    // ========================================================================
    // 1. Estado inicial e validação de inicialização
    // ========================================================================
    Fios2Manager uninit_mgr(nullptr);
    CHECK(uninit_mgr.sceFiosFHOpenSync("/app0/param.sfo", SCE_FIOS_O_RDONLY) == SCE_FIOS_ERROR_NOT_INITIALIZED);
    CHECK(uninit_mgr.sceFiosDHOpenSync("/app0") == SCE_FIOS_ERROR_NOT_INITIALIZED);

    Fios2Manager mgr(&vfs);
    CHECK(mgr.vfs() == &vfs);
    CHECK(mgr.open_file_count() == 0);
    CHECK(mgr.open_dir_count() == 0);
    CHECK(mgr.total_handle_count() == 0);

    // ========================================================================
    // 2. Validações de Argumentos, Bounds e Proteção contra Path Traversal
    // ========================================================================
    CHECK(mgr.sceFiosFHOpenSync("", SCE_FIOS_O_RDONLY) == SCE_FIOS_ERROR_INVALID_ARGUMENT);
    CHECK(mgr.sceFiosFHOpenSync("/app0/../escape.bin", SCE_FIOS_O_RDONLY) == SCE_FIOS_ERROR_PATH_TRAVERSAL);
    CHECK(mgr.sceFiosFHOpenSync("/app0/data/../../escape.bin", SCE_FIOS_O_RDONLY) == SCE_FIOS_ERROR_PATH_TRAVERSAL);
    CHECK(mgr.sceFiosDHOpenSync("/app0/../escape") == SCE_FIOS_ERROR_PATH_TRAVERSAL);

    SceFiosStat dummy_stat{};
    CHECK(mgr.sceFiosStatSync("/app0/../secret", dummy_stat) == SCE_FIOS_ERROR_PATH_TRAVERSAL);
    CHECK(mgr.sceFiosFileDeleteSync("/app0/../secret") == SCE_FIOS_ERROR_PATH_TRAVERSAL);
    CHECK(mgr.sceFiosDirectoryCreate("/user/home/savedata/../escape") == SCE_FIOS_ERROR_PATH_TRAVERSAL);
    CHECK(mgr.sceFiosRenameSync("/user/home/savedata/a", "/user/home/savedata/../b") == SCE_FIOS_ERROR_PATH_TRAVERSAL);

    CHECK(mgr.sceFiosFHOpenSync("/nonexistent/file.bin", SCE_FIOS_O_RDONLY) == SCE_FIOS_ERROR_NOT_FOUND);
    CHECK(!mgr.sceFiosIsValidHandle(0));
    CHECK(!mgr.sceFiosIsValidHandle(-1));
    CHECK(!mgr.sceFiosIsValidHandle(9999));

    // Tentativa de escrita em montagem read-only (/app0)
    CHECK(mgr.sceFiosFHOpenSync("/app0/write_test.bin", SCE_FIOS_O_WRONLY | SCE_FIOS_O_CREAT) == SCE_FIOS_ERROR_ACCESS_DENIED);

    // ========================================================================
    // 3. Ciclo de Vida Completo de Arquivo (Open, Write, Seek, Tell, Sync, Read, Close)
    // ========================================================================
    const std::string save_path = "/user/home/savedata/save01.dat";
    SceFiosFH write_fh = mgr.sceFiosFHOpenSync(save_path, SCE_FIOS_O_WRONLY | SCE_FIOS_O_CREAT | SCE_FIOS_O_TRUNC);
    CHECK(write_fh > 0);
    CHECK(mgr.sceFiosIsValidHandle(write_fh));
    CHECK(mgr.open_file_count() == 1);

    const std::string test_payload = "Hunter's Dream Blood Echoes 123456789";
    std::int64_t written = mgr.sceFiosFHWriteSync(write_fh, test_payload.data(), test_payload.size());
    CHECK(written == static_cast<std::int64_t>(test_payload.size()));

    std::int64_t cur_tell = mgr.sceFiosFHTell(write_fh);
    CHECK(cur_tell == static_cast<std::int64_t>(test_payload.size()));

    CHECK(mgr.sceFiosFHSyncSync(write_fh) == SCE_FIOS_OK);
    CHECK(mgr.sceFiosFHCloseSync(write_fh) == SCE_FIOS_OK);
    CHECK(!mgr.sceFiosIsValidHandle(write_fh));
    CHECK(mgr.open_file_count() == 0);

    // Reabre para leitura
    SceFiosFH read_fh = mgr.sceFiosFHOpenSync(save_path, SCE_FIOS_O_RDONLY);
    CHECK(read_fh > 0);
    CHECK(mgr.sceFiosIsValidHandle(read_fh));

    // Tell no início
    CHECK(mgr.sceFiosFHTell(read_fh) == 0);

    // Leitura completa
    std::vector<char> read_buf(test_payload.size() + 1, 0);
    std::int64_t read_bytes = mgr.sceFiosFHReadSync(read_fh, read_buf.data(), test_payload.size());
    CHECK(read_bytes == static_cast<std::int64_t>(test_payload.size()));
    CHECK(std::string(read_buf.data(), read_bytes) == test_payload);

    // Leitura após EOF retorna 0
    std::int64_t eof_read = mgr.sceFiosFHReadSync(read_fh, read_buf.data(), 10);
    CHECK(eof_read == 0);

    // Seek SET
    std::int64_t seek_pos = mgr.sceFiosFHSeek(read_fh, 9, SCE_FIOS_SEEK_SET);
    CHECK(seek_pos == 9);
    CHECK(mgr.sceFiosFHTell(read_fh) == 9);

    std::array<char, 6> word{};
    read_bytes = mgr.sceFiosFHReadSync(read_fh, word.data(), 5);
    CHECK(read_bytes == 5);
    CHECK(std::string(word.data(), 5) == "Dream");

    // Seek CUR
    seek_pos = mgr.sceFiosFHSeek(read_fh, 1, SCE_FIOS_SEEK_CUR);
    CHECK(seek_pos == 15);
    std::array<char, 6> blood{};
    read_bytes = mgr.sceFiosFHReadSync(read_fh, blood.data(), 5);
    CHECK(read_bytes == 5);
    CHECK(std::string(blood.data(), 5) == "Blood");

    // Seek END
    seek_pos = mgr.sceFiosFHSeek(read_fh, -9, SCE_FIOS_SEEK_END);
    CHECK(seek_pos == static_cast<std::int64_t>(test_payload.size()) - 9);
    std::array<char, 10> digits{};
    read_bytes = mgr.sceFiosFHReadSync(read_fh, digits.data(), 9);
    CHECK(read_bytes == 9);
    CHECK(std::string(digits.data(), 9) == "123456789");

    // Seek fora de limites (< 0) deve falhar
    CHECK(mgr.sceFiosFHSeek(read_fh, -100, SCE_FIOS_SEEK_SET) == SCE_FIOS_ERROR_SEEK_OUT_OF_BOUNDS);
    CHECK(mgr.sceFiosFHSeek(read_fh, -100, SCE_FIOS_SEEK_CUR) == SCE_FIOS_ERROR_SEEK_OUT_OF_BOUNDS);

    CHECK(mgr.sceFiosFHCloseSync(read_fh) == SCE_FIOS_OK);

    // Tentativa de operações em handle fechado
    CHECK(mgr.sceFiosFHReadSync(read_fh, read_buf.data(), 5) == SCE_FIOS_ERROR_INVALID_HANDLE);
    CHECK(mgr.sceFiosFHWriteSync(read_fh, read_buf.data(), 5) == SCE_FIOS_ERROR_INVALID_HANDLE);
    CHECK(mgr.sceFiosFHSeek(read_fh, 0, SCE_FIOS_SEEK_SET) == SCE_FIOS_ERROR_INVALID_HANDLE);
    CHECK(mgr.sceFiosFHTell(read_fh) == SCE_FIOS_ERROR_INVALID_HANDLE);
    CHECK(mgr.sceFiosFHCloseSync(read_fh) == SCE_FIOS_ERROR_INVALID_HANDLE);

    // ========================================================================
    // 4. Streaming e Seek Aleatório em Arquivo VFS
    // ========================================================================
    SceFiosFH game_fh = mgr.sceFiosFHOpenSync("/app0/game_data.bin", SCE_FIOS_O_RDONLY);
    CHECK(game_fh > 0);

    // Lê bytes em posições arbitrárias
    for (std::uint64_t offset : {0ULL, 64ULL, 128ULL, 200ULL, 250ULL}) {
        CHECK(mgr.sceFiosFHSeek(game_fh, static_cast<std::int64_t>(offset), SCE_FIOS_SEEK_SET) == static_cast<std::int64_t>(offset));
        std::uint8_t b = 0;
        CHECK(mgr.sceFiosFHReadSync(game_fh, &b, 1) == 1);
        CHECK(b == static_cast<std::uint8_t>(offset));
    }

    // Leitura parcial na borda final do arquivo (256 bytes total)
    CHECK(mgr.sceFiosFHSeek(game_fh, 250, SCE_FIOS_SEEK_SET) == 250);
    std::array<std::uint8_t, 16> edge_buf{};
    std::int64_t got = mgr.sceFiosFHReadSync(game_fh, edge_buf.data(), edge_buf.size());
    CHECK(got == 6); // 256 - 250 = 6 bytes restantes
    for (int i = 0; i < 6; ++i) {
        CHECK(edge_buf[i] == static_cast<std::uint8_t>(250 + i));
    }

    CHECK(mgr.sceFiosFHCloseSync(game_fh) == SCE_FIOS_OK);

    // ========================================================================
    // 5. Stat, Criação de Diretório, Listagem e Manipulação de Arquivos
    // ========================================================================
    SceFiosStat stat_res{};
    CHECK(mgr.sceFiosStatSync("/app0/game_data.bin", stat_res) == SCE_FIOS_OK);
    CHECK(stat_res.size == 256);
    CHECK((stat_res.attributes & SCE_FIOS_ATTR_REGULAR) != 0);
    CHECK((stat_res.attributes & SCE_FIOS_ATTR_READONLY) != 0);
    CHECK((stat_res.attributes & SCE_FIOS_ATTR_DIRECTORY) == 0);

    // Criação de diretório hierárquico
    const std::string new_dir = "/user/home/savedata/profiles/hunter1";
    CHECK(mgr.sceFiosDirectoryCreate(new_dir) == SCE_FIOS_OK);

    CHECK(mgr.sceFiosStatSync(new_dir, stat_res) == SCE_FIOS_OK);
    CHECK((stat_res.attributes & SCE_FIOS_ATTR_DIRECTORY) != 0);

    // Cria arquivos dentro do novo diretório
    {
        SceFiosFH f1 = mgr.sceFiosFHOpenSync(new_dir + "/profile.json", SCE_FIOS_O_WRONLY | SCE_FIOS_O_CREAT);
        CHECK(f1 > 0);
        const char j[] = "{\"level\": 120}";
        CHECK(mgr.sceFiosFHWriteSync(f1, j, sizeof(j)) == sizeof(j));
        CHECK(mgr.sceFiosFHCloseSync(f1) == SCE_FIOS_OK);

        SceFiosFH f2 = mgr.sceFiosFHOpenSync(new_dir + "/inventory.bin", SCE_FIOS_O_WRONLY | SCE_FIOS_O_CREAT);
        CHECK(f2 > 0);
        const char inv[] = "SawCleaver+10";
        CHECK(mgr.sceFiosFHWriteSync(f2, inv, sizeof(inv)) == sizeof(inv));
        CHECK(mgr.sceFiosFHCloseSync(f2) == SCE_FIOS_OK);
    }

    // Listagem sequencial de diretório (SceFiosDH)
    SceFiosDH dh = mgr.sceFiosDHOpenSync(new_dir);
    CHECK(dh > 0);
    CHECK(mgr.sceFiosIsValidDH(dh));
    CHECK(mgr.open_dir_count() == 1);

    std::vector<std::string> dir_filenames;
    SceFiosDirEntry entry{};
    while (mgr.sceFiosDHReadSync(dh, &entry) == 1) {
        dir_filenames.push_back(entry.name);
        CHECK((entry.attributes & SCE_FIOS_ATTR_REGULAR) != 0);
        CHECK(entry.size > 0);
    }
    CHECK(dir_filenames.size() == 2);
    // Ordenação alfabética determinística
    CHECK(dir_filenames[0] == "inventory.bin");
    CHECK(dir_filenames[1] == "profile.json");

    // EOF após ler todos
    CHECK(mgr.sceFiosDHReadSync(dh, &entry) == 0);
    CHECK(mgr.sceFiosDHCloseSync(dh) == SCE_FIOS_OK);
    CHECK(!mgr.sceFiosIsValidDH(dh));
    CHECK(mgr.open_dir_count() == 0);

    // Renomeação de arquivo
    CHECK(mgr.sceFiosRenameSync(new_dir + "/profile.json", new_dir + "/profile_renamed.json") == SCE_FIOS_OK);
    CHECK(mgr.sceFiosStatSync(new_dir + "/profile.json", stat_res) == SCE_FIOS_ERROR_NOT_FOUND);
    CHECK(mgr.sceFiosStatSync(new_dir + "/profile_renamed.json", stat_res) == SCE_FIOS_OK);

    // Exclusão de arquivo
    CHECK(mgr.sceFiosFileDeleteSync(new_dir + "/profile_renamed.json") == SCE_FIOS_OK);
    CHECK(mgr.sceFiosStatSync(new_dir + "/profile_renamed.json", stat_res) == SCE_FIOS_ERROR_NOT_FOUND);

    // ========================================================================
    // 6. Operações com Memória Guest (GuestMemory)
    // ========================================================================
    constexpr std::size_t kGuestBudget = 2 * 1024 * 1024; // 2 MiB
    GuestMemory guest_mem(kGuestBudget);
    mgr.set_guest_memory(&guest_mem);
    CHECK(mgr.guest_memory() == &guest_mem);

    // Mapeia regiões no guest: 0x100000 para leitura/escrita
    constexpr std::uint64_t kGuestPage = 0x100000;
    CHECK(guest_mem.map(kGuestPage, 64 * 1024, Permission::read | Permission::write) == MemoryError::ok);

    // Leitura direta do FIOS2 para GuestMemory
    SceFiosFH g_fh = mgr.sceFiosFHOpenSync("/app0/game_data.bin", SCE_FIOS_O_RDONLY);
    CHECK(g_fh > 0);
    std::int64_t g_read = mgr.sceFiosFHReadSync(g_fh, guest_mem, kGuestPage, 256);
    CHECK(g_read == 256);
    CHECK(mgr.sceFiosFHCloseSync(g_fh) == SCE_FIOS_OK);

    // Verifica integridade dos bytes na memória guest
    std::vector<std::uint8_t> guest_check(256);
    CHECK(guest_mem.load(kGuestPage, guest_check.data(), 256) == MemoryError::ok);
    for (std::size_t i = 0; i < 256; ++i) {
        CHECK(guest_check[i] == static_cast<std::uint8_t>(i));
    }

    // Modifica alguns bytes no guest e grava em novo arquivo através do FIOS2
    for (std::size_t i = 0; i < 256; ++i) {
        guest_check[i] = static_cast<std::uint8_t>(255 - i);
    }
    CHECK(guest_mem.store(kGuestPage, guest_check.data(), 256) == MemoryError::ok);

    const std::string guest_out_path = "/user/home/savedata/inverted.bin";
    SceFiosFH g_out_fh = mgr.sceFiosFHOpenSync(guest_out_path, SCE_FIOS_O_WRONLY | SCE_FIOS_O_CREAT | SCE_FIOS_O_TRUNC);
    CHECK(g_out_fh > 0);
    std::int64_t g_written = mgr.sceFiosFHWriteSync(g_out_fh, guest_mem, kGuestPage, 256);
    CHECK(g_written == 256);
    CHECK(mgr.sceFiosFHCloseSync(g_out_fh) == SCE_FIOS_OK);

    // Stat para GuestMemory
    constexpr std::uint64_t kGuestStatAddr = kGuestPage + 1024;
    CHECK(mgr.sceFiosStatSync(guest_out_path, guest_mem, kGuestStatAddr) == SCE_FIOS_OK);
    SceFiosStat guest_stat{};
    CHECK(guest_mem.load(kGuestStatAddr, &guest_stat, sizeof(guest_stat)) == MemoryError::ok);
    CHECK(guest_stat.size == 256);

    // Listagem de diretório para GuestMemory
    SceFiosDH g_dh = mgr.sceFiosDHOpenSync(new_dir);
    CHECK(g_dh > 0);
    constexpr std::uint64_t kGuestDirAddr = kGuestPage + 2048;
    CHECK(mgr.sceFiosDHReadSync(g_dh, guest_mem, kGuestDirAddr) == 1);
    SceFiosDirEntry g_entry{};
    CHECK(guest_mem.load(kGuestDirAddr, &g_entry, sizeof(g_entry)) == MemoryError::ok);
    CHECK(std::string(g_entry.name) == "inventory.bin");
    CHECK(mgr.sceFiosDHCloseSync(g_dh) == SCE_FIOS_OK);

    // Operação para endereço não-mapeado no guest deve falhar com erro
    constexpr std::uint64_t kUnmappedAddr = 0x90000000;
    CHECK(mgr.sceFiosStatSync(guest_out_path, guest_mem, kUnmappedAddr) == SCE_FIOS_ERROR_INVALID_ARGUMENT);

    // ========================================================================
    // 7. Limite Rígido de 256 Handles Concorrentes
    // ========================================================================
    std::vector<SceFiosFH> open_handles;
    open_handles.reserve(SCE_FIOS_MAX_HANDLES + 1);

    for (std::size_t i = 0; i < SCE_FIOS_MAX_HANDLES; ++i) {
        SceFiosFH h = mgr.sceFiosFHOpenSync("/app0/param.sfo", SCE_FIOS_O_RDONLY);
        CHECK(h > 0);
        open_handles.push_back(h);
    }
    CHECK(mgr.total_handle_count() == SCE_FIOS_MAX_HANDLES);

    // Abertura do 257º handle deve falhar com SCE_FIOS_ERROR_TOO_MANY_HANDLES
    SceFiosFH overflow_fh = mgr.sceFiosFHOpenSync("/app0/param.sfo", SCE_FIOS_O_RDONLY);
    CHECK(overflow_fh == SCE_FIOS_ERROR_TOO_MANY_HANDLES);

    // Também deve falhar para abertura de diretório
    SceFiosDH overflow_dh = mgr.sceFiosDHOpenSync("/app0");
    CHECK(overflow_dh == SCE_FIOS_ERROR_TOO_MANY_HANDLES);

    // Fecha 1 handle e verifica que novo handle pode ser alocado
    CHECK(mgr.sceFiosFHCloseSync(open_handles.back()) == SCE_FIOS_OK);
    open_handles.pop_back();
    CHECK(mgr.total_handle_count() == SCE_FIOS_MAX_HANDLES - 1);

    SceFiosFH realloc_fh = mgr.sceFiosFHOpenSync("/app0/param.sfo", SCE_FIOS_O_RDONLY);
    CHECK(realloc_fh > 0);
    open_handles.push_back(realloc_fh);

    // Fecha todos os handles
    for (SceFiosFH h : open_handles) {
        CHECK(mgr.sceFiosFHCloseSync(h) == SCE_FIOS_OK);
    }
    CHECK(mgr.total_handle_count() == 0);

    // ========================================================================
    // 8. Overlays e Resolução Dinâmica de Caminhos
    // ========================================================================
    // Sem overlay: /app0/param.sfo retorna conteúdo base "SFOTEST001"
    {
        SceFiosFH base_fh = mgr.sceFiosFHOpenSync("/app0/param.sfo", SCE_FIOS_O_RDONLY);
        CHECK(base_fh > 0);
        char buf[11]{0};
        CHECK(mgr.sceFiosFHReadSync(base_fh, buf, 10) == 10);
        CHECK(std::string(buf) == "SFOTEST001");
        CHECK(mgr.sceFiosFHCloseSync(base_fh) == SCE_FIOS_OK);
    }

    // Adiciona overlay: /app0 -> /patch (ordem 10)
    std::int32_t overlay_id = 0;
    CHECK(mgr.sceFiosOverlayAddSync("/app0", "/patch", &overlay_id, 10) == SCE_FIOS_OK);
    CHECK(overlay_id > 0);
    CHECK(mgr.overlay_count() == 1);

    // Agora, leitura de /app0/param.sfo é redirecionada para /patch/param.sfo ("SFOPATCH99")
    {
        SceFiosFH patch_fh = mgr.sceFiosFHOpenSync("/app0/param.sfo", SCE_FIOS_O_RDONLY);
        CHECK(patch_fh > 0);
        char buf[11]{0};
        CHECK(mgr.sceFiosFHReadSync(patch_fh, buf, 10) == 10);
        CHECK(std::string(buf) == "SFOPATCH99");
        CHECK(mgr.sceFiosFHCloseSync(patch_fh) == SCE_FIOS_OK);
    }

    // Arquivo presente apenas em /app0 (/app0/game_data.bin) continua sendo encontrado por fallthrough
    {
        SceFiosFH gd_fh = mgr.sceFiosFHOpenSync("/app0/game_data.bin", SCE_FIOS_O_RDONLY);
        CHECK(gd_fh > 0);
        CHECK(mgr.sceFiosFHCloseSync(gd_fh) == SCE_FIOS_OK);
    }

    // Remoção do overlay restaura comportamento original
    CHECK(mgr.sceFiosOverlayRemoveSync(overlay_id) == SCE_FIOS_OK);
    CHECK(mgr.overlay_count() == 0);

    {
        SceFiosFH base_fh = mgr.sceFiosFHOpenSync("/app0/param.sfo", SCE_FIOS_O_RDONLY);
        CHECK(base_fh > 0);
        char buf[11]{0};
        CHECK(mgr.sceFiosFHReadSync(base_fh, buf, 10) == 10);
        CHECK(std::string(buf) == "SFOTEST001");
        CHECK(mgr.sceFiosFHCloseSync(base_fh) == SCE_FIOS_OK);
    }

    // ========================================================================
    // 9. Operações Assíncronas (SceFiosOp)
    // ========================================================================
    SceFiosOp manual_op = mgr.sceFiosOpCreate(SCE_FIOS_OK, 42);
    CHECK(manual_op > 0);
    CHECK(mgr.sceFiosOpWait(manual_op) == SCE_FIOS_OK);
    CHECK(mgr.sceFiosOpGetActualCount(manual_op) == 42);
    CHECK(mgr.sceFiosOpDelete(manual_op) == SCE_FIOS_OK);
    CHECK(mgr.sceFiosOpWait(manual_op) == SCE_FIOS_ERROR_INVALID_HANDLE);

    // Integração de SceFiosOp com Open / Read / Close no estilo PS4
    SceFiosOp async_op = 0;
    SceFiosFH async_fh = 0;
    CHECK(mgr.sceFiosFHOpenSync(&async_op, &async_fh, "/app0/param.sfo", SCE_FIOS_O_RDONLY) == SCE_FIOS_OK);
    CHECK(async_fh > 0);
    CHECK(async_op > 0);
    CHECK(mgr.sceFiosOpWait(async_op) == SCE_FIOS_OK);
    CHECK(mgr.sceFiosOpGetActualCount(async_op) == 1);

    char op_read_buf[11]{0};
    CHECK(mgr.sceFiosFHReadSync(&async_op, async_fh, op_read_buf, 10) == 10);
    CHECK(mgr.sceFiosOpGetActualCount(async_op) == 10);

    CHECK(mgr.sceFiosFHCloseSync(&async_op, async_fh) == SCE_FIOS_OK);
    CHECK(mgr.sceFiosOpDelete(async_op) == SCE_FIOS_OK);

    // ========================================================================
    // 10. Montagem de Arquivo / Diretório (sceFiosArchiveMountSync)
    // ========================================================================
    std::filesystem::path arc_dir = sandbox / "archive_folder";
    std::filesystem::create_directories(arc_dir, ec);
    {
        std::ofstream f(arc_dir / "archived_asset.dat", std::ios::binary);
        f.write("ARCHIVE_DATA_12345", 18);
    }

    SceFiosFH arc_fh = 0;
    CHECK(mgr.sceFiosArchiveMountSync(arc_dir.string(), "/mounted_arc", &arc_fh) == SCE_FIOS_OK);
    CHECK(vfs.is_mounted("/mounted_arc"));

    SceFiosFH asset_fh = mgr.sceFiosFHOpenSync("/mounted_arc/archived_asset.dat", SCE_FIOS_O_RDONLY);
    CHECK(asset_fh > 0);
    char arc_read_buf[32]{0};
    CHECK(mgr.sceFiosFHReadSync(asset_fh, arc_read_buf, 18) == 18);
    CHECK(std::string(arc_read_buf, 18) == "ARCHIVE_DATA_12345");
    CHECK(mgr.sceFiosFHCloseSync(asset_fh) == SCE_FIOS_OK);
    if (arc_fh > 0) {
        CHECK(mgr.sceFiosFHCloseSync(arc_fh) == SCE_FIOS_OK);
    }

    // Rejeição de caminhos inválidos e traversal em ArchiveMountSync
    CHECK(mgr.sceFiosArchiveMountSync("/app0/../bad", "/mount") == SCE_FIOS_ERROR_PATH_TRAVERSAL);
    CHECK(mgr.sceFiosArchiveMountSync(arc_dir.string(), "/mount/../bad") == SCE_FIOS_ERROR_PATH_TRAVERSAL);
    CHECK(mgr.sceFiosArchiveMountSync("", "/mount") == SCE_FIOS_ERROR_INVALID_ARGUMENT);
    CHECK(mgr.sceFiosArchiveMountSync(arc_dir.string(), "") == SCE_FIOS_ERROR_INVALID_ARGUMENT);
    CHECK(mgr.sceFiosArchiveMountSync("/nonexistent/arc.bhd5", "/mount") == SCE_FIOS_ERROR_NOT_FOUND);

    // ========================================================================
    // 11. Teste Multithread com Leituras Simultâneas
    // ========================================================================
    constexpr int kNumThreads = 6;
    constexpr int kIterationsPerThread = 50;
    std::vector<std::thread> workers;
    workers.reserve(kNumThreads);
    std::atomic<bool> threads_passed{true};

    for (int t = 0; t < kNumThreads; ++t) {
        workers.emplace_back([&mgr, &threads_passed, t] {
            for (int iter = 0; iter < kIterationsPerThread; ++iter) {
                SceFiosFH fh = mgr.sceFiosFHOpenSync("/app0/game_data.bin", SCE_FIOS_O_RDONLY);
                if (fh <= 0) {
                    threads_passed.store(false);
                    return;
                }

                // Seek determinístico baseado na thread e iteração
                std::int64_t target_offset = ((t * 17) + (iter * 7)) % 240;
                if (mgr.sceFiosFHSeek(fh, target_offset, SCE_FIOS_SEEK_SET) != target_offset) {
                    threads_passed.store(false);
                    mgr.sceFiosFHCloseSync(fh);
                    return;
                }

                std::array<std::uint8_t, 16> buf{};
                std::int64_t n = mgr.sceFiosFHReadSync(fh, buf.data(), buf.size());
                if (n != 16) {
                    threads_passed.store(false);
                    mgr.sceFiosFHCloseSync(fh);
                    return;
                }

                for (std::size_t i = 0; i < 16; ++i) {
                    if (buf[i] != static_cast<std::uint8_t>(target_offset + i)) {
                        threads_passed.store(false);
                        mgr.sceFiosFHCloseSync(fh);
                        return;
                    }
                }

                if (mgr.sceFiosFHCloseSync(fh) != SCE_FIOS_OK) {
                    threads_passed.store(false);
                    return;
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }
    CHECK(threads_passed.load());
    CHECK(mgr.total_handle_count() == 0);

    // Limpeza da sandbox de teste
    std::filesystem::remove_all(sandbox, ec);
}); }
