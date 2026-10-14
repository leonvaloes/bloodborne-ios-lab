#include "check.h"
#include "game_package.h"
#include "vfs.h"
#include "binder.h"

#include <vector>
#include <string>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <algorithm>

static void write_le32_bytes(std::uint8_t *p, std::uint32_t val) {
    p[0] = static_cast<std::uint8_t>(val & 0xFF);
    p[1] = static_cast<std::uint8_t>((val >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((val >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((val >> 24) & 0xFF);
}

static void write_le64_bytes(std::uint8_t *p, std::uint64_t val) {
    write_le32_bytes(p, static_cast<std::uint32_t>(val & 0xFFFFFFFFu));
    write_le32_bytes(p + 4, static_cast<std::uint32_t>((val >> 32) & 0xFFFFFFFFu));
}

struct SyntheticAsset {
    std::string path;
    std::vector<std::uint8_t> data;
};

// Helper para sintetizar contêineres BHD5 (layout 64-bit FromSoftware) e BDT alinhados
static bool create_synthetic_bhd5_bdt(
    const std::filesystem::path &bhd_path,
    const std::filesystem::path &bdt_path,
    const std::vector<SyntheticAsset> &assets
) {
    // 1. Escreve arquivo de dados BDT
    std::ofstream bdt_out(std::filesystem::u8path(bdt_path.string()), std::ios::binary);
    if (!bdt_out.is_open()) return false;

    struct Rec {
        std::uint64_t hash;
        std::uint32_t padded_size;
        std::uint32_t file_size;
        std::uint64_t file_offset;
    };
    std::vector<Rec> recs;
    recs.reserve(assets.size());

    std::uint64_t current_offset = 0;
    for (const auto &item : assets) {
        Rec r;
        r.hash = bb::Bhd5Reader::hash_path(item.path);
        r.file_size = static_cast<std::uint32_t>(item.data.size());
        r.padded_size = (r.file_size + 15) & ~15u;
        if (r.padded_size == 0) r.padded_size = 16;
        r.file_offset = current_offset;

        if (!item.data.empty()) {
            bdt_out.write(reinterpret_cast<const char *>(item.data.data()), item.data.size());
        }
        std::size_t pad = r.padded_size - r.file_size;
        if (pad > 0) {
            std::vector<char> pad_buf(pad, 0);
            bdt_out.write(pad_buf.data(), pad);
        }
        current_offset += r.padded_size;
        recs.push_back(r);
    }
    bdt_out.close();

    // 2. Escreve cabeçalho BHD5:
    // Header: 24 bytes
    // Buckets: 1 bucket (8 bytes: count=recs.size(), offset=32)
    // Registros: recs.size() * 24 bytes no offset 32
    std::uint32_t bucket_count = 1;
    std::uint32_t bucket_offset = 24;
    std::uint32_t records_offset = 32;
    std::uint32_t total_bhd_size = records_offset + static_cast<std::uint32_t>(recs.size() * 24);

    std::vector<std::uint8_t> bhd_buf(total_bhd_size, 0);
    std::memcpy(bhd_buf.data(), "BHD5", 4);
    bhd_buf[4] = 0xFF;
    write_le32_bytes(bhd_buf.data() + 12, bucket_count);
    write_le32_bytes(bhd_buf.data() + 16, bucket_offset);
    write_le32_bytes(bhd_buf.data() + 20, total_bhd_size);

    // Bucket 0
    write_le32_bytes(bhd_buf.data() + bucket_offset, static_cast<std::uint32_t>(recs.size()));
    write_le32_bytes(bhd_buf.data() + bucket_offset + 4, records_offset);

    // Registros de arquivo
    for (std::size_t i = 0; i < recs.size(); ++i) {
        std::uint8_t *rec_ptr = bhd_buf.data() + records_offset + i * 24;
        write_le64_bytes(rec_ptr, recs[i].hash);
        write_le32_bytes(rec_ptr + 8, recs[i].padded_size);
        write_le32_bytes(rec_ptr + 12, recs[i].file_size);
        write_le64_bytes(rec_ptr + 16, recs[i].file_offset);
    }

    std::ofstream bhd_out(std::filesystem::u8path(bhd_path.string()), std::ios::binary);
    if (!bhd_out.is_open()) return false;
    bhd_out.write(reinterpret_cast<const char *>(bhd_buf.data()), bhd_buf.size());
    bhd_out.close();

    return true;
}

int main() { return run([] {
    using namespace bb;

    std::cout << "[test_game_package] Iniciando suite de testes para GamePackageManager...\n";

    // ========================================================================
    // 1. CLASSIFICACAO E PRIORIZACAO DETERMINISTICA
    // ========================================================================
    {
        std::cout << "[test_game_package] 1. Testando deteccao de tipo e priorizacao de pacotes...\n";

        CHECK(GamePackageManager::detect_package_type("data0") == PackageType::base);
        CHECK(GamePackageManager::detect_package_type("data1") == PackageType::base);
        CHECK(GamePackageManager::detect_package_type("patch") == PackageType::patch);
        CHECK(GamePackageManager::detect_package_type("data0_patch") == PackageType::patch);
        CHECK(GamePackageManager::detect_package_type("dlc0") == PackageType::dlc);
        CHECK(GamePackageManager::detect_package_type("dlc_expansion") == PackageType::dlc);

        int prio_patch = GamePackageManager::default_priority_for(PackageType::patch, "patch");
        int prio_patch1 = GamePackageManager::default_priority_for(PackageType::patch, "patch1");
        int prio_dlc0 = GamePackageManager::default_priority_for(PackageType::dlc, "dlc0");
        int prio_dlc1 = GamePackageManager::default_priority_for(PackageType::dlc, "dlc1");
        int prio_data0 = GamePackageManager::default_priority_for(PackageType::base, "data0");
        int prio_data1 = GamePackageManager::default_priority_for(PackageType::base, "data1");

        // Precedência estrita: Patch > DLC > Base
        CHECK(prio_patch > prio_dlc1);
        CHECK(prio_dlc0 > prio_data1);
        CHECK(prio_patch1 > prio_patch);
        CHECK(prio_dlc1 > prio_dlc0);
        CHECK(prio_data1 > prio_data0);

        std::cout << "    [ok] Prioridades calculadas: patch1=" << prio_patch1
                  << ", patch=" << prio_patch << ", dlc1=" << prio_dlc1
                  << ", dlc0=" << prio_dlc0 << ", data1=" << prio_data1
                  << ", data0=" << prio_data0 << "\n";
    }

    // ========================================================================
    // 2. VARREDURA ESTRUTURADA DE DIRETORIOS E EMPARELHAMENTO .bhd5/.bdt
    // ========================================================================
    std::filesystem::path sandbox = std::filesystem::current_path() / "test_game_package_sandbox";
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
    std::filesystem::create_directories(sandbox / "dvdroot_ps4", ec);

    std::filesystem::path data0_bhd = sandbox / "dvdroot_ps4" / "data0.bhd5";
    std::filesystem::path data0_bdt = sandbox / "dvdroot_ps4" / "data0.bdt";
    std::filesystem::path data1_bhd = sandbox / "dvdroot_ps4" / "data1.bhd5";
    std::filesystem::path data1_bdt = sandbox / "dvdroot_ps4" / "data1.bdt";
    std::filesystem::path patch_bhd = sandbox / "dvdroot_ps4" / "patch.bhd5";
    std::filesystem::path patch_bdt = sandbox / "dvdroot_ps4" / "patch.bdt";
    std::filesystem::path dlc0_bhd = sandbox / "dvdroot_ps4" / "dlc0.bhd5";
    std::filesystem::path dlc0_bdt = sandbox / "dvdroot_ps4" / "dlc0.bdt";

    // Cargas de teste sintéticas
    std::vector<std::uint8_t> base_gameparam = {'G', 'A', 'M', 'E', 'P', 'A', 'R', 'A', 'M', '_', 'V', '1'};
    std::vector<std::uint8_t> patch_gameparam = {'G', 'A', 'M', 'E', 'P', 'A', 'R', 'A', 'M', '_', 'V', '2', '_', 'P', 'A', 'T', 'C', 'H'};
    std::vector<std::uint8_t> drawparam_data = {'D', 'R', 'A', 'W', 'P', 'A', 'R', 'A', 'M', '_', 'O', 'K'};
    std::vector<std::uint8_t> item_msg_data = {'I', 'T', 'E', 'M', '_', 'M', 'S', 'G', '_', 'E', 'N', 'G'};
    std::vector<std::uint8_t> menu_msg_data = {'M', 'E', 'N', 'U', '_', 'M', 'S', 'G', '_', 'E', 'N', 'G'};
    std::vector<std::uint8_t> map_m10_data = {'M', 'A', 'P', '_', 'M', '1', '0', '_', 'H', 'U', 'N', 'T', 'E', 'R'};
    std::vector<std::uint8_t> event_data = {'E', 'V', 'E', 'N', 'T', '_', 'C', 'O', 'M', 'M', 'O', 'N'};
    std::vector<std::uint8_t> dlc_extra_data = {'D', 'L', 'C', '_', 'O', 'L', 'D', '_', 'H', 'U', 'N', 'T', 'E', 'R', 'S'};

    // Pacote data0: gameparam (v1), drawparam, msg/item
    CHECK(create_synthetic_bhd5_bdt(data0_bhd, data0_bdt, {
        {"param/gameparam.parambnd.dcx", base_gameparam},
        {"param/drawparam/drawparam.parambnd.dcx", drawparam_data},
        {"msg/engus/item.msgbnd.dcx", item_msg_data}
    }));

    // Pacote data1: map m10, msg/menu, event common
    CHECK(create_synthetic_bhd5_bdt(data1_bhd, data1_bdt, {
        {"map/m10_00_00_00.msb", map_m10_data},
        {"msg/engus/menu.msgbnd.dcx", menu_msg_data},
        {"event/common.emevd.dcx", event_data}
    }));

    // Pacote patch: gameparam (v2 - override do base!) e notas de patch
    CHECK(create_synthetic_bhd5_bdt(patch_bhd, patch_bdt, {
        {"param/gameparam.parambnd.dcx", patch_gameparam},
        {"patch_notes.txt", {'P', 'A', 'T', 'C', 'H', '1', '.', '0', '9'}}
    }));

    // Cria arquivo órfão .bhd5 sem o respectivo .bdt (deve ser rejeitado)
    {
        std::ofstream orphan(std::filesystem::u8path((sandbox / "dvdroot_ps4" / "orphan.bhd5").string()), std::ios::binary);
        orphan.write("BHD5", 4);
    }
    // Cria arquivo genérico que não é pacote
    {
        std::ofstream dummy(std::filesystem::u8path((sandbox / "dvdroot_ps4" / "readme.txt").string()), std::ios::binary);
        dummy.write("Bloodborne", 10);
    }

    {
        std::cout << "[test_game_package] 2. Testando varredura e pareamento estruturado...\n";
        GamePackageManager mgr;

        // Varre a subpasta dvdroot_ps4
        std::size_t found = mgr.scan_directory((sandbox / "dvdroot_ps4").string());
        CHECK(found == 3); // data0, data1, patch (orphan.bhd5 e readme.txt rejeitados)
        CHECK(mgr.package_count() == 3);
        CHECK(mgr.has_package("data0"));
        CHECK(mgr.has_package("data1"));
        CHECK(mgr.has_package("patch"));
        CHECK(!mgr.has_package("orphan"));

        // Varredura idempotente: segunda varredura não duplica pacotes existentes
        std::size_t found2 = mgr.scan_directory((sandbox / "dvdroot_ps4").string());
        CHECK(found2 == 0);
        CHECK(mgr.package_count() == 3);

        // Estatísticas agregadas
        CHECK(mgr.total_files_indexed() == 8); // 3 (data0) + 3 (data1) + 2 (patch) = 8
        CHECK(mgr.unique_files_count() == 7);  // gameparam é duplicado (override) -> 7 únicos
        CHECK(mgr.total_bdt_size() > 0);
        CHECK(mgr.total_bhd_size() > 0);

        std::cout << "    [ok] Varredura catalogou " << mgr.package_count()
                  << " pacotes, " << mgr.total_files_indexed() << " arquivos ("
                  << mgr.unique_files_count() << " unicos), "
                  << mgr.total_bdt_size() << " bytes BDT.\n";
    }

    // ========================================================================
    // 3. PRECEDENCIA HIERARQUICA DE OVERRIDE (PATCH > BASE)
    // ========================================================================
    {
        std::cout << "[test_game_package] 3. Testando precedencia de override (Patch sobre Base)...\n";
        GamePackageManager mgr;
        mgr.scan_directory((sandbox / "dvdroot_ps4").string());

        // gameparam está presente em data0 (v1) e em patch (v2)
        std::string resolved_pkg;
        const auto *entry = mgr.find_entry("param/gameparam.parambnd.dcx", &resolved_pkg);
        CHECK(entry != nullptr);
        CHECK(resolved_pkg == "patch"); // Patch deve ter precedência!

        std::vector<std::uint8_t> read_data;
        CHECK(mgr.read_file("param/gameparam.parambnd.dcx", read_data) == BinderError::ok);
        CHECK(read_data == patch_gameparam);
        CHECK(read_data != base_gameparam);

        // Ativo exclusivo de data1 (mapa)
        CHECK(mgr.find_entry("map/m10_00_00_00.msb", &resolved_pkg) != nullptr);
        CHECK(resolved_pkg == "data1");
        CHECK(mgr.read_file("map/m10_00_00_00.msb", read_data) == BinderError::ok);
        CHECK(read_data == map_m10_data);

        // Streaming sob demanda com override
        std::vector<std::uint8_t> stream_buf(9, 0);
        std::size_t streamed = 0;
        CHECK(mgr.stream_file("param/gameparam.parambnd.dcx", 0, stream_buf.data(), stream_buf.size(), streamed) == BinderError::ok);
        CHECK(streamed == 9);
        std::string stream_str(stream_buf.begin(), stream_buf.end());
        CHECK(stream_str == "GAMEPARAM");

        // Arquivo inexistente
        CHECK(!mgr.has_file("nonexistent/asset.bin"));
        CHECK(mgr.read_file("nonexistent/asset.bin", read_data) == BinderError::file_not_found);

        std::cout << "    [ok] Precedencia de override do patch confirmada com sucesso.\n";
    }

    // ========================================================================
    // 4. MONTAGEM MULTIPLA NO VIRTUAL FILE SYSTEM E TRANSPARENCIA
    // ========================================================================
    {
        std::cout << "[test_game_package] 4. Testando montagem simultanea no VFS (/dvdroot e /app0/dvdroot_ps4)...\n";
        GamePackageManager mgr;
        mgr.scan_directory((sandbox / "dvdroot_ps4").string());

        VirtualFileSystem vfs;
        CHECK(mgr.mount_all(vfs, {"/dvdroot", "/app0/dvdroot_ps4"}));

        CHECK(vfs.is_mounted("/dvdroot"));
        CHECK(vfs.is_mounted("/app0/dvdroot_ps4"));

        // Acesso transparente via /dvdroot
        CHECK(vfs.exists("/dvdroot/param/gameparam.parambnd.dcx"));
        CHECK(vfs.exists("/dvdroot/map/m10_00_00_00.msb"));
        CHECK(vfs.exists("/dvdroot/patch_notes.txt"));
        CHECK(!vfs.exists("/dvdroot/nonexistent.bin"));

        // Acesso transparente via /app0/dvdroot_ps4
        CHECK(vfs.exists("/app0/dvdroot_ps4/param/gameparam.parambnd.dcx"));
        CHECK(vfs.exists("/app0/dvdroot_ps4/map/m10_00_00_00.msb"));

        // Conteúdo lido do VFS respeita a sobreposição do patch
        std::vector<std::uint8_t> vfs_gameparam;
        CHECK(vfs.read_file("/dvdroot/param/gameparam.parambnd.dcx", vfs_gameparam) == VfsError::ok);
        CHECK(vfs_gameparam == patch_gameparam);

        // Stat pelo VFS
        VfsStat st{};
        CHECK(vfs.stat("/dvdroot/param/gameparam.parambnd.dcx", st) == VfsError::ok);
        CHECK(st.size == patch_gameparam.size());
        CHECK(st.is_readonly);

        // Streaming read_at via VFS preserva override
        std::uint8_t slice[9] = {};
        std::size_t bytes_got = 0;
        CHECK(vfs.read_at("/dvdroot/param/gameparam.parambnd.dcx", 0, slice, sizeof(slice), bytes_got) == VfsError::ok);
        CHECK(bytes_got == sizeof(slice));
        CHECK(std::memcmp(slice, "GAMEPARAM", 9) == 0);

        // Streaming de ativo resolvido em data1
        std::uint8_t map_slice[7] = {};
        CHECK(vfs.read_at("/dvdroot/map/m10_00_00_00.msb", 0, map_slice, sizeof(map_slice), bytes_got) == VfsError::ok);
        CHECK(bytes_got == sizeof(map_slice));
        CHECK(std::memcmp(map_slice, "MAP_M10", 7) == 0);

        // Desmontagem de todos os pontos
        CHECK(mgr.unmount_all(vfs, {"/dvdroot", "/app0/dvdroot_ps4"}));
        CHECK(!vfs.is_mounted("/dvdroot"));
        CHECK(!vfs.is_mounted("/app0/dvdroot_ps4"));
        CHECK(!vfs.exists("/dvdroot/param/gameparam.parambnd.dcx"));

        std::cout << "    [ok] Montagem multipla e streaming delimitado no VFS validados.\n";
    }

    // ========================================================================
    // 5. TESTE DE EXPANSÃO / DLC E HIERARQUIA COMPLETA (PATCH > DLC > BASE)
    // ========================================================================
    {
        std::cout << "[test_game_package] 5. Testando integracao de DLC (Patch > DLC > Base)...\n";

        // DLC sobrescreve map/m10_00_00_00.msb e adiciona dlc_quest.bin
        std::vector<std::uint8_t> dlc_map = {'M', 'A', 'P', '_', 'M', '1', '0', '_', 'D', 'L', 'C'};
        CHECK(create_synthetic_bhd5_bdt(dlc0_bhd, dlc0_bdt, {
            {"map/m10_00_00_00.msb", dlc_map},
            {"dlc/dlc_quest.bin", dlc_extra_data}
        }));

        GamePackageManager mgr;
        mgr.scan_directory((sandbox / "dvdroot_ps4").string());
        CHECK(mgr.package_count() == 4); // data0, data1, dlc0, patch

        std::string resolver;
        // Mapa m10 agora deve ser resolvido pelo DLC (prio 100 > prio data1 11)
        CHECK(mgr.find_entry("map/m10_00_00_00.msb", &resolver) != nullptr);
        CHECK(resolver == "dlc0");

        std::vector<std::uint8_t> data;
        CHECK(mgr.read_file("map/m10_00_00_00.msb", data) == BinderError::ok);
        CHECK(data == dlc_map);

        // gameparam continua no patch (prio 200 > DLC 100 > Base 10)
        CHECK(mgr.find_entry("param/gameparam.parambnd.dcx", &resolver) != nullptr);
        CHECK(resolver == "patch");

        std::cout << "    [ok] Hierarquia tripla Patch > DLC > Base confirmada.\n";
    }

    // ========================================================================
    // 6. VALIDADOR DE INTEGRIDADE ESSENCIAL DE ASSETS
    // ========================================================================
    {
        std::cout << "[test_game_package] 6. Testando PackageInspectionReport e verify_essential_assets...\n";

        GamePackageManager mgr;
        mgr.scan_directory((sandbox / "dvdroot_ps4").string());

        // Inspeciona com o conjunto completo de ativos essenciais sintetizados
        PackageInspectionReport report = mgr.verify_essential_assets();
        CHECK(report.valid);
        CHECK(report.package_count == 4);
        CHECK(report.essential_files_missing.empty());
        CHECK(report.essential_files_found.size() == 6);
        CHECK(report.total_files_indexed == 10);
        CHECK(report.unique_files_count == 8);
        CHECK(report.total_bdt_size > 0);
        CHECK(report.diagnostic_summary.find("[OK]") != std::string::npos);

        // Confere mapeamento de resolução
        CHECK(report.essential_file_package_map["param/gameparam.parambnd.dcx"] == "patch");
        CHECK(report.essential_file_package_map["param/drawparam/drawparam.parambnd.dcx"] == "data0");
        CHECK(report.essential_file_package_map["msg/engus/item.msgbnd.dcx"] == "data0");
        CHECK(report.essential_file_package_map["msg/engus/menu.msgbnd.dcx"] == "data1");
        CHECK(report.essential_file_package_map["map/m10_00_00_00.msb"] == "dlc0");
        CHECK(report.essential_file_package_map["event/common.emevd.dcx"] == "data1");

        // Teste de integridade incompleta (somente data0)
        GamePackageManager incomplete_mgr;
        incomplete_mgr.add_package(data0_bhd.string(), data0_bdt.string());
        PackageInspectionReport inc_report = incomplete_mgr.verify_essential_assets();
        CHECK(!inc_report.valid);
        CHECK(!inc_report.essential_files_missing.empty());
        CHECK(inc_report.diagnostic_summary.find("[FALHA]") != std::string::npos);
        // data0 não tem mapas nem eventos
        CHECK(std::find(inc_report.essential_files_missing.begin(),
                        inc_report.essential_files_missing.end(),
                        "map/m10_00_00_00.msb") != inc_report.essential_files_missing.end());

        // Teste de lista customizada de ativos essenciais
        std::vector<std::string> custom_list = {
            "param/gameparam.parambnd.dcx",
            "missing/required_asset.bin"
        };
        PackageInspectionReport custom_report = mgr.verify_essential_assets(custom_list);
        CHECK(!custom_report.valid);
        CHECK(custom_report.essential_files_found.size() == 1);
        CHECK(custom_report.essential_files_missing.size() == 1);
        CHECK(custom_report.essential_files_missing[0] == "missing/required_asset.bin");

        std::cout << "    [ok] Relatorio de inspecao e diagnostico estruturado validados com sucesso.\n";
    }

    // ========================================================================
    // 7. VERIFICACAO DE ATIVOS REAIS NO HOST (SE DISPONIVEIS)
    // ========================================================================
    {
        std::cout << "[test_game_package] 7. Verificando presenca de pacotes reais no host...\n";
        std::vector<std::string> real_search_dirs = {
            "bloodborne_portPC/dvdroot_ps4",
            "C:/Users/Usuario/Documents/bloodborne_iphone/bloodborne_portPC/blodboorn 3171/Bloodborne\xe2\x84\xa2/Image0/dvdroot_ps4"
        };
        GamePackageManager real_mgr;
        std::size_t real_pkgs = 0;
        for (const auto &d : real_search_dirs) {
            std::error_code dir_ec;
            if (std::filesystem::exists(std::filesystem::u8path(d), dir_ec)) {
                real_pkgs += real_mgr.scan_directory(d, false);
            }
        }
        if (real_pkgs > 0) {
            std::cout << "    [host] Localizados " << real_pkgs << " pacotes reais no host.\n";
            auto report = real_mgr.verify_essential_assets();
            std::cout << "    [host] " << report.diagnostic_summary << "\n";
        } else {
            std::cout << "    [host] Nenhum pacote .bhd5/.bdt real encontrado (esperado em ambiente de desenvolvimento limpo).\n";
        }
    }

    // ========================================================================
    // LIMPEZA DA SANDBOX TEMPORARIA
    // ========================================================================
    std::filesystem::remove_all(sandbox, ec);

    std::cout << "[test_game_package] Todos os testes passaram com exito!\n";
}); }
