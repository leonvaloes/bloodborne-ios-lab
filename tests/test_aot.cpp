#include "check.h"
#include "aot.h"
#include "dispatcher.h"
#include <random>
#include <cstring>
#include <filesystem>
#include <fstream>

template<class F> bool rejects(F f) { try { f(); return false; } catch (const std::runtime_error&) { return true; } }

int main() { return run([] {
    using namespace bb;

    // ========================================================================
    // 1. Legacy Baseline Decoder Checks
    // ========================================================================
    const auto valid = parse_hex("48 89 f8 48 01 f0 c3\n");
    CHECK(valid.size() == 7);
    CHECK(translate(valid, "fixture_add").find("add64") != std::string::npos);
    CHECK(rejects([&] { translate(valid, "injected(){}"); }));
    CHECK(rejects([] { parse_hex("0x48"); }));
    CHECK(rejects([] { parse_hex("gg"); }));
    CHECK(rejects([] { parse_hex("4"); }));
    CHECK(rejects([] { parse_hex("-1"); }));
    CHECK(rejects([] { parse_hex("48x"); }));
    CHECK(rejects([] { translate({}, "fixture_add"); }));
    for (std::size_t length = 0; length < valid.size(); ++length) {
        const std::vector<std::uint8_t> truncated(valid.begin(), valid.begin() + length);
        CHECK(rejects([&] { translate(truncated, "fixture_add"); }));
    }
    auto extra = valid; extra.push_back(0xc3);
    CHECK(rejects([&] { translate(extra, "fixture_add"); }));

    // Exhaustively mutate each byte: only the three explicitly supported ALU
    // opcodes may pass. No best-effort acceptance of unrecognized instructions.
    for (unsigned index = 0; index < valid.size(); ++index) for (unsigned value = 0; value < 256; ++value) {
        auto mutation = valid; mutation[index] = static_cast<std::uint8_t>(value);
        const bool allowed = mutation == valid || (index == 4 && (value == 0x29 || value == 0x31));
        CHECK(rejects([&] { translate(mutation, "fixture_add"); }) != allowed);
    }
    std::mt19937 rng(1234);
    for (unsigned trial = 0; trial < 10000; ++trial) {
        std::vector<std::uint8_t> bytes(rng() % 64);
        for (auto& byte : bytes) byte = static_cast<std::uint8_t>(rng());
        CHECK(rejects([&] { translate(bytes, "fixture_add"); }));
    }
    std::cout << "decoder: malformed text, truncation, 1792 byte mutations and 10000 random inputs passed\n";

    // ========================================================================
    // 2. Instruction Decoding & Lifting Unit Tests
    // ========================================================================
    // Push RBP (0x55)
    const std::uint8_t push_rbp[] = {0x55};
    auto i_push = decode_instruction(push_rbp, sizeof(push_rbp), 0x1000);
    CHECK(i_push.opcode == Opcode::PUSH);
    CHECK(i_push.op1.reg == RegId::RBP);
    CHECK(i_push.length == 1);

    // Mov RBP, RSP (48 89 E5)
    const std::uint8_t mov_rbp_rsp[] = {0x48, 0x89, 0xE5};
    auto i_mov = decode_instruction(mov_rbp_rsp, sizeof(mov_rbp_rsp), 0x1001);
    CHECK(i_mov.opcode == Opcode::MOV);
    CHECK(i_mov.op1.reg == RegId::RBP);
    CHECK(i_mov.op2.reg == RegId::RSP);
    CHECK(i_mov.length == 3);

    // Sub RSP, 32 (48 83 EC 20)
    const std::uint8_t sub_rsp[] = {0x48, 0x83, 0xEC, 0x20};
    auto i_sub = decode_instruction(sub_rsp, sizeof(sub_rsp), 0x1004);
    CHECK(i_sub.opcode == Opcode::SUB);
    CHECK(i_sub.op1.reg == RegId::RSP);
    CHECK(i_sub.op2.imm == 0x20);
    CHECK(i_sub.length == 4);

    // Ret (0xC3)
    const std::uint8_t ret_code[] = {0xC3};
    auto i_ret = decode_instruction(ret_code, sizeof(ret_code), 0x1008);
    CHECK(i_ret.opcode == Opcode::RET);
    CHECK(i_ret.is_terminator);
    CHECK(i_ret.length == 1);

    // Relative control flow: signed displacements, exact next-IP targets,
    // condition mapping and bounded decoding of every incomplete encoding.
    const auto check_branch = [](const std::vector<std::uint8_t>& bytes,
                                 Opcode opcode, std::uint64_t target) {
        auto inst = decode_instruction(bytes.data(), bytes.size(), 0x1000);
        CHECK(inst.opcode == opcode);
        CHECK(inst.length == bytes.size());
        CHECK(inst.target_vaddr == target);
        CHECK(inst.op1.type == OperandType::REL_BRANCH);
        CHECK(inst.op1.rip_target == target);
        CHECK(inst.is_call == (opcode == Opcode::CALL));
        CHECK(inst.is_terminator == (opcode == Opcode::JMP));
        for (std::size_t n = 1; n < bytes.size(); ++n) {
            auto partial = decode_instruction(bytes.data(), n, 0x1000);
            CHECK(partial.opcode == Opcode::UNKNOWN);
            CHECK(partial.length == n);
            CHECK(partial.target_vaddr == 0);
            CHECK(partial.op1.type == OperandType::NONE);
            CHECK(!partial.is_call && !partial.is_terminator);
        }
    };
    check_branch({0xE8, 0xF9, 0xFF, 0xFF, 0xFF}, Opcode::CALL, 0xFFE);
    check_branch({0xE9, 0xF9, 0xFF, 0xFF, 0xFF}, Opcode::JMP, 0xFFE);
    check_branch({0xEB, 0xFE}, Opcode::JMP, 0x1000);
    check_branch({0xEB, 0x7F}, Opcode::JMP, 0x1081);
    check_branch({0xE8, 0x01, 0x00, 0x00, 0x00}, Opcode::CALL, 0x1006);
    for (unsigned cc = 0; cc < 16; ++cc) {
        for (bool near : {false, true}) {
            std::vector<std::uint8_t> bytes = near
                ? std::vector<std::uint8_t>{0x0F, static_cast<std::uint8_t>(0x80 + cc), 0xFA, 0xFF, 0xFF, 0xFF}
                : std::vector<std::uint8_t>{static_cast<std::uint8_t>(0x70 + cc), 0xFE};
            check_branch(bytes, Opcode::JCC, 0x1000);
            auto inst = decode_instruction(bytes.data(), bytes.size(), 0x1000);
            CHECK(inst.condition == static_cast<ConditionCode>(cc));
            CHECK(inst.mnemonic == "j" + condition_code_name(static_cast<ConditionCode>(cc)));
        }
    }
    const std::uint8_t prefixed_jcc[] = {0x66, 0x0F, 0x84, 0xFA, 0xFF, 0xFF, 0xFF};
    auto prefixed = decode_instruction(prefixed_jcc, sizeof(prefixed_jcc), 0x1000);
    CHECK(prefixed.opcode == Opcode::JCC);
    CHECK(prefixed.length == sizeof(prefixed_jcc));
    CHECK(prefixed.target_vaddr == 0x1001);
    for (std::size_t n = 2; n < sizeof(prefixed_jcc); ++n) {
        auto partial = decode_instruction(prefixed_jcc, n, 0x1000);
        CHECK(partial.opcode == Opcode::UNKNOWN);
        CHECK(partial.length == n);
        CHECK(partial.target_vaddr == 0);
    }

    // ========================================================================
    // 3. Function Lifting & Control Flow Analysis
    // ========================================================================
    // Code with internal branch:
    // 48 39 f7       cmp rdi, rsi
    // 74 05          jz +5 (loc_40100a)
    // 48 31 c0       xor rax, rax
    // c3             ret
    // 90             nop (alignment pad)
    // b8 01 00 00 00 mov eax, 1
    // c3             ret
    const std::vector<std::uint8_t> branch_fn_bytes = {
        0x48, 0x39, 0xF7,
        0x74, 0x05,
        0x48, 0x31, 0xC0,
        0xC3,
        0x90,
        0xB8, 0x01, 0x00, 0x00, 0x00,
        0xC3
    };
    auto lifted_branch = lift_function(branch_fn_bytes.data(), branch_fn_bytes.size(), 0x401000, "func_branch_test");
    CHECK(lifted_branch.name == "func_branch_test");
    CHECK(lifted_branch.vaddr == 0x401000);
    CHECK(!lifted_branch.internal_labels.empty());
    CHECK(lifted_branch.internal_labels[0] == 0x40100A);
    std::string branch_cpp = emit_function_cpp(lifted_branch);
    CHECK(branch_cpp.find("loc_40100a:") != std::string::npos);
    CHECK(branch_cpp.find("goto loc_40100a;") != std::string::npos);

    // ========================================================================
    // 4. Code Section Sweeping & Function Discovery
    // ========================================================================
    // Multiple functions separated by NOPs:
    // fn1: 55 48 89 e5 48 31 c0 5d c3 (9 bytes)
    // padding: 90 90 (2 bytes)
    // fn2: 55 48 89 e5 48 89 f8 5d c3 (9 bytes)
    std::vector<std::uint8_t> code_sec = {
        0x55, 0x48, 0x89, 0xE5, 0x48, 0x31, 0xC0, 0x5D, 0xC3,
        0x90, 0x90,
        0x55, 0x48, 0x89, 0xE5, 0x48, 0x89, 0xF8, 0x5D, 0xC3
    };
    auto discovered = lift_code_section(code_sec.data(), code_sec.size(), 0x500000, 0x500000);
    CHECK(discovered.size() == 2);
    CHECK(discovered[0].vaddr == 0x500000);
    CHECK(discovered[0].is_entry_point);
    CHECK(discovered[1].vaddr == 0x50000B);

    // ========================================================================
    // 5. Synthetic ELF Parsing & Batch Translation Output
    // ========================================================================
    std::vector<std::uint8_t> synth_elf(512, 0);
    synth_elf[0] = 0x7F; synth_elf[1] = 'E'; synth_elf[2] = 'L'; synth_elf[3] = 'F';
    synth_elf[4] = 2; synth_elf[5] = 1;
    std::uint64_t s_entry = 0x600000, s_phoff = 64;
    std::uint16_t s_phentsize = 56, s_phnum = 1;
    std::memcpy(synth_elf.data() + 24, &s_entry, 8);
    std::memcpy(synth_elf.data() + 32, &s_phoff, 8);
    std::memcpy(synth_elf.data() + 54, &s_phentsize, 2);
    std::memcpy(synth_elf.data() + 56, &s_phnum, 2);

    // PT_LOAD executable segment at offset 128
    std::uint32_t p_type = 1, p_flags = 5;
    std::uint64_t p_offset = 128, p_vaddr = 0x600000, p_filesz = code_sec.size();
    std::memcpy(synth_elf.data() + 64, &p_type, 4);
    std::memcpy(synth_elf.data() + 68, &p_flags, 4);
    std::memcpy(synth_elf.data() + 72, &p_offset, 8);
    std::memcpy(synth_elf.data() + 80, &p_vaddr, 8);
    std::memcpy(synth_elf.data() + 96, &p_filesz, 8);
    std::memcpy(synth_elf.data() + 104, &p_filesz, 8);
    std::memcpy(synth_elf.data() + 128, code_sec.data(), code_sec.size());

    auto elf_funcs = lift_elf(synth_elf.data(), synth_elf.size(), 0);
    CHECK(elf_funcs.size() == 2);

    // Test batch translation writing to temp directory
    std::error_code ec;
    auto tmp = std::filesystem::temp_directory_path(ec) / "aot_batch_test";
    std::filesystem::create_directories(tmp, ec);
    std::string elf_path = (tmp / "test.elf").string();
    {
        std::ofstream ef(elf_path, std::ios::binary);
        ef.write(reinterpret_cast<const char*>(synth_elf.data()), synth_elf.size());
    }

    AotBatchConfig b_cfg{};
    b_cfg.functions_per_batch = 1; // force 2 batches
    auto b_res = batch_translate_elf(elf_path, tmp.string(), b_cfg);
    CHECK(b_res.success);
    CHECK(b_res.total_functions == 2);
    CHECK(b_res.batch_files_count == 2);
    CHECK(std::filesystem::exists(b_res.header_file));
    CHECK(std::filesystem::exists(b_res.registry_file));
    CHECK(std::filesystem::exists(b_res.manifest_file));
    CHECK(std::filesystem::file_size(b_res.manifest_file) > 50);

    std::filesystem::remove_all(tmp, ec);

    // ========================================================================
    // 6. Indirect Dispatcher Active Singleton & Batch Registration
    // ========================================================================
    IndirectDispatcher test_dispatcher;
    IndirectDispatcher::set_active(&test_dispatcher);
    CHECK(IndirectDispatcher::get_active() == &test_dispatcher);

    std::vector<std::pair<std::uint64_t, GuestFn>> batch_entries = {
        {0x700000, [](std::uint64_t a, std::uint64_t b) -> Result { return Result{a + b, 0}; }},
        {0x700010, [](std::uint64_t a, std::uint64_t b) -> Result { return Result{a ^ b, 0}; }}
    };
    test_dispatcher.register_batch(batch_entries);
    CHECK(test_dispatcher.has_target(0x700000));
    CHECK(test_dispatcher.has_target(0x700010));
    CHECK(test_dispatcher.dispatch(0x700000, 40, 2).value == 42);
    CHECK(test_dispatcher.dispatch(0x700010, 0xFF, 0x0F).value == 0xF0);

    IndirectDispatcher::set_active(nullptr);

    std::cout << "aot_batch: instruction decoding, function lifting, ELF batch generation and dispatcher registry verified successfully\n";
}); }
