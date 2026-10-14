#include "check.h"
#include "vfs.h"
#include <filesystem>
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

    // Desmontagem
    CHECK(vfs.unmount("/app0"));
    CHECK(!vfs.is_mounted("/app0"));

    // Limpeza
    std::filesystem::remove_all(test_dir, ec);
}); }
