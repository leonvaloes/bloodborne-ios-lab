#include "check.h"
#include "game_boot_coordinator.h"
#include "binder.h"

#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <iostream>
#include <memory>
#include <algorithm>

namespace {

using namespace bb;

using Bytes = std::vector<std::uint8_t>;

void put_le(Bytes& b, std::size_t pos, std::uint64_t val, unsigned bytes = 8) {
    for (unsigned i = 0; i < bytes; ++i) {
        b.at(pos + i) = static_cast<std::uint8_t>(val >> (8 * i));
    }
}

void write_ph(Bytes& b, int i, std::uint32_t type, std::uint64_t off, std::uint64_t va, std::uint64_t fs, std::uint64_t ms) {
    auto p = 0x40 + i * 56;
    put_le(b, p, type, 4);
    put_le(b, p + 4, 7, 4);
    put_le(b, p + 8, off);
    put_le(b, p + 16, va);
    put_le(b, p + 32, fs);
    put_le(b, p + 40, ms);
}

void write_tag(Bytes& b, std::size_t& pos, std::uint64_t tag, std::uint64_t val) {
    put_le(b, pos, tag);
    put_le(b, pos + 8, val);
    pos += 16;
}

Bytes synthesize_elf(bool library = false, bool call_import = false, const std::string& needed_module = "") {
    Bytes b(0x900, 0);
    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 2; b[5] = 1; b[6] = 1;
    put_le(b, 18, 62, 2);
    put_le(b, 24, 0x400); // entry point
    put_le(b, 32, 0x40);  // phoff
    put_le(b, 54, 56, 2); // entsize
    put_le(b, 56, 6, 2);  // phnum

    write_ph(b, 0, 1, 0x400, 0x400, 0x200, 0x200);          // Code
    write_ph(b, 1, 0x61000010, 0x600, 0x600, 0x100, 0x100); // Data / GOT
    write_ph(b, 2, 2, 0x700, 0, 0x100, 0x100);              // Dynamic
    write_ph(b, 3, 0x61000000, 0x800, 0, 0x100, 0x100);     // Str / Sym
    write_ph(b, 4, 7, 0x480, 0x480, 4, 16);                 // TLS
    write_ph(b, 5, 0x61000001, 0x500, 0x500, 8, 8);         // ProcParam

    if (library) {
        // xor eax, eax; ret (retorna 0 para sucesso do inicializador do modulo)
        b[0x400] = 0x31; b[0x401] = 0xc0; b[0x402] = 0xc3;
        // 0x410: export function (mov eax, 42; ret)
        b[0x410] = 0xb8; b[0x411] = 0x2a; b[0x412] = 0x00; b[0x413] = 0x00; b[0x414] = 0x00;
        b[0x415] = 0xc3;
    } else if (call_import) {
        // mov rax, 0x10000620 (endereco do import na GOT)
        // mov rax, [rax]
        // call rax
        // ret
        const std::uint8_t code[] = {
            0x48, 0xb8, 0x20, 0x06, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, // mov rax, 0x10000620
            0x48, 0x8b, 0x00,                                           // mov rax, [rax]
            0xff, 0xd0,                                                 // call rax
            0xc3                                                        // ret
        };
        std::memcpy(b.data() + 0x400, code, sizeof(code));
    } else {
        // mov rax, 100; ret
        const std::uint8_t code[] = {
            0x48, 0xc7, 0xc0, 0x64, 0x00, 0x00, 0x00, // mov rax, 100
            0xc3                                      // ret
        };
        std::memcpy(b.data() + 0x400, code, sizeof(code));
    }

    // TLS dados iniciais
    b[0x480] = 1; b[0x481] = 2; b[0x482] = 3; b[0x483] = 4;

    std::size_t p = 0x700;
    write_tag(b, p, 0x61000035, 0);
    write_tag(b, p, 0x61000037, 64);
    write_tag(b, p, 0x61000039, 64);
    write_tag(b, p, 0x6100003f, 48);
    write_tag(b, p, 0x61000029, 112);
    write_tag(b, p, 0x6100002d, 48);
    write_tag(b, p, 0x6100002f, 160);
    write_tag(b, p, 0x61000031, 0);
    write_tag(b, p, 0x61000013, (1ull << 48) | 1);
    write_tag(b, p, 0x6100000d, (1ull << 48) | 6);
    if (library) {
        write_tag(b, p, 12, 0x400); // DT_INIT = 0x400
    }

    // String table
    const char strings[] = "\0libc\0module\0bzQExy189ZI#B#B\0";
    std::memcpy(b.data() + 0x800, strings, sizeof(strings));

    if (!needed_module.empty()) {
        std::size_t needed_offset = 32;
        std::memcpy(b.data() + 0x800 + needed_offset, needed_module.c_str(), needed_module.size() + 1);
        write_tag(b, p, 1, needed_offset); // DT_NEEDED
    }

    // Simbolo de import/export
    put_le(b, 0x800 + 64 + 24, 13, 4); // nome no offset 13: bzQExy189ZI#B#B
    b[0x800 + 64 + 28] = 0x12;
    put_le(b, 0x800 + 64 + 30, library ? 1 : 0, 2);
    put_le(b, 0x800 + 64 + 32, 0x410);

    // Relocacoes
    put_le(b, 0x800 + 112, 0x620);
    put_le(b, 0x800 + 120, (1ull << 32) | 7);
    put_le(b, 0x800 + 136, 0);

    put_le(b, 0x800 + 136, 0x628);
    put_le(b, 0x800 + 144, 8);
    put_le(b, 0x800 + 152, 0x411);

    return b;
}

struct SyntheticAsset {
    std::string path;
    std::vector<std::uint8_t> data;
};

bool create_synthetic_bhd5_bdt(
    const std::filesystem::path& bhd_path,
    const std::filesystem::path& bdt_path,
    const std::vector<SyntheticAsset>& assets
) {
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
    for (const auto& item : assets) {
        Rec r;
        r.hash = Bhd5Reader::hash_path(item.path);
        r.file_size = static_cast<std::uint32_t>(item.data.size());
        r.padded_size = (r.file_size + 15) & ~15u;
        if (r.padded_size == 0) r.padded_size = 16;
        r.file_offset = current_offset;

        if (!item.data.empty()) {
            bdt_out.write(reinterpret_cast<const char*>(item.data.data()), item.data.size());
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

    std::uint32_t bucket_count = 1;
    std::uint32_t bucket_offset = 24;
    std::uint32_t records_offset = 32;
    std::uint32_t total_bhd_size = records_offset + static_cast<std::uint32_t>(recs.size() * 24);

    std::vector<std::uint8_t> bhd_buf(total_bhd_size, 0);
    std::memcpy(bhd_buf.data(), "BHD5", 4);
    bhd_buf[4] = 0xFF;
    put_le(bhd_buf, 12, bucket_count, 4);
    put_le(bhd_buf, 16, bucket_offset, 4);
    put_le(bhd_buf, 20, total_bhd_size, 4);

    put_le(bhd_buf, bucket_offset, static_cast<std::uint32_t>(recs.size()), 4);
    put_le(bhd_buf, bucket_offset + 4, records_offset, 4);

    for (std::size_t i = 0; i < recs.size(); ++i) {
        std::size_t rec_pos = records_offset + i * 24;
        put_le(bhd_buf, rec_pos, recs[i].hash, 8);
        put_le(bhd_buf, rec_pos + 8, recs[i].padded_size, 4);
        put_le(bhd_buf, rec_pos + 12, recs[i].file_size, 4);
        put_le(bhd_buf, rec_pos + 16, recs[i].file_offset, 8);
    }

    std::ofstream bhd_out(std::filesystem::u8path(bhd_path.string()), std::ios::binary);
    if (!bhd_out.is_open()) return false;
    bhd_out.write(reinterpret_cast<const char*>(bhd_buf.data()), bhd_buf.size());
    bhd_out.close();
    return true;
}

std::vector<SyntheticAsset> create_essential_assets_payload() {
    std::vector<SyntheticAsset> assets;
    const std::vector<std::string> paths = GamePackageManager::default_essential_paths();
    for (std::size_t i = 0; i < paths.size(); ++i) {
        std::string dummy = "SYNTHETIC_DATA_" + std::to_string(i);
        assets.push_back({paths[i], std::vector<std::uint8_t>(dummy.begin(), dummy.end())});
    }
    return assets;
}

} // namespace

int main() { return run([] {
    std::cout << "[test_game_boot_coordinator] Iniciando suite de orquestracao unificada...\n";

    std::filesystem::path sandbox = std::filesystem::current_path() / "test_boot_coord_sandbox";
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
    std::filesystem::create_directories(sandbox, ec);

    // ========================================================================
    // 1. Validacao com diretorio inexistente
    // ========================================================================
    {
        std::cout << "[test_game_boot_coordinator] 1. Testando diretorio invalido...\n";
        GameBootCoordinator coordinator;
        CHECK(coordinator.state() == BootState::not_started);

        BootConfig config;
        config.base_dir = (sandbox / "non_existent_folder").string();

        CHECK(!coordinator.initialize(config));
        CHECK(coordinator.state() == BootState::error);

        auto diag = coordinator.diagnostics();
        CHECK(diag.state == BootState::error);
        CHECK(!diag.last_error.empty());
        CHECK(diag.total_steps == 0);
        CHECK(!diag.assets_valid);
    }

    // ========================================================================
    // 2. Falha de integridade: ativos essenciais ausentes
    // ========================================================================
    {
        std::cout << "[test_game_boot_coordinator] 2. Testando ativos essenciais ausentes...\n";
        std::filesystem::path test_dir = sandbox / "missing_assets";
        std::filesystem::create_directories(test_dir / "dvdroot_ps4", ec);

        // Pacote incompleto (apenas um arquivo aleatorio)
        std::vector<SyntheticAsset> dummy_assets = {{"other/random.bin", {1, 2, 3, 4}}};
        CHECK(create_synthetic_bhd5_bdt(test_dir / "dvdroot_ps4" / "data0.bhd5",
                                        test_dir / "dvdroot_ps4" / "data0.bdt",
                                        dummy_assets));

        // Escreve eboot.bin valido
        auto eboot_bytes = synthesize_elf(false, false);
        std::ofstream eboot_out(test_dir / "eboot.bin", std::ios::binary);
        eboot_out.write(reinterpret_cast<const char*>(eboot_bytes.data()), eboot_bytes.size());
        eboot_out.close();

        GameBootCoordinator coordinator;
        BootConfig config;
        config.base_dir = test_dir.string();
        config.require_essential_assets = true;

        CHECK(!coordinator.initialize(config));
        CHECK(coordinator.state() == BootState::error);

        auto diag = coordinator.diagnostics();
        CHECK(diag.state == BootState::error);
        CHECK(!diag.assets_valid);
        CHECK(diag.package_report.package_count == 1);
        CHECK(diag.package_report.essential_files_missing.size() == 6);
        CHECK(diag.summary().find("Error") != std::string::npos);
    }

    // ========================================================================
    // 3. Inicializacao completa e execucao deterministica
    // ========================================================================
    {
        std::cout << "[test_game_boot_coordinator] 3. Testando boot completo bem-sucedido...\n";
        std::filesystem::path test_dir = sandbox / "full_boot";
        std::filesystem::create_directories(test_dir / "dvdroot_ps4", ec);

        // Cria pacote contendo todos os arquivos essenciais
        auto essential_assets = create_essential_assets_payload();
        CHECK(create_synthetic_bhd5_bdt(test_dir / "dvdroot_ps4" / "data0.bhd5",
                                        test_dir / "dvdroot_ps4" / "data0.bdt",
                                        essential_assets));

        // eboot.bin: mov rax, 100; ret
        auto eboot_bytes = synthesize_elf(false, false);
        std::ofstream eboot_out(test_dir / "eboot.bin", std::ios::binary);
        eboot_out.write(reinterpret_cast<const char*>(eboot_bytes.data()), eboot_bytes.size());
        eboot_out.close();

        GameBootCoordinator coordinator;
        BootConfig config;
        config.base_dir = test_dir.string();

        CHECK(coordinator.initialize(config));
        CHECK(coordinator.state() == BootState::modules_initialized);

        // Verifica montagem e integridade do VFS
        CHECK(coordinator.vfs().is_mounted("/dvdroot"));
        CHECK(coordinator.vfs().is_mounted("/app0/dvdroot_ps4"));
        CHECK(coordinator.vfs().is_mounted("/savedata0"));
        CHECK(coordinator.package_report().valid);
        CHECK(coordinator.package_report().essential_files_found.size() == 6);

        // Leitura de asset via VFS
        std::vector<std::uint8_t> asset_data;
        CHECK(coordinator.vfs().read_file("/dvdroot/param/gameparam.parambnd.dcx", asset_data) == VfsError::ok);
        CHECK(!asset_data.empty());

        // Verifica estado dos modulos e bootstrap
        CHECK(coordinator.image().entry == 0x10000400);
        CHECK(coordinator.guest_memory() != nullptr);
        CHECK(coordinator.cpu() != nullptr);
        CHECK(coordinator.runtime() != nullptr);

        auto diag_before = coordinator.diagnostics();
        CHECK(diag_before.state == BootState::modules_initialized);
        CHECK(diag_before.assets_valid);
        CHECK(diag_before.save_initialized);
        CHECK(diag_before.save_mount_point == "/savedata0");
        CHECK(diag_before.total_steps == 0);

        // Executa fatia de passos
        std::uint64_t executed = coordinator.step(100);
        CHECK(executed >= 2);
        CHECK(coordinator.state() == BootState::halted);
        CHECK(coordinator.cpu()->state.gpr[0] == 100);

        auto diag_after = coordinator.diagnostics();
        CHECK(diag_after.state == BootState::halted);
        CHECK(diag_after.total_steps >= 2);
        CHECK(diag_after.total_import_calls == 0);
        std::string sum = diag_after.summary();
        CHECK(sum.find("halted") != std::string::npos);
        CHECK(sum.find("Total Steps Executed:") != std::string::npos);
    }

    // ========================================================================
    // 4. Boot com modulo PRX na ordem de dependencia e despacho de trap HLE
    // ========================================================================
    {
        std::cout << "[test_game_boot_coordinator] 4. Testando modulo sce_module e trap HLE...\n";
        std::filesystem::path test_dir = sandbox / "module_hle_boot";
        std::filesystem::create_directories(test_dir / "dvdroot_ps4", ec);
        std::filesystem::create_directories(test_dir / "sce_module", ec);

        auto essential_assets = create_essential_assets_payload();
        CHECK(create_synthetic_bhd5_bdt(test_dir / "dvdroot_ps4" / "data0.bhd5",
                                        test_dir / "dvdroot_ps4" / "data0.bdt",
                                        essential_assets));

        // Modulo PRX: libtest.prx com inicializador que retorna 0
        auto prx_bytes = synthesize_elf(true, false);
        std::ofstream prx_out(test_dir / "sce_module" / "libtest.prx", std::ios::binary);
        prx_out.write(reinterpret_cast<const char*>(prx_bytes.data()), prx_bytes.size());
        prx_out.close();

        // eboot.bin: depende de libtest.prx e executa chamada ao import _init_env
        auto eboot_bytes = synthesize_elf(false, true, "libtest.prx");
        std::ofstream eboot_out(test_dir / "eboot.bin", std::ios::binary);
        eboot_out.write(reinterpret_cast<const char*>(eboot_bytes.data()), eboot_bytes.size());
        eboot_out.close();

        GameBootCoordinator coordinator;
        BootConfig config;
        config.base_dir = test_dir.string();

        CHECK(coordinator.initialize(config));
        CHECK(coordinator.state() == BootState::modules_initialized);
        CHECK(coordinator.image().modules.size() == 1);
        CHECK(coordinator.image().modules[0].name == "libtest.prx");

        // Executa slice de instrucoes: chama a funcao exportada do modulo PRX e para no exit_trap
        std::uint64_t executed = coordinator.step(50);
        CHECK(executed > 0);
        CHECK(coordinator.state() == BootState::halted);
        CHECK(coordinator.cpu()->state.gpr[0] == 42);

        auto diag = coordinator.diagnostics();
        CHECK(diag.modules_count == 1);
        CHECK(diag.active_modules.at(0) == "libtest.prx");
    }

    // ========================================================================
    // 5. Despacho de Trap HLE pelo interpretador unificado
    // ========================================================================
    {
        std::cout << "[test_game_boot_coordinator] 5. Testando despacho de trap HLE...\n";
        std::filesystem::path test_dir = sandbox / "hle_trap_boot";
        std::filesystem::create_directories(test_dir / "dvdroot_ps4", ec);

        auto essential_assets = create_essential_assets_payload();
        CHECK(create_synthetic_bhd5_bdt(test_dir / "dvdroot_ps4" / "data0.bhd5",
                                        test_dir / "dvdroot_ps4" / "data0.bdt",
                                        essential_assets));

        // eboot.bin com import nao vinculado a nenhum PRX: vai para o trap 0x7f000000
        auto eboot_bytes = synthesize_elf(false, true, "");
        std::ofstream eboot_out(test_dir / "eboot.bin", std::ios::binary);
        eboot_out.write(reinterpret_cast<const char*>(eboot_bytes.data()), eboot_bytes.size());
        eboot_out.close();

        GameBootCoordinator coordinator;
        BootConfig config;
        config.base_dir = test_dir.string();

        CHECK(coordinator.initialize(config));
        CHECK(coordinator.state() == BootState::modules_initialized);
        CHECK(coordinator.image().imports.size() == 1);
        CHECK(coordinator.image().imports[0].address == 0x7f000000);

        // Executa: chama o trap de import HLE, runtime emula _init_env (retorna 0), retorna e para no exit_trap
        std::uint64_t executed = coordinator.step(50);
        CHECK(executed > 0);
        CHECK(coordinator.state() == BootState::halted);

        auto diag = coordinator.diagnostics();
        CHECK(diag.total_import_calls == 1);
        CHECK(diag.import_call_counts.count("bzQExy189ZI#B#B") == 1);
        CHECK(diag.last_import_called == "bzQExy189ZI#B#B");
    }

    // ========================================================================
    // 6. Execucao de fases individuais e transicoes de estado
    // ========================================================================
    {
        std::cout << "[test_game_boot_coordinator] 6. Testando execucao de fases individuais...\n";
        std::filesystem::path test_dir = sandbox / "phases_test";
        std::filesystem::create_directories(test_dir / "dvdroot_ps4", ec);

        auto essential_assets = create_essential_assets_payload();
        CHECK(create_synthetic_bhd5_bdt(test_dir / "dvdroot_ps4" / "data0.bhd5",
                                        test_dir / "dvdroot_ps4" / "data0.bdt",
                                        essential_assets));

        auto eboot_bytes = synthesize_elf(false, false);
        std::ofstream eboot_out(test_dir / "eboot.bin", std::ios::binary);
        eboot_out.write(reinterpret_cast<const char*>(eboot_bytes.data()), eboot_bytes.size());
        eboot_out.close();

        GameBootCoordinator coordinator;
        BootConfig config;
        config.base_dir = test_dir.string();

        CHECK(coordinator.state() == BootState::not_started);

        // Fase 1: Packages & VFS
        CHECK(coordinator.phase1_mount_packages(config));
        CHECK(coordinator.state() == BootState::packages_mounted);
        CHECK(coordinator.vfs().is_mounted("/dvdroot"));

        // Fase 2: Saves & I/O
        CHECK(coordinator.phase2_setup_saves(config));
        CHECK(coordinator.state() == BootState::packages_mounted);
        CHECK(coordinator.vfs().is_mounted("/savedata0"));

        // Fase 3: Modulos & TLS
        CHECK(coordinator.phase3_load_modules(config));
        CHECK(coordinator.guest_memory() != nullptr);

        // Fase 4: Runtime & Inicializacao
        CHECK(coordinator.phase4_initialize_runtime(config));
        CHECK(coordinator.state() == BootState::modules_initialized);

        // Step
        CHECK(coordinator.step(50) >= 2);
        CHECK(coordinator.state() == BootState::halted);

        // Reset e re-inicializacao
        coordinator.reset();
        CHECK(coordinator.state() == BootState::not_started);
        CHECK(!coordinator.vfs().is_mounted("/dvdroot"));
        CHECK(!coordinator.vfs().is_mounted("/savedata0"));

        CHECK(coordinator.initialize(config));
        CHECK(coordinator.state() == BootState::modules_initialized);
    }

    // ========================================================================
    // 7. Boot com eboot.elf e modulos PRX soltos no base_path (sem subpasta)
    // ========================================================================
    {
        std::cout << "[test_game_boot_coordinator] 7. Testando eboot.elf e PRX solto no base_path...\n";
        std::filesystem::path test_dir = sandbox / "loose_files_boot";
        std::filesystem::create_directories(test_dir, ec);

        // Modulo PRX solto diretamente em test_dir (Documents no iOS)
        auto prx_bytes = synthesize_elf(true, false);
        std::ofstream prx_out(test_dir / "libtest.prx", std::ios::binary);
        prx_out.write(reinterpret_cast<const char*>(prx_bytes.data()), prx_bytes.size());
        prx_out.close();

        // eboot.elf (em vez de eboot.bin) diretamente em test_dir
        auto elf_bytes = synthesize_elf(false, true, "libtest.prx");
        std::ofstream elf_out(test_dir / "eboot.elf", std::ios::binary);
        elf_out.write(reinterpret_cast<const char*>(elf_bytes.data()), elf_bytes.size());
        elf_out.close();

        GameBootCoordinator coordinator;
        BootConfig config;
        config.base_dir = test_dir.string();

        CHECK(coordinator.initialize(config));
        CHECK(coordinator.state() == BootState::modules_initialized);
        CHECK(coordinator.image().modules.size() == 1);
        CHECK(coordinator.image().modules[0].name == "libtest.prx");

        std::uint64_t executed = coordinator.step(50);
        CHECK(executed > 0);
        CHECK(coordinator.state() == BootState::halted);
        CHECK(coordinator.cpu()->state.gpr[0] == 42);
    }

    // ========================================================================
    // 8. Teste de vinculacao de InputManager ao runtime
    // ========================================================================
    {
        std::cout << "[test_game_boot_coordinator] 8. Testando vinculacao de InputManager...\n";
        std::filesystem::path test_dir = sandbox / "input_boot";
        std::filesystem::create_directories(test_dir, ec);

        auto eboot_bytes = synthesize_elf(false, false);
        std::ofstream elf_out(test_dir / "eboot.elf", std::ios::binary);
        elf_out.write(reinterpret_cast<const char*>(eboot_bytes.data()), eboot_bytes.size());
        elf_out.close();

        InputManager input;
        GameBootCoordinator coordinator;
        coordinator.attach_input_manager(&input);
        CHECK(coordinator.input_manager() == &input);

        BootConfig config;
        config.base_dir = test_dir.string();

        CHECK(coordinator.initialize(config));
        CHECK(coordinator.runtime() != nullptr);
        input.set_button(PAD_CROSS, true);
        auto pad_data = coordinator.runtime()->pad_manager().format_pad_data(input.poll());
        CHECK(pad_data.buttons & PAD_CROSS);
    }

    std::filesystem::remove_all(sandbox, ec);
    std::cout << "[test_game_boot_coordinator] Todos os testes passaram com sucesso!\n";
}); }
