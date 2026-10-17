#include "check.h"
#include "binder.h"
#include "vfs.h"

#include <vector>
#include <string>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

static void write_le32_buf(std::uint8_t *p, std::uint32_t val) {
    p[0] = static_cast<std::uint8_t>(val & 0xFF);
    p[1] = static_cast<std::uint8_t>((val >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((val >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((val >> 24) & 0xFF);
}

static void write_le64_buf(std::uint8_t *p, std::uint64_t val) {
    write_le32_buf(p, static_cast<std::uint32_t>(val & 0xFFFFFFFFu));
    write_le32_buf(p + 4, static_cast<std::uint32_t>((val >> 32) & 0xFFFFFFFFu));
}

int main() { return run([] {
    using namespace bb;

    std::cout << "[test_binder] Iniciando suite de testes para BHD5, BDT, DCX e BND4...\n";

    // ========================================================================
    // 1. TESTES SINTETICOS DCX (COMPRESSAO / DESCOMPRESSAO / ROBUSTEZ)
    // ========================================================================
    {
        std::cout << "[test_binder] 1. Testando descompressao DCX sintetica...\n";

        // Gera carga de teste com padroes repetitivos e variados
        std::vector<std::uint8_t> original_payload(12000);
        for (std::size_t i = 0; i < original_payload.size(); ++i) {
            original_payload[i] = static_cast<std::uint8_t>((i * 7 + (i / 13)) & 0xFF);
        }

        std::vector<std::uint8_t> dcx_blob;
        CHECK(DcxDecompressor::create_dcx(original_payload.data(), original_payload.size(), dcx_blob) == BinderError::ok);
        CHECK(!dcx_blob.empty());
        CHECK(DcxDecompressor::is_dcx(dcx_blob.data(), dcx_blob.size()));

        DcxHeader header{};
        CHECK(DcxDecompressor::parse_header(dcx_blob.data(), dcx_blob.size(), header) == BinderError::ok);
        CHECK(header.algorithm == DcxAlgorithm::deflate);
        CHECK(header.uncompressed_size == original_payload.size());
        CHECK(std::string(header.algorithm_tag) == "DFLT");
        CHECK(header.data_offset == 0x4C);

        std::vector<std::uint8_t> decompressed;
        CHECK(DcxDecompressor::decompress(dcx_blob.data(), dcx_blob.size(), decompressed) == BinderError::ok);
        CHECK(decompressed.size() == original_payload.size());
        CHECK(decompressed == original_payload);

        // Testes de limites e rejeicao de corrupcao
        CHECK(!DcxDecompressor::is_dcx(nullptr, 0));
        CHECK(!DcxDecompressor::is_dcx(dcx_blob.data(), 10)); // Buffer menor que 0x4C

        DcxHeader bad_hdr{};
        CHECK(DcxDecompressor::parse_header(nullptr, 0, bad_hdr) == BinderError::out_of_bounds);

        std::vector<std::uint8_t> corrupted_magic = dcx_blob;
        corrupted_magic[0] = 'X';
        CHECK(DcxDecompressor::parse_header(corrupted_magic.data(), corrupted_magic.size(), bad_hdr) == BinderError::invalid_magic);

        std::vector<std::uint8_t> corrupted_dcs = dcx_blob;
        corrupted_dcs[0x18] = 'X'; // Quebra o bloco DCS
        CHECK(DcxDecompressor::parse_header(corrupted_dcs.data(), corrupted_dcs.size(), bad_hdr) == BinderError::invalid_header);

        std::vector<std::uint8_t> corrupted_data = dcx_blob;
        corrupted_data[0x50] ^= 0xFF; // Inverte bits do payload zlib
        corrupted_data[0x51] ^= 0xAA;
        std::vector<std::uint8_t> bad_out;
        CHECK(DcxDecompressor::decompress(corrupted_data.data(), corrupted_data.size(), bad_out) == BinderError::decompression_failed);

        // Teste de carga de 0 bytes
        std::vector<std::uint8_t> empty_dcx;
        CHECK(DcxDecompressor::create_dcx(nullptr, 0, empty_dcx) == BinderError::ok);
        CHECK(DcxDecompressor::is_dcx(empty_dcx.data(), empty_dcx.size()));
        std::vector<std::uint8_t> empty_out;
        CHECK(DcxDecompressor::decompress(empty_dcx.data(), empty_dcx.size(), empty_out) == BinderError::ok);
        CHECK(empty_out.empty());

        std::cout << "[test_binder] -> DCX sintetico e rejeicao de falhas validados com sucesso.\n";
    }

    // ========================================================================
    // 2. TESTES SINTETICOS BHD5 / BDT (LAYOUT 64-BIT, BUCKETS E STREAMING)
    // ========================================================================
    {
        std::cout << "[test_binder] 2. Testando BHD5 / BDT streaming e hashing...\n";

        // Validacao do algoritmo de hash FromSoftware
        std::string p1 = "/Data/Sound/BGM.fsb";
        std::string norm1 = Bhd5Reader::normalize_archive_path(p1);
        CHECK(norm1 == "\\data\\sound\\bgm.fsb");
        std::uint64_t h1 = Bhd5Reader::hash_path(p1);
        std::uint64_t h2 = Bhd5Reader::hash_path("\\DATA\\SOUND\\BGM.FSB");
        CHECK(h1 != 0);
        CHECK(h1 == h2); // Insensivel a maiusculas e separador de diretorio

        std::filesystem::path sandbox = std::filesystem::current_path() / "test_binder_sandbox";
        std::error_code ec;
        std::filesystem::create_directories(sandbox, ec);

        std::filesystem::path bhd_file = sandbox / "archive.bhd5";
        std::filesystem::path bdt_file = sandbox / "archive.bdt";

        // Cria dados para 3 arquivos no BDT
        std::vector<std::uint8_t> file0_data(80, 0xAA);
        std::vector<std::uint8_t> file1_data(200, 0xBB);
        std::vector<std::uint8_t> file2_data(150, 0xCC);

        // Preenche sequencias reconheciveis
        for (std::size_t i = 0; i < file1_data.size(); ++i) file1_data[i] = static_cast<std::uint8_t>(i & 0xFF);
        for (std::size_t i = 0; i < file2_data.size(); ++i) file2_data[i] = static_cast<std::uint8_t>((i + 50) & 0xFF);

        // Monta o arquivo BDT em disco com offsets alinhados
        {
            std::ofstream bdt_out(std::filesystem::u8path(bdt_file.string()), std::ios::binary);
            // Offset 0: file0 (80 bytes)
            bdt_out.write(reinterpret_cast<const char *>(file0_data.data()), file0_data.size());
            // Padding ate 128
            std::vector<char> pad(48, 0);
            bdt_out.write(pad.data(), pad.size());
            // Offset 128: file1 (200 bytes)
            bdt_out.write(reinterpret_cast<const char *>(file1_data.data()), file1_data.size());
            // Padding ate 384
            pad.assign(56, 0);
            bdt_out.write(pad.data(), pad.size());
            // Offset 384: file2 (150 bytes)
            bdt_out.write(reinterpret_cast<const char *>(file2_data.data()), file2_data.size());
        }

        // Monta o arquivo BHD5: 2 buckets, layout de 24 bytes por registro
        // Header: 24 bytes
        // Buckets: 2 * 8 = 16 bytes (offset 24 a 40)
        // Registros bucket 0: 2 arquivos (offset 40 a 88)
        // Registros bucket 1: 1 arquivo (offset 88 a 112)
        std::vector<std::uint8_t> bhd_data(112, 0);
        std::memcpy(bhd_data.data(), "BHD5", 4);
        bhd_data[4] = 0xFF; // Flag padrao Bloodborne
        write_le32_buf(bhd_data.data() + 12, 2);  // bucket_count = 2
        write_le32_buf(bhd_data.data() + 16, 24); // bucket_offset = 24
        write_le32_buf(bhd_data.data() + 20, 112);

        // Bucket 0: 2 arquivos no offset 40
        write_le32_buf(bhd_data.data() + 24, 2);
        write_le32_buf(bhd_data.data() + 28, 40);

        // Bucket 1: 1 arquivo no offset 88
        write_le32_buf(bhd_data.data() + 32, 1);
        write_le32_buf(bhd_data.data() + 36, 88);

        std::string path0 = "param/gameparam.parambnd.dcx";
        std::string path1 = "map/m10_00_00_00.msb";
        std::string path2 = "script/event.emevd";

        std::uint64_t hash0 = Bhd5Reader::hash_path(path0);
        std::uint64_t hash1 = Bhd5Reader::hash_path(path1);
        std::uint64_t hash2 = Bhd5Reader::hash_path(path2);

        // Bucket 0 - Registro 0 (file0)
        write_le64_buf(bhd_data.data() + 40, hash0);
        write_le32_buf(bhd_data.data() + 48, 128); // padded_size
        write_le32_buf(bhd_data.data() + 52, static_cast<std::uint32_t>(file0_data.size()));
        write_le64_buf(bhd_data.data() + 56, 0);   // offset no BDT

        // Bucket 0 - Registro 1 (file1)
        write_le64_buf(bhd_data.data() + 64, hash1);
        write_le32_buf(bhd_data.data() + 72, 256);
        write_le32_buf(bhd_data.data() + 76, static_cast<std::uint32_t>(file1_data.size()));
        write_le64_buf(bhd_data.data() + 80, 128); // offset 128 no BDT

        // Bucket 1 - Registro 2 (file2)
        write_le64_buf(bhd_data.data() + 88, hash2);
        write_le32_buf(bhd_data.data() + 96, 256);
        write_le32_buf(bhd_data.data() + 100, static_cast<std::uint32_t>(file2_data.size()));
        write_le64_buf(bhd_data.data() + 104, 384); // offset 384 no BDT

        {
            std::ofstream bhd_out(std::filesystem::u8path(bhd_file.string()), std::ios::binary);
            bhd_out.write(reinterpret_cast<const char *>(bhd_data.data()), bhd_data.size());
        }

        // Abre o leitor BHD5 apontando para o par (.bhd5, .bdt)
        Bhd5Reader reader;
        CHECK(reader.open(bhd_file.string(), bdt_file.string()) == BinderError::ok);
        CHECK(reader.is_open());
        CHECK(reader.bucket_count() == 2);
        CHECK(reader.file_count() == 3);

        CHECK(reader.has_file(path0));
        CHECK(reader.has_file(path1));
        CHECK(reader.has_file(path2));
        CHECK(!reader.has_file("nonexistent/asset.bin"));

        const auto *e1 = reader.get_entry(path1);
        CHECK(e1 != nullptr);
        CHECK(e1->file_size == file1_data.size());
        CHECK(e1->file_offset == 128);

        // Leitura completa de arquivos
        std::vector<std::uint8_t> read0;
        CHECK(reader.read_file(path0, read0) == BinderError::ok);
        CHECK(read0 == file0_data);

        std::vector<std::uint8_t> read1;
        CHECK(reader.read_file(path1, read1) == BinderError::ok);
        CHECK(read1 == file1_data);

        // Streaming sob demanda (leitura parcial por fatias sem carregar tudo)
        std::vector<std::uint8_t> slice(60, 0);
        std::size_t bytes_streamed = 0;
        // Le 60 bytes a partir do offset 20 do arquivo 1
        CHECK(reader.stream_file(path1, 20, slice.data(), slice.size(), bytes_streamed) == BinderError::ok);
        CHECK(bytes_streamed == 60);
        std::vector<std::uint8_t> expected_slice(file1_data.begin() + 20, file1_data.begin() + 80);
        CHECK(slice == expected_slice);

        // Streaming no final do arquivo (clamp)
        std::vector<std::uint8_t> end_slice(100, 0);
        CHECK(reader.stream_file(path1, 180, end_slice.data(), end_slice.size(), bytes_streamed) == BinderError::ok);
        CHECK(bytes_streamed == 20); // 200 - 180 = 20
        std::vector<std::uint8_t> expected_end(file1_data.begin() + 180, file1_data.end());
        end_slice.resize(bytes_streamed);
        CHECK(end_slice == expected_end);

        // Streaming alem do final do arquivo
        CHECK(reader.stream_file(path1, 300, slice.data(), slice.size(), bytes_streamed) == BinderError::ok);
        CHECK(bytes_streamed == 0);

        std::cout << "[test_binder] -> BHD5 / BDT ingestao e streaming validados com sucesso.\n";
    }

    // ========================================================================
    // 3. TESTES SINTETICOS BND4 (BINDER V4 CONTÊINER)
    // ========================================================================
    {
        std::cout << "[test_binder] 3. Testando contêiner BND4 sintetico...\n";

        // Layout de um BND4 minimo com 2 arquivos:
        // Header: 64 bytes
        // 2 File headers (36 bytes cada): 72 bytes (offsets 64..135)
        // File 0 data (140..179): 40 bytes
        // File 1 data (180..209): 30 bytes
        // Names:
        // Name 0 (210..220): "data0.bin\0"
        // Name 1 (222..232): "data1.bin\0"
        std::vector<std::uint8_t> bnd_raw(260, 0);
        std::memcpy(bnd_raw.data(), "BND4", 4);
        write_le32_buf(bnd_raw.data() + 12, 2);   // file_count = 2
        write_le64_buf(bnd_raw.data() + 16, 64);  // header_size = 64
        std::memcpy(bnd_raw.data() + 24, "07D7R6\0\0", 8); // version
        write_le64_buf(bnd_raw.data() + 32, 36);  // file_header_size = 36
        bnd_raw[0x28] = 2; // unicode_flag = 2 (ASCII)

        // File entry 0 (offset 64):
        write_le64_buf(bnd_raw.data() + 72, 40);  // compressed_size
        write_le64_buf(bnd_raw.data() + 80, 40);  // uncompressed_size
        write_le32_buf(bnd_raw.data() + 88, 140); // data_offset
        write_le32_buf(bnd_raw.data() + 92, 100); // id = 100
        write_le32_buf(bnd_raw.data() + 96, 210); // name_offset

        // File entry 1 (offset 100):
        write_le64_buf(bnd_raw.data() + 108, 30); // compressed_size
        write_le64_buf(bnd_raw.data() + 116, 30); // uncompressed_size
        write_le32_buf(bnd_raw.data() + 124, 180); // data_offset
        write_le32_buf(bnd_raw.data() + 128, 200); // id = 200
        write_le32_buf(bnd_raw.data() + 132, 222); // name_offset

        // Preenche dados dos arquivos
        for (std::size_t i = 0; i < 40; ++i) bnd_raw[140 + i] = static_cast<std::uint8_t>(i + 1);
        for (std::size_t i = 0; i < 30; ++i) bnd_raw[180 + i] = static_cast<std::uint8_t>(i + 50);

        // Escreve nomes
        std::memcpy(bnd_raw.data() + 210, "data0.bin", 10);
        std::memcpy(bnd_raw.data() + 222, "data1.bin", 10);

        Bnd4Reader bnd;
        CHECK(bnd.load(bnd_raw.data(), bnd_raw.size()) == BinderError::ok);
        CHECK(bnd.is_open());
        CHECK(bnd.file_count() == 2);
        CHECK(bnd.version() == "07D7R6");

        CHECK(bnd.has_file(100));
        CHECK(bnd.has_file(200));
        CHECK(!bnd.has_file(300));

        CHECK(bnd.has_file("data0.bin"));
        CHECK(bnd.has_file("data1.bin"));

        std::vector<std::uint8_t> out0;
        CHECK(bnd.read_file(100, out0) == BinderError::ok);
        CHECK(out0.size() == 40);
        CHECK(out0[0] == 1);
        CHECK(out0[39] == 40);

        std::vector<std::uint8_t> out1;
        CHECK(bnd.read_file("data1.bin", out1) == BinderError::ok);
        CHECK(out1.size() == 30);
        CHECK(out1[0] == 50);
        CHECK(out1[29] == 79);

        std::cout << "[test_binder] -> BND4 sintetico validado com sucesso.\n";
    }

    // ========================================================================
    // 4. INTEGRACAO VFS COM BHD5 E BND4 (CAMADA DE TRANSPARENCIA VIRTUAL)
    // ========================================================================
    {
        std::cout << "[test_binder] 4. Testando integracao transparente com VFS...\n";

        std::filesystem::path sandbox = std::filesystem::current_path() / "test_binder_sandbox";
        std::filesystem::path bhd_file = sandbox / "archive.bhd5";
        std::filesystem::path bdt_file = sandbox / "archive.bdt";

        auto bhd_reader = std::make_shared<Bhd5Reader>();
        CHECK(bhd_reader->open(bhd_file.string(), bdt_file.string()) == BinderError::ok);

        VirtualFileSystem vfs;
        CHECK(vfs.mount_bhd5("/dvdroot", bhd_reader));
        CHECK(vfs.is_mounted("/dvdroot"));

        // Consulta existencia de arquivo dentro do BHD5 atraves do caminho virtual
        CHECK(vfs.exists("/dvdroot/param/gameparam.parambnd.dcx"));
        CHECK(vfs.exists("/dvdroot/map/m10_00_00_00.msb"));
        CHECK(!vfs.exists("/dvdroot/unknown.bin"));

        // Consulta de metadados stat via VFS
        VfsStat st{};
        CHECK(vfs.stat("/dvdroot/map/m10_00_00_00.msb", st) == VfsError::ok);
        CHECK(st.size == 200);
        CHECK(st.is_readonly == true);
        CHECK(!st.is_directory);

        // Leitura de arquivo atraves da interface de alto nivel do VFS
        std::vector<std::uint8_t> vfs_data;
        CHECK(vfs.read_file("/dvdroot/map/m10_00_00_00.msb", vfs_data) == VfsError::ok);
        CHECK(vfs_data.size() == 200);
        CHECK(vfs_data[0] == 0);
        CHECK(vfs_data[199] == 199);
        std::uint8_t small[12] = {};
        std::size_t got = 0;
        CHECK(vfs.read_at("/dvdroot/map/m10_00_00_00.msb", 30, small, sizeof(small), got) == VfsError::ok);
        CHECK(got == sizeof(small) && small[0] == 30 && small[11] == 41);
        CHECK(vfs.read_at("/dvdroot/map/m10_00_00_00.msb", 196, small, sizeof(small), got) == VfsError::ok);
        CHECK(got == 4 && small[0] == 196 && small[3] == 199);
        CHECK(vfs.read_at("/dvdroot/map/m10_00_00_00.msb", UINT64_MAX, small, sizeof(small), got) == VfsError::ok);
        CHECK(got == 0);
        CHECK(vfs.read_at("/dvdroot/map/m10_00_00_00.msb", 0, nullptr, 1, got) == VfsError::invalid_path);
        CHECK(vfs.read_at("/dvdroot/unknown.bin", 0, small, sizeof(small), got) == VfsError::not_found);
        CHECK(vfs.read_at("/dvdroot/../unknown.bin", 0, small, sizeof(small), got) == VfsError::traversal_detected);

        // T118: Precedência e sobreposição de BND4 sobreposto no mesmo mount point com BHD5
        {
            std::vector<std::uint8_t> bnd_overlap_raw(260, 0);
            std::memcpy(bnd_overlap_raw.data(), "BND4", 4);
            write_le32_buf(bnd_overlap_raw.data() + 12, 2);
            write_le64_buf(bnd_overlap_raw.data() + 16, 64);
            std::memcpy(bnd_overlap_raw.data() + 24, "07D7R6\0\0", 8);
            write_le64_buf(bnd_overlap_raw.data() + 32, 36);
            bnd_overlap_raw[0x28] = 2;

            write_le64_buf(bnd_overlap_raw.data() + 72, 40);
            write_le64_buf(bnd_overlap_raw.data() + 80, 40);
            write_le32_buf(bnd_overlap_raw.data() + 88, 140);
            write_le32_buf(bnd_overlap_raw.data() + 92, 100);
            write_le32_buf(bnd_overlap_raw.data() + 96, 210);

            write_le64_buf(bnd_overlap_raw.data() + 108, 30);
            write_le64_buf(bnd_overlap_raw.data() + 116, 30);
            write_le32_buf(bnd_overlap_raw.data() + 124, 180);
            write_le32_buf(bnd_overlap_raw.data() + 128, 200);
            write_le32_buf(bnd_overlap_raw.data() + 132, 222);

            for (std::size_t i = 0; i < 40; ++i) bnd_overlap_raw[140 + i] = static_cast<std::uint8_t>(i + 1);
            for (std::size_t i = 0; i < 30; ++i) bnd_overlap_raw[180 + i] = static_cast<std::uint8_t>(i + 50);

            std::memcpy(bnd_overlap_raw.data() + 210, "data0.bin", 10);
            std::memcpy(bnd_overlap_raw.data() + 222, "data1.bin", 10);

            auto bnd_reader = std::make_shared<Bnd4Reader>();
            CHECK(bnd_reader->load(bnd_overlap_raw.data(), bnd_overlap_raw.size()) == BinderError::ok);

            CHECK(vfs.mount_bnd4("/dvdroot", bnd_reader));
            // map/m10_00_00_00.msb existe no BHD5
            std::size_t bhd_got = 0;
            std::uint8_t bhd_buf[16] = {0};
            CHECK(vfs.read_at("/dvdroot/map/m10_00_00_00.msb", 0, bhd_buf, sizeof(bhd_buf), bhd_got) == VfsError::ok);
            CHECK(bhd_got == sizeof(bhd_buf));

            // data0.bin NÃO existe no BHD5, mas existe no BND4 sobreposto
            CHECK(vfs.has_archive_or_synthetic_file("/dvdroot/data0.bin"));
            VfsStat bnd_st{};
            CHECK(vfs.stat("/dvdroot/data0.bin", bnd_st) == VfsError::ok);
            CHECK(bnd_st.size == 40);
            std::vector<std::uint8_t> bnd_full;
            CHECK(vfs.read_file("/dvdroot/data0.bin", bnd_full) == VfsError::ok);
            CHECK(bnd_full.size() == 40);

            // read_at deve alinhar com read_file e ler com sucesso do BND4 sobreposto
            std::vector<std::uint8_t> bnd_read_at(40, 0);
            std::size_t bnd_got_bytes = 0;
            CHECK(vfs.read_at("/dvdroot/data0.bin", 0, bnd_read_at.data(), 40, bnd_got_bytes) == VfsError::ok);
            CHECK(bnd_got_bytes == 40);
            CHECK(bnd_read_at == bnd_full);

            // Leitura incremental de 15 bytes a partir do offset 10
            std::vector<std::uint8_t> bnd_slice(15, 0);
            CHECK(vfs.read_at("/dvdroot/data0.bin", 10, bnd_slice.data(), 15, bnd_got_bytes) == VfsError::ok);
            CHECK(bnd_got_bytes == 15);
            CHECK(std::memcmp(bnd_slice.data(), bnd_full.data() + 10, 15) == 0);

            // Arquivo que não existe em nenhum dos dois
            CHECK(vfs.read_at("/dvdroot/neither.bin", 0, small, sizeof(small), got) == VfsError::not_found);
        }

        // Desmontagem
        CHECK(vfs.unmount("/dvdroot"));
        CHECK(!vfs.is_mounted("/dvdroot"));
        CHECK(!vfs.exists("/dvdroot/map/m10_00_00_00.msb"));

        // Fecha arquivo antes de remover o diretorio no Windows
        bhd_reader->close();
        bhd_reader = nullptr;

        // Limpa arquivos temporarios do sandbox
        std::error_code ec;
        std::filesystem::remove_all(sandbox, ec);

        std::cout << "[test_binder] -> Integracao VFS com BHD5 / BND4 validada com sucesso.\n";
    }

    // ========================================================================
    // 5. VALIDACAO COM ARQUIVOS REAIS DO BLOODBORNE (item.msgbnd.dcx)
    // ========================================================================
    {
        std::cout << "[test_binder] 5. Verificando ativos reais do Bloodborne no host...\n";

        // Procura item.msgbnd.dcx em caminhos comuns do projeto
        std::vector<std::string> candidate_paths = {
            "../../bloodborne_portPC/blodboorn 3171/Bloodborne\xe2\x84\xa2/Image0/dvdroot_ps4/msg/zhocn/item.msgbnd.dcx",
            "../bloodborne_portPC/blodboorn 3171/Bloodborne\xe2\x84\xa2/Image0/dvdroot_ps4/msg/zhocn/item.msgbnd.dcx",
            "bloodborne_portPC/blodboorn 3171/Bloodborne\xe2\x84\xa2/Image0/dvdroot_ps4/msg/zhocn/item.msgbnd.dcx",
            "C:/Users/Usuario/Documents/bloodborne_iphone/bloodborne_portPC/blodboorn 3171/Bloodborne\xe2\x84\xa2/Image0/dvdroot_ps4/msg/zhocn/item.msgbnd.dcx",
            "C:/Users/Usuario/Documents/bloodborne_iphone/bloodborne_portPC/blodboorn 3171/Bloodborne\xe2\x84\xa2/Image0/dvdroot_ps4/msg/porbr/item.msgbnd.dcx",
            "C:/Users/Usuario/Documents/bloodborne_iphone/bloodborne_portPC/blodboorn 3171/Bloodborne\xe2\x84\xa2/Image0/dvdroot_ps4/msg/engus/item.msgbnd.dcx"
        };

        std::string real_asset_path;
        for (const auto &p : candidate_paths) {
            std::error_code ec;
            if (std::filesystem::exists(std::filesystem::u8path(p), ec)) {
                real_asset_path = p;
                break;
            }
        }

        if (!real_asset_path.empty()) {
            std::cout << "[test_binder] Ativo real encontrado: " << real_asset_path << "\n";
            CHECK(DcxDecompressor::is_dcx_file(real_asset_path));

            std::vector<std::uint8_t> decompressed_bnd4;
            BinderError dec_err = DcxDecompressor::decompress_file(real_asset_path, decompressed_bnd4);
            CHECK(dec_err == BinderError::ok);
            CHECK(decompressed_bnd4.size() > 50000); // BND4 descomprimido tem varias centenas de KB
            std::cout << "[test_binder] -> DCX descomprimido com sucesso (" << decompressed_bnd4.size() << " bytes)\n";

            auto bnd_reader = std::make_shared<Bnd4Reader>();
            BinderError bnd_err = bnd_reader->load(decompressed_bnd4.data(), decompressed_bnd4.size());
            CHECK(bnd_err == BinderError::ok);
            CHECK(bnd_reader->file_count() > 0);
            std::cout << "[test_binder] -> BND4 carregado com " << bnd_reader->file_count() << " arquivos internos.\n";

            // Lista e valida os arquivos internos (.fmg de textos e mensagens)
            auto entries = bnd_reader->entries();
            CHECK(!entries.empty());
            for (std::size_t i = 0; i < std::min<std::size_t>(5, entries.size()); ++i) {
                std::cout << "    [entry " << i << "] ID=" << entries[i].id
                          << ", Nome=" << entries[i].name
                          << ", Tam=" << entries[i].compressed_size << "\n";
            }

            // Extrai o primeiro arquivo do BND4 e confere integridade
            std::vector<std::uint8_t> fmg_data;
            CHECK(bnd_reader->read_file(entries[0].id, fmg_data) == BinderError::ok);
            CHECK(fmg_data.size() == entries[0].compressed_size);

            // Monta o BND4 no VFS e testa extracao virtual
            VirtualFileSystem vfs;
            CHECK(vfs.mount_bnd4("/msg", bnd_reader));
            CHECK(vfs.is_mounted("/msg"));
            CHECK(vfs.exists("/msg/" + entries[0].name));

            std::vector<std::uint8_t> vfs_fmg;
            CHECK(vfs.read_file("/msg/" + entries[0].name, vfs_fmg) == VfsError::ok);
            CHECK(vfs_fmg == fmg_data);

            std::cout << "[test_binder] -> Validacao completa com ativo real do Bloodborne concluida com sucesso!\n";
        } else {
            std::cout << "[test_binder] Aviso: Ativo real nao localizado nos caminhos padrao; etapa real ignorada de forma limpa.\n";
        }
    }

    std::cout << "[test_binder] Todos os testes passaram com 100% de sucesso!\n";
}); }
