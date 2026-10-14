#include "smoke.h"
#include "fixtures.h"
#include "guest_memory.h"
#include "simd.h"
#include "dispatcher.h"
#include "hle.h"
#include "manifest.h"
#include "elf_parser.h"
#include "guest_loader.h"
#include "input.h"
#include "vfs.h"
#include "audio.h"
#include "stability.h"
#include "engine_loop.h"
#include "shader_translator.h"
#include "metal_pipeline.h"
#include "flver_parser.h"
#include <limits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace bb {
SmokeReport smoke() {
    SmokeReport report{0, 0};
    const auto check = [&report](bool passed) {
        ++report.checks;
        if (!passed) {
            ++report.failures;
            std::cerr << "smoke: check #" << report.checks << " failed\n";
        }
    };

    // Legacy baseline checks (6 checks)
    check(fixture_add(1, 2).value == 3);
    const auto carry = fixture_add(std::numeric_limits<std::uint64_t>::max(), 1);
    check(carry.value == 0 && (carry.flags & (CF | ZF)) == (CF | ZF));
    const auto overflow = fixture_add(0x7fffffffffffffffULL, 1);
    check(overflow.value == 0x8000000000000000ULL && (overflow.flags & OF));
    check(fixture_sub(0, 1).value == std::numeric_limits<std::uint64_t>::max());
    check((fixture_sub(0, 1).flags & CF) != 0);
    check(fixture_xor(0xff, 0xff).value == 0);

    // F1: Arithmetic & flags (expanded, 19 checks)
    check(fixture_add8(1, 2).value == 3);
    check(fixture_add16(0xFF, 1).value == 0x100);
    check(fixture_add32(0xFFFFFFFF, 1).value == 0);
    check(fixture_add64(std::numeric_limits<std::uint64_t>::max(), 1).value == 0);

    check(fixture_adc8(0xFE, 1).value == 0xFF);
    check((fixture_adc64(std::numeric_limits<std::uint64_t>::max(), 0).flags & CF) != 0);

    check(fixture_sub8(0, 1).value == 0xFF);
    check(fixture_sub16(0, 1).value == 0xFFFF);
    check(fixture_sub32(0, 1).value == 0xFFFFFFFF);
    check(fixture_sub64(0, 1).value == std::numeric_limits<std::uint64_t>::max());

    check((fixture_sbb64(0, 0).flags & CF) != 0);

    check(fixture_and8(0xF0, 0x0F).value == 0);
    check(fixture_or64(0xF0, 0x0F).value == 0xFF);
    check(fixture_xor8(0xFF, 0xFF).value == 0);
    check(fixture_xor64(0xFFFFFFFFFFFFFFFFULL, 0).value == 0xFFFFFFFFFFFFFFFFULL);

    check(fixture_shl8(1, 3).value == 8);
    check(fixture_shr64(0x8000000000000000ULL, 1).value == 0x4000000000000000ULL);

    check((fixture_cmp32(5, 3).flags & ZF) == 0);
    check((fixture_cmp32(5, 5).flags & ZF) != 0);
    check((fixture_test64(0xFF, 0).flags & ZF) != 0);

    // F2: Control flow (6 checks)
    check(fixture_call_direct(10, 20).value == 30);
    check(fixture_call_indirect(0xABCD, 0x1234).value == (0xABCD ^ 0x1234));

    auto fact5 = fixture_recursion(5, 0);
    check(fact5.value == 120);

    check(fixture_jump_table(0, 0).value == 10);
    check(fixture_jump_table(2, 0).value == 30);
    check(fixture_jump_table(5, 0).value == 20);

    // F3: ABI (9 checks)
    check(fixture_abi_0args(0, 0).value == 0xDEAD);
    check(fixture_abi_1arg(0, 42).value == 42);

    auto six = fixture_abi_6args(1, 2, 3, 4, 5, 6);
    check(six.value == 21);

    auto twelve = fixture_abi_12args(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    check(twelve.value == 78);

    struct Pair { std::uint64_t x, y; };
    Pair p{0x100, 0x200};
    check(fixture_abi_struct_byval(0, reinterpret_cast<std::uint64_t>(&p)).value == 0x300);

    Pair out{};
    fixture_abi_struct_ret(reinterpret_cast<std::uint64_t>(&out), 0);
    check(out.x == 0xABCD && out.y == 0x1234);

    check(fixture_abi_fp_args(1.5, 2.5).value == 4);
    check((fixture_abi_align16(0x123, 0).value & 0xF) == 0);
    check(fixture_abi_callee_saved(0x10, 0x20).value == 0x130);

    // Guest memory baseline checks (8 checks)
    GuestMemory memory(1024);
    check(memory.map(0x400000, 64, read | write) == MemoryError::ok);
    const auto expected = fixture_add64(0xBB00, 0x15).value;
    check(memory.store(0x400038, &expected, sizeof expected) == MemoryError::ok);
    std::uint64_t observed = 0;
    check(memory.load(0x400038, &observed, sizeof observed) == MemoryError::ok);
    check(observed == 0xBB15);
    check(memory.store(0x400039, &expected, sizeof expected) == MemoryError::unmapped);
    check(memory.protect(0x400000, read) == MemoryError::ok);
    check(memory.store(0x400000, &expected, sizeof expected) == MemoryError::denied);
    check(memory.unmap(0x400000) == MemoryError::ok);

    // F4: SIMD Vector 128-bit checks (7 checks)
    Vec128 v1{{{1.0f, 2.0f, 3.0f, 4.0f}}}, v2{{{5.0f, 6.0f, 7.0f, 8.0f}}};
    check(vec_add_f32(v1, v2).f32[0] == 6.0f);
    check(vec_mul_f32(v1, v2).f32[3] == 32.0f);
    check(vec_dot_f32(v1, v2) == 70.0f);
    check(vec_xor(v1, v1).u64[0] == 0 && vec_xor(v1, v1).u64[1] == 0);
    check(!vec_has_nan(v1));
    Vec128 vnan{{{NAN, 0.0f, 0.0f, 0.0f}}};
    check(vec_has_nan(vnan));
    Vec128 vinf{{{INFINITY, 0.0f, 0.0f, 0.0f}}};
    check(vec_has_inf(vinf));

    // F5: Atomics on Guest Memory (5 checks)
    check(memory.map(0x500000, 64, read | write) == MemoryError::ok);
    std::uint64_t old_v = 0;
    check(memory.atomic_fetch_add_u64(0x500000, 42, old_v) == MemoryError::ok && old_v == 0);
    check(memory.atomic_fetch_add_u64(0x500000, 10, old_v) == MemoryError::ok && old_v == 42);
    std::uint64_t cas_expected = 52, cas_desired = 100;
    bool cas_success = false;
    check(memory.atomic_compare_exchange_u64(0x500000, cas_expected, cas_desired, cas_success) == MemoryError::ok && cas_success);
    check(memory.atomic_compare_exchange_u64(0x500000, cas_expected, 200, cas_success) == MemoryError::ok && !cas_success && cas_expected == 100);

    // F6: Indirect Dispatcher & HLE imports (7 checks)
    IndirectDispatcher dispatcher;
    dispatcher.register_target(0x401000, fixture_add64);
    dispatcher.register_target(0x402000, fixture_sub64);
    check(dispatcher.has_target(0x401000));
    check(dispatcher.dispatch(0x401000, 100, 50).value == 150);
    check(dispatcher.dispatch(0x402000, 100, 50).value == 50);
    check(!dispatcher.has_target(0x999999));

    auto hle = HleManager::create_standard();
    check(hle.invoke(0x2002, memory, 0, 0, 0) == 0); // sceKernelGetCurrentCpu
    check(hle.invoke(0x2003, memory, 0x600000, 64, 0x500008) == 0); // sceKernelAllocateDirectMemory
    check(hle.invoke(0x2001, memory, 0x600000, 0, 0) == 0); // sceKernelGettimeofday

    // Translation Manifest validation (2 checks)
    TranslationManifest manifest{1, "x86_64", "arm64-apple-ios", "52d3441",
        {{"0x1000", 64, "bb_entry", true, "verified"}}};
    check(manifest.validate());
    check(manifest.to_json().find("bb_entry") != std::string::npos);

    // S7: Safe Game Parser validation (3 checks)
    std::vector<std::uint8_t> sfo_min(64, 0);
    sfo_min[0] = 0; sfo_min[1] = 'P'; sfo_min[2] = 'S'; sfo_min[3] = 'F';
    std::uint32_t s_keys = 36, s_vals = 48, s_cnt = 1;
    std::memcpy(sfo_min.data() + 8, &s_keys, 4);
    std::memcpy(sfo_min.data() + 12, &s_vals, 4);
    std::memcpy(sfo_min.data() + 16, &s_cnt, 4);
    std::uint16_t s_koff = 0, s_fmt = 0x0204;
    std::uint32_t s_len = 11, s_max = 16, s_doff = 0;
    std::memcpy(sfo_min.data() + 20, &s_koff, 2);
    std::memcpy(sfo_min.data() + 22, &s_fmt, 2);
    std::memcpy(sfo_min.data() + 24, &s_len, 4);
    std::memcpy(sfo_min.data() + 28, &s_max, 4);
    std::memcpy(sfo_min.data() + 32, &s_doff, 4);
    std::memcpy(sfo_min.data() + 36, "TITLE\0", 6);
    std::memcpy(sfo_min.data() + 48, "Bloodborne\0", 11);
    auto meta = SafeGameParser::parse_sfo(sfo_min.data(), sfo_min.size());
    check(meta.title == "Bloodborne");

    std::vector<std::uint8_t> elf_min(128, 0);
    elf_min[0] = 0x7f; elf_min[1] = 'E'; elf_min[2] = 'L'; elf_min[3] = 'F';
    std::uint16_t emach = EM_X86_64, ephsize = 56, ephnum = 1;
    std::uint64_t eentry = 0x400000, ephent = 64;
    std::memcpy(elf_min.data() + 18, &emach, 2);
    std::memcpy(elf_min.data() + 24, &eentry, 8);
    std::memcpy(elf_min.data() + 32, &ephent, 8);
    std::memcpy(elf_min.data() + 54, &ephsize, 2);
    std::memcpy(elf_min.data() + 56, &ephnum, 2);
    std::uint32_t ptype = 1, pflags = 5;
    std::uint64_t pvaddr = 0x400000, psz = 0x1000;
    std::memcpy(elf_min.data() + 64, &ptype, 4);
    std::memcpy(elf_min.data() + 68, &pflags, 4);
    std::memcpy(elf_min.data() + 80, &pvaddr, 8);
    std::memcpy(elf_min.data() + 96, &psz, 8);
    std::memcpy(elf_min.data() + 104, &psz, 8);
    auto insp = SafeGameParser::inspect_executable(elf_min.data(), elf_min.size());
    check(insp.is_valid && insp.machine == EM_X86_64);
    check(insp.code_size == 0x1000);

    // S8: Guest Loader & First Instruction Execution (4 checks)
    GuestMemory boot_mem(256 * 1024);
    auto boot_rep = GuestLoader::load_and_boot(boot_mem, elf_min.data(), elf_min.size(), dispatcher, hle);
    check(boot_rep.success);
    check(boot_rep.first_instruction_executed);
    check(boot_rep.entry_point == 0x400000);
    check((boot_rep.stack_pointer & 0xF) == 0);

    // S9: Input Subsystem (5 checks)
    InputManager input;
    input.set_button(PAD_CROSS, true);
    check((input.poll().buttons & PAD_CROSS) != 0);
    check(InputManager::apply_deadzone(0.05f, 0.15f) == 0.0f);
    check(InputManager::apply_deadzone(1.0f, 0.15f) == 1.0f);
    input.set_triggers(0.9f, 0.0f);
    check((input.poll().buttons & PAD_L2) != 0);
    check(boot_mem.map(0x600000, 64, read | write) == MemoryError::ok);
    check(input.write_hle_pad_data(boot_mem, 0x600000) == MemoryError::ok);

    // S9: Virtual File System & Path Sanitization (6 checks)
    check(VirtualFileSystem::normalize_path("/app0\\sub//") == "/app0/sub");
    check(VirtualFileSystem::contains_traversal("/app0/../hack"));
    check(!VirtualFileSystem::contains_traversal("/app0/data/file.bin"));
    check(VirtualFileSystem::compute_crc32("123456789", 9) == 0xCBF43926u);
    VirtualFileSystem vfs;
    check(vfs.mount("/app0", ".", true));
    std::string resolved;
    bool ro = false;
    check(vfs.resolve("/app0/file", resolved, ro) == VfsError::ok && ro);

    // S9: Audio HLE & Ring Buffer (6 checks)
    AudioRingBuffer ring(32, 2);
    std::int16_t smp[4] = {1, 2, 3, 4};
    check(ring.write(smp, 2) == 2);
    std::int16_t out_smp[4] = {0};
    check(ring.read(out_smp, 2) == 2 && out_smp[0] == 1);
    std::vector<std::int16_t> underflow_smp(20, 0);
    check(ring.read(underflow_smp.data(), 10) == 0); // Underflow intencional
    check(ring.get_stats().underflow_count >= 1);
    AudioManager audio;
    check(audio.init({48000, 2, 64}));
    audio.set_master_volume(0.5f);
    check(audio.get_master_volume() == 0.5f);
    auto sine = audio.generate_sine_wave(440.0f, 0.01f);
    check(!sine.empty());

    // S9: Stability & Frame Pacing percentiles (5 checks)
    std::vector<double> perc_vals = {10.0, 20.0, 30.0, 40.0, 50.0};
    check(FramePacingTracker::calculate_percentile(perc_vals, 50.0) == 30.0);
    FramePacingTracker tracker(30);
    tracker.record_frame_ms(33.33);
    check(tracker.get_stats().total_frames == 1);
    check(tracker.get_stats().p50_ms == 33.33);
    MemoryMonitor mem_mon;
    check(mem_mon.get_budget_bytes() > 0);
    bool trim_called = false;
    mem_mon.register_trim_callback([&trim_called]() { trim_called = true; });
    mem_mon.trigger_low_memory_warning();
    check(trim_called);

    // S9: Engine Coordinator & Save Integrity (7 checks)
    std::error_code sec;
    std::filesystem::path temp_base = std::filesystem::temp_directory_path(sec);
    if (sec || temp_base.empty()) temp_base = "/tmp";
    std::filesystem::path sbox = temp_base / "smoke_s9_box";
    std::filesystem::remove_all(sbox, sec);
    std::filesystem::path sapp = sbox / "app";
    std::filesystem::path ssave = sbox / "save";
    std::filesystem::create_directories(sapp, sec);
    std::filesystem::create_directories(ssave, sec);

    EngineConfig eng_cfg{};
    eng_cfg.app0_dir = sapp.string();
    eng_cfg.savedata_dir = ssave.string();
    EngineCoordinator engine(eng_cfg);
    check(engine.initialize());
    auto f_res = engine.tick(33.33);
    check(f_res.frame_index == 1);
    check(f_res.frame_time_ms == 33.33);
    std::uint32_t save_val = 0x12345678;
    check(engine.save_game_state("smoke_slot", &save_val, sizeof(save_val)));
    std::vector<std::uint8_t> load_val;
    check(engine.load_game_state("smoke_slot", load_val));
    check(load_val.size() == sizeof(save_val) && *reinterpret_cast<std::uint32_t*>(load_val.data()) == 0x12345678);
    engine.shutdown();
    check(!engine.is_initialized());
    std::filesystem::remove_all(sbox, sec);

    // Sprint 2: SPIR-V Shader Translation & Metal 3 Pipeline (4 checks)
    auto vert_spv = ShaderTranslator::build_basic_vertex_spirv();
    std::string spv_err;
    check(ShaderTranslator::validate_spirv_header(vert_spv.data(), vert_spv.size(), spv_err));
    auto vert_msl = ShaderTranslator::translate_spirv_to_msl(vert_spv);
    check(vert_msl.success && !vert_msl.msl_source.empty());

    auto frag_spv = ShaderTranslator::build_basic_fragment_spirv();
    auto frag_msl = ShaderTranslator::translate_spirv_to_msl(frag_spv);
    check(frag_msl.success && frag_msl.msl_source.find("fragment_main") != std::string::npos);

    Metal3ArgumentTable arg_table{};
    arg_table.bind_texture(0, 0x10008000ULL);
    arg_table.bind_buffer(0, 0x20008000ULL);
    check(arg_table.validate());

    // Sprint 2: FLVER 3D Geometry Parser & Metal Unified Buffers (4 checks)
    auto cube_data = FlverParser::create_synthetic_flver_cube();
    check(FlverParser::is_flver_data(cube_data.data(), cube_data.size()));
    FlverModel flver_model{};
    std::string flver_err;
    check(FlverParser::parse(cube_data.data(), cube_data.size(), flver_model, flver_err));
    check(flver_model.total_vertex_count() == 8 && flver_model.total_triangle_count() == 12);
    auto metal_mesh = FlverParser::convert_to_metal_buffers(flver_model);
    check(metal_mesh.vertex_count == 8 && metal_mesh.index_count == 36 && metal_mesh.bounding_sphere[3] > 0.5f);

    // Cleanup
    check(memory.unmap(0x500000) == MemoryError::ok);
    check(memory.unmap(0x600000) == MemoryError::ok && memory.used() == 0);

    return report;
}
}
