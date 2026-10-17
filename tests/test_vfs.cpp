#include "check.h"
#include "vfs.h"
#include "binder.h"
#include <filesystem>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <array>
#include <cstdint>

int main() { return run([] {
    using namespace bb;

    // Normalização de caminhos
    CHECK(VirtualFileSystem::normalize_path("") == "/");
    CHECK(VirtualFileSystem::normalize_path("app0/eboot.bin") == "app0/eboot.bin");
    CHECK(VirtualFileSystem::normalize_path("/app0\\data//sub/") == "/app0/data/sub");
    CHECK(VirtualFileSystem::normalize_path("///") == "/");

    // Detecção de traversal perigoso
    CHECK(VirtualFileSystem::contains_traversal("/app0/../secret"));
    CHECK(VirtualFileSystem::contains_traversal(".."));
    CHECK(VirtualFileSystem::contains_traversal("/foo/bar/.."));
    CHECK(!VirtualFileSystem::contains_traversal("/app0/data..txt"));
    CHECK(!VirtualFileSystem::contains_traversal("/app0/normal/file.bin"));

    // CRC32
    const char msg[] = "123456789";
    CHECK(VirtualFileSystem::compute_crc32(msg, 9) == 0xCBF43926u);

    // Criação de diretório temporário para testes de montagem
    std::filesystem::path test_dir = std::filesystem::current_path() / "test_vfs_sandbox";
    std::filesystem::path app_dir = test_dir / "app";
    std::filesystem::path save_dir = test_dir / "saves";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(app_dir, ec);
    std::filesystem::create_directories(save_dir, ec);

    VirtualFileSystem vfs;
    CHECK(vfs.mount("/app0", app_dir.string(), true));
    CHECK(vfs.mount("/user/home/savedata", save_dir.string(), false));
    CHECK(vfs.is_mounted("/app0"));
    CHECK(vfs.is_mounted("/user/home/savedata"));
    CHECK(!vfs.is_mounted("/unknown"));

    // Resolução de caminhos
    std::string host_path;
    bool readonly = false;
    CHECK(vfs.resolve("/app0/param.sfo", host_path, readonly) == VfsError::ok);
    CHECK(readonly == true);

    CHECK(vfs.resolve("/user/home/savedata/save001.bin", host_path, readonly) == VfsError::ok);
    CHECK(readonly == false);

    CHECK(vfs.resolve("/app0/../etc/passwd", host_path, readonly) == VfsError::traversal_detected);
    CHECK(vfs.resolve("/nonexistent/file", host_path, readonly) == VfsError::not_mounted);

    // Tentativa de escrita em diretório apenas-leitura
    const char dummy_data[] = "Test Payload Bloodborne";
    CHECK(vfs.write_file("/app0/write_fail.bin", dummy_data, sizeof(dummy_data)) == VfsError::access_denied);

    // Escrita atômica e leitura em diretório de escrita
    CHECK(vfs.write_file("/user/home/savedata/save01.dat", dummy_data, sizeof(dummy_data), true) == VfsError::ok);
    CHECK(vfs.exists("/user/home/savedata/save01.dat"));

    VfsStat st{};
    CHECK(vfs.stat("/user/home/savedata/save01.dat", st) == VfsError::ok);
    CHECK(!st.is_directory);
    CHECK(st.size == sizeof(dummy_data));
    CHECK(!st.is_readonly);

    std::vector<std::uint8_t> read_bytes;
    CHECK(vfs.read_file("/user/home/savedata/save01.dat", read_bytes) == VfsError::ok);
    CHECK(read_bytes.size() == sizeof(dummy_data));
    CHECK(std::string(reinterpret_cast<char *>(read_bytes.data())) == dummy_data);

    // Sparse fixture: offset beyond 4 GiB without allocating or copying an asset.
    constexpr std::uint64_t large_offset = (std::uint64_t{1} << 32) + 17;
    {
        std::ofstream sparse(app_dir / "sparse.dat", std::ios::binary);
        sparse.seekp(static_cast<std::streamoff>(large_offset));
        sparse.write("XYZ", 3);
        CHECK(sparse.good());
    }
    std::array<char, 8> chunk{};
    std::size_t got = 99;
    CHECK(vfs.read_at("/app0/sparse.dat", large_offset, chunk.data(), chunk.size(), got) == VfsError::ok);
    CHECK(got == 3);
    CHECK(std::string(chunk.data(), got) == "XYZ");
    CHECK(vfs.read_at("/app0/sparse.dat", large_offset + 3, chunk.data(), chunk.size(), got) == VfsError::ok);
    CHECK(got == 0);
    CHECK(vfs.read_at("/app0/sparse.dat", UINT64_MAX, chunk.data(), chunk.size(), got) == VfsError::ok);
    CHECK(got == 0);
    CHECK(vfs.read_at("/app0/sparse.dat", 0, nullptr, 1, got) == VfsError::invalid_path);
    CHECK(got == 0);
    CHECK(vfs.read_at("/app0/../outside/secret.txt", 0, chunk.data(), chunk.size(), got) == VfsError::traversal_detected);
    CHECK(got == 0);
    CHECK(vfs.read_at("/app0/missing", 0, chunk.data(), chunk.size(), got) == VfsError::not_found);

    // Remoção
    CHECK(vfs.remove_file("/user/home/savedata/save01.dat") == VfsError::ok);
    CHECK(!vfs.exists("/user/home/savedata/save01.dat"));

    // Falha de stat preserva a saída do chamador.
    VfsStat unchanged{42, true, true};
    CHECK(vfs.stat("/user/home/savedata/missing", unchanged) == VfsError::not_found);
    CHECK(unchanged.size == 42);
    CHECK(unchanged.is_directory);

    // Um symlink dentro de Documents não pode escapar da raiz, nem para leitura
    // nem para criação de arquivos; symlinks internos continuam válidos.
    const auto outside = test_dir / "outside";
    std::filesystem::create_directories(outside, ec);
    { std::ofstream file(outside / "secret.txt"); file << "secret"; }
    std::filesystem::create_directory_symlink(outside, save_dir / "escape", ec);
    if (!ec) {
        CHECK(vfs.resolve("/user/home/savedata/escape/secret.txt", host_path, readonly) == VfsError::access_denied);
        CHECK(!vfs.exists("/user/home/savedata/escape/secret.txt"));
        CHECK(vfs.stat("/user/home/savedata/escape/secret.txt", st) == VfsError::access_denied);
        CHECK(vfs.read_file("/user/home/savedata/escape/secret.txt", read_bytes) == VfsError::access_denied);
        CHECK(vfs.read_at("/user/home/savedata/escape/secret.txt", 0, chunk.data(), chunk.size(), got) == VfsError::access_denied);
        CHECK(got == 0);
        CHECK(vfs.write_file("/user/home/savedata/escape/new.txt", dummy_data, sizeof(dummy_data)) == VfsError::access_denied);
        CHECK(!std::filesystem::exists(outside / "new.txt"));
    }
    ec.clear();
    std::filesystem::create_directories(save_dir / "nested", ec);
    std::filesystem::create_directory_symlink(save_dir / "nested", save_dir / "inside", ec);
    if (!ec) {
        CHECK(vfs.resolve("/user/home/savedata/inside/file.txt", host_path, readonly) == VfsError::ok);
        { std::ofstream file(save_dir / "nested" / "file.txt"); file << "inside"; }
        CHECK(vfs.read_at("/user/home/savedata/inside/file.txt", 2, chunk.data(), 3, got) == VfsError::ok);
        CHECK(got == 3);
        CHECK(std::string(chunk.data(), got) == "sid");
    }

    // Precedência de arquivos reais sobre stubs sintéticos (T058 / FR-004)
    {
        VirtualFileSystem prec_vfs;
        std::filesystem::path prec_root = test_dir / "prec_app";
        std::filesystem::create_directories(prec_root / "dvdroot_ps4" / "shader", ec);
        CHECK(prec_vfs.mount("/app0", prec_root.string(), true));

        // Define um stub sintético para gxgui.shaderbnd.dcx
        std::vector<std::uint8_t> synthetic_stub = {'S', 'Y', 'N', 'T', 'H'};
        CHECK(prec_vfs.set_synthetic_file("/app0/dvdroot_ps4/shader/gxgui.shaderbnd.dcx", synthetic_stub));

        // Sem arquivo real em disco: deve retornar o stub sintético
        std::vector<std::uint8_t> out_read;
        CHECK(prec_vfs.exists("/app0/dvdroot_ps4/shader/gxgui.shaderbnd.dcx"));
        CHECK(prec_vfs.read_file("/app0/dvdroot_ps4/shader/gxgui.shaderbnd.dcx", out_read) == VfsError::ok);
        CHECK(out_read == synthetic_stub);

        // Agora cria o arquivo real no sistema de arquivos do host
        const std::string real_content = "REAL_SHADER_PAYLOAD_FROM_DISK";
        {
            std::ofstream real_file(prec_root / "dvdroot_ps4" / "shader" / "gxgui.shaderbnd.dcx", std::ios::binary);
            real_file.write(real_content.data(), real_content.size());
        }

        // O arquivo real DEVE ter precedência sobre o stub sintético
        out_read.clear();
        CHECK(prec_vfs.exists("/app0/dvdroot_ps4/shader/gxgui.shaderbnd.dcx"));
        CHECK(prec_vfs.read_file("/app0/dvdroot_ps4/shader/gxgui.shaderbnd.dcx", out_read) == VfsError::ok);
        CHECK(std::string(reinterpret_cast<char *>(out_read.data()), out_read.size()) == real_content);

        VfsStat prec_st{};
        CHECK(prec_vfs.stat("/app0/dvdroot_ps4/shader/gxgui.shaderbnd.dcx", prec_st) == VfsError::ok);
        CHECK(prec_st.size == real_content.size());

        char prec_chunk[16]{};
        std::size_t prec_got = 0;
        CHECK(prec_vfs.read_at("/app0/dvdroot_ps4/shader/gxgui.shaderbnd.dcx", 0, prec_chunk, 4, prec_got) == VfsError::ok);
        CHECK(prec_got == 4);
        CHECK(std::string(prec_chunk, 4) == "REAL");

        // Remove o arquivo real: deve retornar ao stub sintético como fallback
        std::filesystem::remove(prec_root / "dvdroot_ps4" / "shader" / "gxgui.shaderbnd.dcx", ec);
        out_read.clear();
        CHECK(prec_vfs.exists("/app0/dvdroot_ps4/shader/gxgui.shaderbnd.dcx"));
        CHECK(prec_vfs.read_file("/app0/dvdroot_ps4/shader/gxgui.shaderbnd.dcx", out_read) == VfsError::ok);
        CHECK(out_read == synthetic_stub);
    }

    // ========================================================================
    // T085: Leitura incremental read_at para entradas BND4 no VFS
    // ========================================================================
    {
        // Constrói um container BND4 sintético com dois arquivos
        std::vector<std::uint8_t> bnd_raw(260, 0);
        std::memcpy(bnd_raw.data(), "BND4", 4);
        auto w32 = [](std::uint8_t *p, std::uint32_t v) {
            p[0] = static_cast<std::uint8_t>(v & 0xFF);
            p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
            p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
            p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
        };
        auto w64 = [&](std::uint8_t *p, std::uint64_t v) {
            w32(p, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
            w32(p + 4, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
        };
        w32(bnd_raw.data() + 12, 2);   // file_count = 2
        w64(bnd_raw.data() + 16, 64);  // header_size = 64
        std::memcpy(bnd_raw.data() + 24, "07D7R6\0\0", 8); // version
        w64(bnd_raw.data() + 32, 36);  // file_header_size = 36
        bnd_raw[0x28] = 2; // unicode_flag = 2 (ASCII)

        // Entrada 0 (data0.bin): offset 140, 40 bytes
        w64(bnd_raw.data() + 72, 40);
        w64(bnd_raw.data() + 80, 40);
        w32(bnd_raw.data() + 88, 140);
        w32(bnd_raw.data() + 92, 100);
        w32(bnd_raw.data() + 96, 210);

        // Entrada 1 (data1.bin): offset 180, 30 bytes
        w64(bnd_raw.data() + 108, 30);
        w64(bnd_raw.data() + 116, 30);
        w32(bnd_raw.data() + 124, 180);
        w32(bnd_raw.data() + 128, 200);
        w32(bnd_raw.data() + 132, 222);

        for (std::size_t i = 0; i < 40; ++i) bnd_raw[140 + i] = static_cast<std::uint8_t>(i + 1);
        for (std::size_t i = 0; i < 30; ++i) bnd_raw[180 + i] = static_cast<std::uint8_t>(i + 50);
        std::memcpy(bnd_raw.data() + 210, "data0.bin", 10);
        std::memcpy(bnd_raw.data() + 222, "data1.bin", 10);

        auto bnd_reader = std::make_shared<Bnd4Reader>();
        CHECK(bnd_reader->load(bnd_raw.data(), bnd_raw.size()) == BinderError::ok);

        VirtualFileSystem bnd_vfs;
        CHECK(bnd_vfs.mount_bnd4("/bnd", bnd_reader));
        CHECK(bnd_vfs.is_mounted("/bnd"));
        CHECK(bnd_vfs.exists("/bnd/data0.bin"));
        CHECK(bnd_vfs.exists("/bnd/data1.bin"));

        // Leitura integral via read_file
        std::vector<std::uint8_t> full_file;
        CHECK(bnd_vfs.read_file("/bnd/data0.bin", full_file) == VfsError::ok);
        CHECK(full_file.size() == 40);

        // Leitura incremental via read_at: paridade exata de conteúdo
        std::vector<std::uint8_t> incremental(40, 0);
        std::size_t read_bytes_count = 0;
        // Lê primeiros 15 bytes
        CHECK(bnd_vfs.read_at("/bnd/data0.bin", 0, incremental.data(), 15, read_bytes_count) == VfsError::ok);
        CHECK(read_bytes_count == 15);
        // Lê próximos 25 bytes
        CHECK(bnd_vfs.read_at("/bnd/data0.bin", 15, incremental.data() + 15, 25, read_bytes_count) == VfsError::ok);
        CHECK(read_bytes_count == 25);
        CHECK(incremental == full_file);

        // Testes de fronteira e limites
        // Leitura no EOF retorna ok com 0 bytes lidos
        CHECK(bnd_vfs.read_at("/bnd/data0.bin", 40, incremental.data(), 10, read_bytes_count) == VfsError::ok);
        CHECK(read_bytes_count == 0);
        // Leitura além do EOF retorna ok com 0 bytes lidos
        CHECK(bnd_vfs.read_at("/bnd/data0.bin", 100, incremental.data(), 10, read_bytes_count) == VfsError::ok);
        CHECK(read_bytes_count == 0);
        // Leitura com tamanho 0
        CHECK(bnd_vfs.read_at("/bnd/data0.bin", 0, nullptr, 0, read_bytes_count) == VfsError::ok);
        CHECK(read_bytes_count == 0);
        // Leitura com dest nulo e tamanho > 0 deve falhar
        CHECK(bnd_vfs.read_at("/bnd/data0.bin", 0, nullptr, 5, read_bytes_count) == VfsError::invalid_path);
        // Arquivo inexistente no mount BND4
        CHECK(bnd_vfs.read_at("/bnd/nonexistent.bin", 0, incremental.data(), 10, read_bytes_count) == VfsError::not_found);
    }

    // ========================================================================
    // T087: Armazenamento atômico e rollback em falhas de substituição no VFS
    // ========================================================================
    {
        std::filesystem::path atomic_test_dir = test_dir / "atomic_tests";
        std::filesystem::create_directories(atomic_test_dir, ec);
        VirtualFileSystem atomic_vfs;
        CHECK(atomic_vfs.mount("/atomic", atomic_test_dir.string(), false));

        // 1. Falha de criação de diretório quando o caminho pai é um arquivo comum
        std::string blocker_file = (atomic_test_dir / "blocker.dat").string();
        {
            std::ofstream out(blocker_file);
            out << "I am a file, not a directory";
        }
        CHECK(atomic_vfs.write_file("/atomic/blocker.dat/child.txt", "data", 4, true) == VfsError::io_error);
        CHECK(atomic_vfs.write_file("/atomic/blocker.dat/child.txt", "data", 4, false) == VfsError::io_error);

        // 2. Preservação do arquivo anterior quando a substituição atômica falha
        const std::string original_content = "ORIGINAL_DATA_V1_PRESERVED";
        CHECK(atomic_vfs.write_file("/atomic/target.dat", original_content.data(), original_content.size(), true) == VfsError::ok);
        CHECK(atomic_vfs.exists("/atomic/target.dat"));

        std::vector<std::uint8_t> check_read;
        CHECK(atomic_vfs.read_file("/atomic/target.dat", check_read) == VfsError::ok);
        CHECK(std::string(reinterpret_cast<char *>(check_read.data()), check_read.size()) == original_content);

        // Injeta falha no hook de rename simulando erro ao renomear tmp -> host
        atomic_vfs.set_rename_hook_for_testing([](const std::filesystem::path &from, const std::filesystem::path &to) -> std::error_code {
            if (from.string().find(".tmp.") != std::string::npos) {
                return std::make_error_code(std::errc::permission_denied);
            }
            std::error_code err;
            std::filesystem::rename(from, to, err);
            return err;
        });

        const std::string bad_payload = "CORRUPTED_OR_FAILED_NEW_DATA";
        VfsError write_res = atomic_vfs.write_file("/atomic/target.dat", bad_payload.data(), bad_payload.size(), true);
        CHECK(write_res == VfsError::io_error);

        // O arquivo original DEVE estar rigorosamente intacto
        check_read.clear();
        CHECK(atomic_vfs.exists("/atomic/target.dat"));
        CHECK(atomic_vfs.read_file("/atomic/target.dat", check_read) == VfsError::ok);
        CHECK(std::string(reinterpret_cast<char *>(check_read.data()), check_read.size()) == original_content);

        // Remove o hook e confirma que nova escrita atômica válida é aceita
        atomic_vfs.set_rename_hook_for_testing(nullptr);
        const std::string valid_update = "VALID_DATA_V2_UPDATED";
        CHECK(atomic_vfs.write_file("/atomic/target.dat", valid_update.data(), valid_update.size(), true) == VfsError::ok);
        check_read.clear();
        CHECK(atomic_vfs.read_file("/atomic/target.dat", check_read) == VfsError::ok);
        CHECK(std::string(reinterpret_cast<char *>(check_read.data()), check_read.size()) == valid_update);

        std::filesystem::remove_all(atomic_test_dir, ec);
    }

    // ========================================================================
    // T177: Rejeicao de data == nullptr com size > 0 (preservacao de bytes)
    // e escrita valida de 0 bytes nas rotas atomica e direta (FR-005)
    // ========================================================================
    {
        std::filesystem::path t177_dir = test_dir / "t177_test";
        std::filesystem::create_directories(t177_dir, ec);

        VirtualFileSystem t177_vfs;
        CHECK(t177_vfs.mount("/t177", t177_dir.string(), false));

        // 1. Rota direta (atomic = false)
        const std::string direct_orig = "ORIGINAL_DIRECT_CONTENT_BYTES_12345";
        CHECK(t177_vfs.write_file("/t177/direct.bin", direct_orig.data(), direct_orig.size(), false) == VfsError::ok);

        // Tentativa de sobrescrita direta com nullptr e size > 0 deve falhar
        CHECK(t177_vfs.write_file("/t177/direct.bin", nullptr, 128, false) == VfsError::io_error);

        // O arquivo original DEVE ser preservado byte a byte
        std::vector<uint8_t> read_direct;
        CHECK(t177_vfs.read_file("/t177/direct.bin", read_direct) == VfsError::ok);
        CHECK(std::string(reinterpret_cast<char *>(read_direct.data()), read_direct.size()) == direct_orig);

        // Tentativa de escrita direta em arquivo inexistente com nullptr e size > 0
        CHECK(t177_vfs.write_file("/t177/direct_missing.bin", nullptr, 64, false) == VfsError::io_error);
        CHECK(!std::filesystem::exists(t177_dir / "direct_missing.bin", ec));

        // Escrita direta de arquivo vazio valida (size = 0, data = nullptr)
        CHECK(t177_vfs.write_file("/t177/direct_empty.bin", nullptr, 0, false) == VfsError::ok);
        CHECK(std::filesystem::exists(t177_dir / "direct_empty.bin", ec));
        CHECK(std::filesystem::file_size(t177_dir / "direct_empty.bin", ec) == 0);

        // 2. Rota atomica (atomic = true)
        const std::string atomic_orig = "ORIGINAL_ATOMIC_CONTENT_BYTES_67890";
        CHECK(t177_vfs.write_file("/t177/atomic.bin", atomic_orig.data(), atomic_orig.size(), true) == VfsError::ok);

        // Tentativa de sobrescrita atomica com nullptr e size > 0 deve falhar
        CHECK(t177_vfs.write_file("/t177/atomic.bin", nullptr, 256, true) == VfsError::io_error);

        // O arquivo original DEVE ser preservado byte a byte
        std::vector<uint8_t> read_atomic;
        CHECK(t177_vfs.read_file("/t177/atomic.bin", read_atomic) == VfsError::ok);
        CHECK(std::string(reinterpret_cast<char *>(read_atomic.data()), read_atomic.size()) == atomic_orig);

        // Tentativa de escrita atomica em arquivo inexistente com nullptr e size > 0
        CHECK(t177_vfs.write_file("/t177/atomic_missing.bin", nullptr, 128, true) == VfsError::io_error);
        CHECK(!std::filesystem::exists(t177_dir / "atomic_missing.bin", ec));

        // Escrita atomica de arquivo vazio valida (size = 0, data = nullptr)
        CHECK(t177_vfs.write_file("/t177/atomic_empty.bin", nullptr, 0, true) == VfsError::ok);
        CHECK(std::filesystem::exists(t177_dir / "atomic_empty.bin", ec));
        CHECK(std::filesystem::file_size(t177_dir / "atomic_empty.bin", ec) == 0);

        t177_vfs.unmount("/t177");
        std::filesystem::remove_all(t177_dir, ec);
    }

    // ========================================================================
    // T170 e T176: Precedencia de read_at e Distincao de Erro de I/O de Not Found
    // ========================================================================
    {
        VirtualFileSystem prec_vfs;
        std::filesystem::path prec_dir = std::filesystem::current_path() / "test_vfs_prec";
        std::filesystem::remove_all(prec_dir, ec);
        std::filesystem::create_directories(prec_dir, ec);

        CHECK(prec_vfs.mount("/data", prec_dir.string(), false));

        // 1. Arquivo presente apenas no host
        const std::string host_content = "HOST_FILE_FOR_T170_AND_T176_DATA_STREAM";
        {
            std::ofstream f(prec_dir / "host_only.bin", std::ios::binary);
            f.write(host_content.data(), host_content.size());
        }

        // 2. Arquivo sintetico
        std::vector<uint8_t> syn_data = {'S', 'Y', 'N', '_', '1', '7', '0'};
        CHECK(prec_vfs.set_synthetic_file("/data/syn_only.bin", syn_data));

        // T170: read_at deve ler arquivo do host sob o mount com paridade
        std::vector<uint8_t> read_buf(host_content.size(), 0);
        std::size_t bytes_read = 0;
        CHECK(prec_vfs.read_at("/data/host_only.bin", 0, read_buf.data(), read_buf.size(), bytes_read) == VfsError::ok);
        CHECK(bytes_read == host_content.size());
        CHECK(std::string(read_buf.begin(), read_buf.end()) == host_content);

        // T170: Leitura incremental com offset via read_at
        std::vector<uint8_t> slice(10, 0);
        CHECK(prec_vfs.read_at("/data/host_only.bin", 5, slice.data(), 10, bytes_read) == VfsError::ok);
        CHECK(bytes_read == 10);
        CHECK(std::string(slice.begin(), slice.end()) == host_content.substr(5, 10));

        // T170: read_at deve ler arquivo sintetico sob o mount com paridade
        std::vector<uint8_t> syn_read(syn_data.size(), 0);
        CHECK(prec_vfs.read_at("/data/syn_only.bin", 0, syn_read.data(), syn_read.size(), bytes_read) == VfsError::ok);
        CHECK(bytes_read == syn_data.size());
        CHECK(syn_read == syn_data);

        // T170: Precedencia hierarquica com mount BND4 ativo concorrentemente sobre o mesmo prefixo /data
        {
            auto w32_local = [](std::uint8_t *p, std::uint32_t v) {
                p[0] = static_cast<std::uint8_t>(v & 0xFF);
                p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
                p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
                p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
            };
            auto w64_local = [&](std::uint8_t *p, std::uint64_t v) {
                w32_local(p, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
                w32_local(p + 4, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
            };

            // Criar BND4 contendo /data/archive_file.bin
            std::vector<std::uint8_t> bnd_prec(300, 0);
            std::memcpy(bnd_prec.data(), "BND4", 4);
            w32_local(bnd_prec.data() + 12, 1);   // 1 arquivo
            w64_local(bnd_prec.data() + 16, 64);  // header_size = 64
            std::memcpy(bnd_prec.data() + 24, "07D7R6\0\0", 8); // version
            w64_local(bnd_prec.data() + 32, 36);  // file_header_size = 36
            bnd_prec[0x28] = 2; // unicode_flag = 2 (ASCII)
            w64_local(bnd_prec.data() + 40, 64);

            // File header em offset 64
            w64_local(bnd_prec.data() + 72, 16);  // compressed_size
            w64_local(bnd_prec.data() + 80, 16);  // uncompressed_size
            w32_local(bnd_prec.data() + 88, 120); // data_offset
            w32_local(bnd_prec.data() + 92, 100);
            w32_local(bnd_prec.data() + 96, 160); // name_offset
            for (std::size_t i = 0; i < 16; ++i) bnd_prec[120 + i] = static_cast<std::uint8_t>(0xA0 + i);
            std::memcpy(bnd_prec.data() + 160, "archive_file.bin", 17);

            auto bnd_p_reader = std::make_shared<Bnd4Reader>();
            CHECK(bnd_p_reader->load(bnd_prec.data(), bnd_prec.size()) == BinderError::ok);
            CHECK(prec_vfs.mount_bnd4("/data", bnd_p_reader));

            // 1. Arquivo no archive tem precedencia
            std::vector<uint8_t> arch_read(16, 0);
            CHECK(prec_vfs.read_at("/data/archive_file.bin", 0, arch_read.data(), 16, bytes_read) == VfsError::ok);
            CHECK(bytes_read == 16);
            CHECK(arch_read[0] == 0xA0);

            // 2. Arquivo ausente no archive mas presente no host eh acessado com sucesso via read_at
            std::vector<uint8_t> host_fallback_read(host_content.size(), 0);
            CHECK(prec_vfs.read_at("/data/host_only.bin", 0, host_fallback_read.data(), host_fallback_read.size(), bytes_read) == VfsError::ok);
            CHECK(bytes_read == host_content.size());
            CHECK(std::string(host_fallback_read.begin(), host_fallback_read.end()) == host_content);

            // 3. Arquivo ausente no archive e no host mas presente em synthetic eh acessado com sucesso via read_at
            std::vector<uint8_t> syn_fallback_read(syn_data.size(), 0);
            CHECK(prec_vfs.read_at("/data/syn_only.bin", 0, syn_fallback_read.data(), syn_fallback_read.size(), bytes_read) == VfsError::ok);
            CHECK(bytes_read == syn_data.size());
            CHECK(syn_fallback_read == syn_data);

            // 4. stat confirma paridade com read_at e read_file em todos os 3 niveis de precedencia
            VfsStat st_arch{}, st_host{}, st_syn{};
            CHECK(prec_vfs.stat("/data/archive_file.bin", st_arch) == VfsError::ok);
            CHECK(st_arch.size == 16);
            CHECK(!st_arch.is_directory);

            CHECK(prec_vfs.stat("/data/host_only.bin", st_host) == VfsError::ok);
            CHECK(st_host.size == host_content.size());
            CHECK(!st_host.is_directory);

            CHECK(prec_vfs.stat("/data/syn_only.bin", st_syn) == VfsError::ok);
            CHECK(st_syn.size == syn_data.size());
            CHECK(!st_syn.is_directory);

            // 5. Arquivo inexistente em todos os 3 retorna not_found
            VfsStat st_none{};
            CHECK(prec_vfs.stat("/data/absent_everywhere.bin", st_none) == VfsError::not_found);
            CHECK(prec_vfs.read_at("/data/absent_everywhere.bin", 0, slice.data(), 10, bytes_read) == VfsError::not_found);
        }

        // T176: Se o caminho no host for um diretorio, read_file e read_at retornam io_error
        std::filesystem::create_directories(prec_dir / "sub_dir", ec);
        std::vector<uint8_t> dir_read;
        CHECK(prec_vfs.read_file("/data/sub_dir", dir_read) == VfsError::io_error);
        CHECK(prec_vfs.read_at("/data/sub_dir", 0, slice.data(), 10, bytes_read) == VfsError::io_error);

        // T176: Se houver stub sintetico com mesmo nome de um diretorio host, não mascarar o erro
        CHECK(prec_vfs.set_synthetic_file("/data/sub_dir", syn_data));
        CHECK(prec_vfs.read_file("/data/sub_dir", dir_read) == VfsError::io_error);
        CHECK(prec_vfs.read_at("/data/sub_dir", 0, slice.data(), 10, bytes_read) == VfsError::io_error);

        prec_vfs.unmount("/data");
        std::filesystem::remove_all(prec_dir, ec);
    }

    // Desmontagem
    CHECK(vfs.unmount("/app0"));
    CHECK(!vfs.is_mounted("/app0"));

    // Limpeza
    std::filesystem::remove_all(test_dir, ec);
}); }
