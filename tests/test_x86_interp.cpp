#include "check.h"
#include "../core/x86_interp.h"
#include <cstring>
#include <initializer_list>

using namespace bb;

namespace {
constexpr std::uint64_t CODE = 0x1000, STACK = 0x10000, DATA = 0x20000, TRAP = 0xF0000000ULL;

struct Machine {
    GuestMemory mem{1u << 20};
    X86Interpreter cpu{mem};
    Machine(std::initializer_list<std::uint8_t> code) {
        CHECK(mem.map(CODE, 0x1000, Permission::read | Permission::write) == MemoryError::ok);
        CHECK(mem.map(STACK, 0x1000, Permission::read | Permission::write) == MemoryError::ok);
        CHECK(mem.map(DATA, 0x1000, Permission::read | Permission::write) == MemoryError::ok);
        std::vector<std::uint8_t> bytes(code);
        CHECK(mem.store(CODE, bytes.data(), bytes.size()) == MemoryError::ok);
        cpu.state.rip = CODE;
        cpu.state.gpr[4] = STACK + 0x1000 - 8;
        const std::uint64_t ret = TRAP;  // `ret` from the entry returns into the trap range
        CHECK(mem.store(cpu.state.gpr[4], &ret, 8) == MemoryError::ok);
        cpu.add_trap_range(TRAP, 0x1000);
    }
    CpuRunResult run(std::uint64_t n = 1000) { return cpu.run(n); }
};
}

int main() {
    return run([] {
        {   // mov/add/ret
            Machine m{0xB8, 5, 0, 0, 0, 0x83, 0xC0, 3, 0xC3};
            const auto r = m.run();
            CHECK(r.stop == CpuStop::trap);
            CHECK(r.steps == 3);
            CHECK(m.cpu.state.gpr[0] == 8);
        }
        {   // loop sum 1..10 with dec/jnz
            Machine m{0x31, 0xC0, 0xB9, 10, 0, 0, 0, 0x01, 0xC8, 0xFF, 0xC9, 0x75, 0xFA, 0xC3};
            const auto r = m.run();
            CHECK(r.stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[0] == 55);
            CHECK(m.cpu.state.gpr[1] == 0);
        }
        {   // call / ret
            Machine m{0xE8, 1, 0, 0, 0, 0xC3, 0xB8, 7, 0, 0, 0, 0xC3};
            const auto r = m.run();
            CHECK(r.stop == CpuStop::trap);
            CHECK(r.steps == 4);
            CHECK(m.cpu.state.gpr[0] == 7);
        }
        {   // 32-bit unsigned div
            Machine m{0xB8, 100, 0, 0, 0, 0x31, 0xD2, 0xB9, 7, 0, 0, 0, 0xF7, 0xF1, 0xC3};
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[0] == 14);
            CHECK(m.cpu.state.gpr[2] == 2);
        }
        {   // 64-bit mul with carry into rdx
            Machine m{0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xB9, 2, 0, 0, 0, 0x48, 0xF7, 0xE1, 0xC3};
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[0] == 0xFFFFFFFFFFFFFFFEULL);
            CHECK(m.cpu.state.gpr[2] == 1);
            CHECK((m.cpu.state.rflags & FLAG_CF) != 0);
        }
        {   // rip-relative lea
            Machine m{0x48, 0x8D, 0x05, 0x10, 0, 0, 0, 0xC3};
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[0] == CODE + 7 + 0x10);
        }
        {   // LEA calcula offset efetivo, sem somar bases FS/GS; flags intactas.
            for (const std::uint8_t prefix : {0x64, 0x65}) {
                Machine m{prefix, 0x48, 0x8D, 0x47, 0x10, 0xC3};
                m.cpu.state.gpr[7] = DATA;
                m.cpu.state.fs_base = 0x10000;
                m.cpu.state.gs_base = 0x20000;
                m.cpu.state.rflags = 2 | FLAG_CF | FLAG_ZF;
                CHECK(m.run().stop == CpuStop::trap);
                CHECK(m.cpu.state.gpr[0] == DATA + 0x10);
                CHECK(m.cpu.state.rflags == (2 | FLAG_CF | FLAG_ZF));
            }
        }
        {   // LEA RIP-relative com GS também ignora a base do segmento.
            Machine m{0x65, 0x48, 0x8D, 0x05, 0x10, 0, 0, 0, 0xC3};
            m.cpu.state.gs_base = DATA;
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[0] == CODE + 8 + 0x10);
        }
        {   // PUSH imm16: dois bytes na pilha, imediato de dois bytes e RIP correto.
            Machine m{0x66, 0x68, 0x34, 0xF2, 0xF9};
            const std::uint64_t old_sp = m.cpu.state.gpr[4];
            m.cpu.state.rflags = 2 | FLAG_ZF;
            const std::uint8_t bytes[] = {0x66, 0x68, 0x34, 0xF2};
            const auto d = decode_x86(bytes, sizeof(bytes));
            CHECK(d.supported && d.length == 4);
            CHECK(m.run(1).stop == CpuStop::step_limit);
            CHECK(m.cpu.state.rip == CODE + 4);
            CHECK(m.cpu.state.gpr[4] == old_sp - 2);
            std::uint16_t value = 0;
            CHECK(m.mem.load(old_sp - 2, &value, 2) == MemoryError::ok);
            CHECK(value == 0xF234);
            CHECK(m.cpu.state.rflags == (2 | FLAG_ZF));
            CHECK(m.run(1).stop == CpuStop::step_limit);
            CHECK((m.cpu.state.rflags & FLAG_CF) != 0);
        }
        {   // Shift com count mascarado para zero lê memória, mas não escreve em página readonly.
            for (const std::uint8_t count : {0, 32}) {
                Machine m{0xC1, 0x27, count, 0xC3}; // shl dword ptr [rdi], imm8
                m.cpu.state.gpr[7] = DATA;
                m.cpu.state.rflags = 2 | FLAG_CF | FLAG_OF | FLAG_ZF;
                const std::uint32_t value = 0x80000001;
                CHECK(m.mem.store(DATA, &value, 4) == MemoryError::ok);
                CHECK(m.mem.protect(DATA, Permission::read) == MemoryError::ok);
                CHECK(m.run().stop == CpuStop::trap);
                std::uint32_t result = 0;
                CHECK(m.mem.load(DATA, &result, 4) == MemoryError::ok);
                CHECK(result == value);
                CHECK(m.cpu.state.rflags == (2 | FLAG_CF | FLAG_OF | FLAG_ZF));
            }
            Machine m{0xD3, 0x27, 0xC3}; // shl dword ptr [rdi], cl
            m.cpu.state.gpr[7] = DATA;
            m.cpu.state.gpr[1] = 32;
            m.cpu.state.rflags = 2 | FLAG_CF;
            const std::uint32_t value = 0x11;
            CHECK(m.mem.store(DATA, &value, 4) == MemoryError::ok);
            CHECK(m.mem.protect(DATA, Permission::read) == MemoryError::ok);
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.rflags == (2 | FLAG_CF));
        }
        {   // Mesmo com count zero, operandos de memória inválidos ainda causam fault de leitura.
            Machine m{0xC1, 0x27, 0};
            m.cpu.state.gpr[7] = 0x99999;
            const auto r = m.run();
            CHECK(r.stop == CpuStop::memory_fault && r.fault_address == 0x99999);
            CHECK(r.rip == CODE);
        }
        {   // Count efetivo não zero escreve: página readonly causa fault; CF da operação normal.
            Machine m{0xD1, 0x27}; // shl dword ptr [rdi], 1
            m.cpu.state.gpr[7] = DATA;
            const std::uint32_t value = 0x80000001;
            CHECK(m.mem.store(DATA, &value, 4) == MemoryError::ok);
            CHECK(m.mem.protect(DATA, Permission::read) == MemoryError::ok);
            const auto r = m.run();
            CHECK(r.stop == CpuStop::memory_fault && r.fault_address == DATA);
            CHECK((m.cpu.state.rflags & FLAG_CF) != 0);
        }
        {   // memory fault reports the address and the faulting rip
            Machine m{0x8B, 0x01};
            m.cpu.state.gpr[1] = 0x99999;
            const auto r = m.run();
            CHECK(r.stop == CpuStop::memory_fault);
            CHECK(r.fault_address == 0x99999);
            CHECK(r.rip == CODE);
            CHECK(r.steps == 0);
        }
        {   // unsupported opcode is reported, not skipped
            Machine m{0x0F, 0x38, 0x00};
            const auto r = m.run();
            CHECK(r.stop == CpuStop::unsupported_opcode);
            CHECK(r.rip == CODE);
            CHECK(r.byte_count >= 2 && r.bytes[0] == 0x0F && r.bytes[1] == 0x38);
        }
        {   // divide by zero
            Machine m{0x31, 0xC9, 0xF7, 0xF1};
            const auto r = m.run();
            CHECK(r.stop == CpuStop::divide_error);
            CHECK(r.rip == CODE + 2);
        }
        {   // rep stosb
            Machine m{0x48, 0xBF, 0, 0, 2, 0, 0, 0, 0, 0, 0xB9, 4, 0, 0, 0, 0xB0, 0xAB, 0xF3, 0xAA, 0xC3};
            CHECK(m.run().stop == CpuStop::trap);
            std::uint8_t out[5] = {};
            CHECK(m.mem.load(DATA, out, 5) == MemoryError::ok);
            CHECK(out[0] == 0xAB && out[3] == 0xAB && out[4] == 0);
            CHECK(m.cpu.state.gpr[1] == 0);
            CHECK(m.cpu.state.gpr[7] == DATA + 4);
        }
        {   // step limit
            Machine m{0xEB, 0xFE};
            const auto r = m.run(100);
            CHECK(r.stop == CpuStop::step_limit);
            CHECK(r.steps == 100);
        }
        {   // signed vs unsigned compare + setcc
            Machine m{0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0x83, 0xF8, 0x01, 0x0F, 0x9C, 0xC1, 0x0F, 0x92, 0xC2, 0xC3};
            m.cpu.state.gpr[2] = 0xFF;
            CHECK(m.run().stop == CpuStop::trap);
            CHECK((m.cpu.state.gpr[1] & 0xFF) == 1);
            CHECK((m.cpu.state.gpr[2] & 0xFF) == 0);
        }
        {   // movups store of xmm1
            Machine m{0x0F, 0x11, 0x0F, 0xC3};
            m.cpu.state.gpr[7] = DATA;
            m.cpu.state.xmm[1][0] = 0x1122334455667788ULL;
            m.cpu.state.xmm[1][1] = 0x99AABBCCDDEEFF00ULL;
            CHECK(m.run().stop == CpuStop::trap);
            std::uint64_t lo = 0, hi = 0;
            CHECK(m.mem.load(DATA, &lo, 8) == MemoryError::ok);
            CHECK(m.mem.load(DATA + 8, &hi, 8) == MemoryError::ok);
            CHECK(lo == 0x1122334455667788ULL && hi == 0x99AABBCCDDEEFF00ULL);
        }
        {   // indirect call into an import stub stops with a trap and a valid return address
            Machine m{0x48, 0xB8, 0x10, 0, 0, 0xF0, 0, 0, 0, 0, 0xFF, 0xD0};
            const auto r = m.run();
            CHECK(r.stop == CpuStop::trap);
            CHECK(r.rip == TRAP + 0x10);
            std::uint64_t ret = 0;
            CHECK(m.mem.load(m.cpu.state.gpr[4], &ret, 8) == MemoryError::ok);
            CHECK(ret == CODE + 12);
        }
        {   // pshufd: [1,2,3,4] -> [4,3,2,1]
            Machine m{0x66, 0x0F, 0x70, 0xC1, 0x1B, 0xC3};
            m.cpu.state.xmm[1][0] = 0x0000000200000001ULL;
            m.cpu.state.xmm[1][1] = 0x0000000400000003ULL;
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.xmm[0][0] == 0x0000000300000004ULL);
            CHECK(m.cpu.state.xmm[0][1] == 0x0000000100000002ULL);
        }
        {   // paddb: modular, pcmpeqb: igualdade byte a byte
            Machine m{0x66, 0x0F, 0xFC, 0xC1, 0x66, 0x0F, 0x74, 0xC1, 0xC3};
            m.cpu.state.xmm[0][0] = 0xFF;
            m.cpu.state.xmm[1][0] = 1;
            CHECK(m.run(1).stop == CpuStop::step_limit);
            CHECK(m.cpu.state.xmm[0][0] == 0);
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.xmm[0][0] == 0xFFFFFFFFFFFFFF00ULL);
        }
        {   // sqrtps [4,9,16,25] -> [2,3,4,5]
            Machine m{0x0F, 0x51, 0xC1, 0xC3};
            m.cpu.state.xmm[1][0] = 0x4110000040800000ULL;
            m.cpu.state.xmm[1][1] = 0x41C8000041800000ULL;
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.xmm[0][0] == 0x4040000040000000ULL);
            CHECK(m.cpu.state.xmm[0][1] == 0x40A0000040800000ULL);
        }
        {   // endereçamento de 32 bits disp32 absoluto, GS após truncar endereço
            Machine m{0x67, 0x65, 0x8B, 0x05, 0x20, 0, 0, 0, 0xC3};
            m.cpu.state.gs_base = DATA - 0x20;
            const std::uint32_t v = 0x12345678;
            CHECK(m.mem.store(DATA, &v, 4) == MemoryError::ok);
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[0] == v);
        }
        {   // repne scasb encontra terceiro byte e deixa RCX=1
            Machine m{0xF2, 0xAE, 0xC3};
            const std::uint8_t v[] = {1, 2, 7, 9};
            CHECK(m.mem.store(DATA, v, sizeof(v)) == MemoryError::ok);
            m.cpu.state.gpr[0] = 7; m.cpu.state.gpr[7] = DATA; m.cpu.state.gpr[1] = 4;
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[7] == DATA + 3 && m.cpu.state.gpr[1] == 1);
            CHECK((m.cpu.state.rflags & FLAG_ZF) != 0);
        }
        {   // SSSE3 pshufb: identidade invertida e zero explícito
            Machine m{0x66, 0x0F, 0x38, 0x00, 0xC1, 0xC3};
            m.cpu.state.xmm[0][0] = 0x0706050403020100ULL;
            m.cpu.state.xmm[0][1] = 0x0F0E0D0C0B0A0908ULL;
            m.cpu.state.xmm[1][0] = 0x08090A0B0C0D0E0FULL;
            m.cpu.state.xmm[1][1] = 0x8001020304050607ULL;
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.xmm[0][0] == 0x08090A0B0C0D0E0FULL);
            CHECK(m.cpu.state.xmm[0][1] == 0x0001020304050607ULL);
        }
        {   // palignr: deslocamento 14 de src:dest
            Machine m{0x66, 0x0F, 0x3A, 0x0F, 0xC1, 14, 0xC3};
            m.cpu.state.xmm[0][0] = 0x1122334455667788ULL;
            m.cpu.state.xmm[0][1] = 0x99AABBCCDDEEFF00ULL;
            m.cpu.state.xmm[1][1] = 0x2211000000000000ULL;
            CHECK(m.run().stop == CpuStop::trap);
            CHECK((m.cpu.state.xmm[0][0] & 0xFFFFULL) == 0x2211);
            CHECK(m.cpu.state.xmm[0][0] == 0x3344556677882211ULL);
        }
        {   // flags e syscall não escapam para o host
            Machine m{0xF9, 0x9C, 0xF8, 0x9D, 0x0F, 0x05};
            CHECK(m.run().stop == CpuStop::halt);
            CHECK((m.cpu.state.rflags & FLAG_CF) != 0);
        }
        {   // decodificador estrutural
            const std::uint8_t b[] = {0x66, 0x0F, 0x70, 0xC1, 0x1B};
            const auto d = decode_x86(b, sizeof(b));
            CHECK(d.length == 5 && d.opcode == 0x10170 && d.supported);
        }
        {   // BMI1: VEX.NDD destination comes from inverted vvvv, not ModRM.reg.
            // Actual boot encoding: c4 e2 f0 f3 ca = blsr rcx, rdx.
            Machine m{0xC4, 0xE2, 0xF0, 0xF3, 0xCA, 0xC3};
            m.cpu.state.gpr[2] = 0x8000000000000010ULL;
            m.cpu.state.gpr[1] = 0xDEAD;
            m.cpu.state.rflags |= FLAG_OF | FLAG_CF | FLAG_ZF;
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[1] == 0x8000000000000000ULL);
            CHECK(m.cpu.state.gpr[2] == 0x8000000000000010ULL);
            CHECK((m.cpu.state.rflags & (FLAG_CF | FLAG_OF | FLAG_ZF | FLAG_SF)) == FLAG_SF);
            const std::uint8_t b[] = {0xC4, 0xE2, 0xF0, 0xF3, 0xCA};
            CHECK(decode_x86(b, sizeof(b)).length == 5);
            CHECK(decode_x86(b, sizeof(b)).supported);
        }
        {   // BLSR 32 zeroes high half, sets CF and ZF on source zero.
            Machine m{0xC4, 0xE2, 0x78, 0xF3, 0xC9, 0xC3};
            m.cpu.state.gpr[1] = 0;
            m.cpu.state.gpr[0] = ~0ULL;
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[0] == 0);
            CHECK((m.cpu.state.rflags & (FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF)) == (FLAG_CF | FLAG_ZF));
        }
        {   // BLSMSK 64 via memory and extended destination r9.
            Machine m{0xC4, 0xE2, 0xB0, 0xF3, 0x17, 0xC3};
            m.cpu.state.gpr[7] = DATA;
            const std::uint64_t v = 0x100;
            CHECK(m.mem.store(DATA, &v, 8) == MemoryError::ok);
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[9] == 0x1FF);
            CHECK((m.cpu.state.rflags & (FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF)) == 0);
        }
        {   // VEX.X e VEX.B em SIB: [r8 + r9*2 + 8], destino r11.
            Machine m{0xC4, 0x02, 0xA0, 0xF3, 0x4C, 0x48, 8, 0xC3};
            m.cpu.state.gpr[8] = DATA;
            m.cpu.state.gpr[9] = 4;
            const std::uint64_t v = 0x8000000000000010ULL;
            CHECK(m.mem.store(DATA + 16, &v, 8) == MemoryError::ok);
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[11] == 0x8000000000000000ULL);
        }
        {   // BLSMSK zero -> all bits one, CF and SF set, ZF always clear.
            Machine m{0xC4, 0xE2, 0x78, 0xF3, 0xD1, 0xC3};
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[0] == 0xFFFFFFFFULL);
            CHECK((m.cpu.state.rflags & (FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF)) == (FLAG_CF | FLAG_SF));
        }
        {   // BLSI 32 via VEX.B extended r10 source: CF set for nonzero, SF at bit 31.
            Machine m{0xC4, 0xC2, 0x78, 0xF3, 0xDA, 0xC3};
            m.cpu.state.gpr[10] = 0x80000000ULL;
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[0] == 0x80000000ULL);
            CHECK((m.cpu.state.rflags & (FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF)) == (FLAG_CF | FLAG_SF));
        }
        {   // BLSI zero: CF clear, ZF set.
            Machine m{0xC4, 0xE2, 0xF0, 0xF3, 0xDA, 0xC3};
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.gpr[1] == 0);
            CHECK((m.cpu.state.rflags & (FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF)) == FLAG_ZF);
        }
        {   // Unrecognized opcode extensions, mandatory prefix and VEX.L must not silently execute.
            for (auto bytes : {std::initializer_list<std::uint8_t>{0xC4, 0xE2, 0x78, 0xF3, 0xC0},
                               {0xC4, 0xE2, 0x79, 0xF3, 0xC9},
                               {0xC4, 0xE2, 0x7C, 0xF3, 0xC9},
                               {0xC4, 0xE1, 0x78, 0xF3, 0xC9},
                               {0x48, 0xC4, 0xE2, 0x78, 0xF3, 0xC9}}) {
                Machine m(bytes);
                CHECK(m.run().stop == CpuStop::unsupported_opcode);
            }
        }
        {   // Truncated VEX prefix and memory displacement report fetch fault, not success.
            Machine m{0xC4, 0xE2, 0xF0, 0xF3, 0x8A};
            const std::uint8_t b[] = {0xC4, 0xE2, 0xF0, 0xF3, 0x8A};
            CHECK(decode_x86(b, sizeof(b)).length == 0);
            // The mapped code page is padded with zeroes; use a RIP at its last five bytes.
            CHECK(m.mem.store(CODE + 0x1000 - sizeof(b), b, sizeof(b)) == MemoryError::ok);
            m.cpu.state.rip = CODE + 0x1000 - sizeof(b);
            CHECK(m.run().stop == CpuStop::memory_fault);
        }
        {   // REX immediately before opcode selects SPL; legacy prefix cancels earlier REX.
            Machine m{0x40, 0xB4, 0x12, 0x40, 0x66, 0xB4, 0x34, 0xC3};
            m.cpu.state.gpr[4] = STACK + 0x1000 - 8;
            CHECK(m.run(2).stop == CpuStop::step_limit);
            CHECK((m.cpu.state.gpr[0] & 0xFF00) == 0x3400);
            CHECK((m.cpu.state.gpr[4] & 0xFF) == 0x12);
        }
        {   // RIP-relative destination points beyond the trailing immediate.
            Machine m{0xC7, 0x05, 0x10, 0, 0, 0, 0x78, 0x56, 0x34, 0x12, 0xC3};
            CHECK(m.run().stop == CpuStop::trap);
            std::uint32_t v = 0;
            CHECK(m.mem.load(CODE + 10 + 0x10, &v, 4) == MemoryError::ok);
            CHECK(v == 0x12345678);
        }
        {   // A REP fault leaves completed iterations committed and RIP at instruction start.
            Machine m{0xF3, 0xAA};
            m.cpu.state.gpr[7] = DATA + 0xFFE;
            m.cpu.state.gpr[1] = 4;
            m.cpu.state.gpr[0] = 0xA5;
            const auto r = m.run();
            CHECK(r.stop == CpuStop::memory_fault && r.rip == CODE && r.fault_address == DATA + 0x1000);
            CHECK(m.cpu.state.gpr[1] == 2 && m.cpu.state.gpr[7] == DATA + 0x1000);
            std::uint8_t b[2] = {};
            CHECK(m.mem.load(DATA + 0xFFE, b, 2) == MemoryError::ok);
            CHECK(b[0] == 0xA5 && b[1] == 0xA5);
        }
        {   // Signed 32-bit dividend -2^63 / -1 raises #DE without modifying operands.
            Machine m{0xF7, 0xF9};
            m.cpu.state.gpr[2] = 0x80000000;
            m.cpu.state.gpr[0] = 0;
            m.cpu.state.gpr[1] = 0xFFFFFFFF;
            CHECK(m.run().stop == CpuStop::divide_error);
            CHECK(m.cpu.state.gpr[0] == 0 && m.cpu.state.gpr[2] == 0x80000000);
        }
        {   // Fifteen prefix bytes cannot turn into an instruction by reading byte sixteen.
            Machine m{0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
                      0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x90};
            CHECK(m.run().stop == CpuStop::undefined);
        }
        {   // VEX.128 vxorps uses two independent sources and clears upper YMM.
            Machine m{0xC5, 0xF0, 0x57, 0xC2, 0xC3};
            m.cpu.state.xmm[1][0] = 0xAAAAAAAAAAAAAAAAULL;
            m.cpu.state.xmm[2][0] = 0xFFFF0000FFFF0000ULL;
            m.cpu.state.ymm_hi[0][0] = ~0ULL;
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.xmm[0][0] == 0x5555AAAA5555AAAAULL);
            CHECK(m.cpu.state.ymm_hi[0][0] == 0);
            const std::uint8_t b[] = {0xC5, 0xF0, 0x57, 0xC2};
            CHECK(decode_x86(b, sizeof(b)).length == 4);
        }
        {   // VEX.256 vaddps adds all eight lanes; vzeroupper preserves XMM.
            Machine m{0xC5, 0xF4, 0x58, 0xC2, 0xC5, 0xF8, 0x77, 0xC3};
            for (int i = 0; i < 2; ++i) {
                m.cpu.state.xmm[1][i] = m.cpu.state.ymm_hi[1][i] = 0x3F8000003F800000ULL;
                m.cpu.state.xmm[2][i] = m.cpu.state.ymm_hi[2][i] = 0x4000000040000000ULL;
            }
            CHECK(m.run(1).stop == CpuStop::step_limit);
            CHECK(m.cpu.state.xmm[0][0] == 0x4040000040400000ULL);
            CHECK(m.cpu.state.ymm_hi[0][1] == 0x4040000040400000ULL);
            CHECK(m.run().stop == CpuStop::trap);
            CHECK(m.cpu.state.xmm[0][0] == 0x4040000040400000ULL);
            CHECK(m.cpu.state.ymm_hi[0][1] == 0);
        }
        {   // Reserved vvvv on VMOV and unsupported AVX map must not execute.
            for (auto bytes : {std::initializer_list<std::uint8_t>{0xC5, 0xF0, 0x10, 0xC1},
                               {0xC4, 0xE2, 0x78, 0x58, 0xC1}}) {
                Machine m(bytes);
                CHECK(m.run().stop == CpuStop::unsupported_opcode);
            }
        }
        {   // VMOVD / VMOVQ (0x6E): reg/mem -> xmm, W=0 (32-bit) and W=1 (64-bit)
            // 32-bit reg -> xmm: zeroes upper 96 bits of XMM and clears ymm_hi
            Machine m1{0xC5, 0xF9, 0x6E, 0xC3, 0xC3}; // vmovd xmm0, ebx; ret
            m1.cpu.state.gpr[3] = 0xDEADBEEF12345678ULL;
            m1.cpu.state.xmm[0][0] = ~0ULL; m1.cpu.state.xmm[0][1] = ~0ULL;
            m1.cpu.state.ymm_hi[0][0] = ~0ULL; m1.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m1.run().stop == CpuStop::trap);
            CHECK(m1.cpu.state.xmm[0][0] == 0x12345678ULL);
            CHECK(m1.cpu.state.xmm[0][1] == 0);
            CHECK(m1.cpu.state.ymm_hi[0][0] == 0 && m1.cpu.state.ymm_hi[0][1] == 0);
            const std::uint8_t d_vmovd[] = {0xC5, 0xF9, 0x6E, 0xC3};
            CHECK(decode_x86(d_vmovd, sizeof(d_vmovd)).length == 4);
            CHECK(decode_x86(d_vmovd, sizeof(d_vmovd)).supported);

            // 64-bit reg -> xmm: zeroes upper 64 bits of XMM and clears ymm_hi
            Machine m2{0xC4, 0xE1, 0xF9, 0x6E, 0xC3, 0xC3}; // vmovq xmm0, rbx; ret
            m2.cpu.state.gpr[3] = 0x0123456789ABCDEFULL;
            m2.cpu.state.xmm[0][0] = ~0ULL; m2.cpu.state.xmm[0][1] = ~0ULL;
            m2.cpu.state.ymm_hi[0][0] = ~0ULL; m2.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m2.run().stop == CpuStop::trap);
            CHECK(m2.cpu.state.xmm[0][0] == 0x0123456789ABCDEFULL);
            CHECK(m2.cpu.state.xmm[0][1] == 0);
            CHECK(m2.cpu.state.ymm_hi[0][0] == 0 && m2.cpu.state.ymm_hi[0][1] == 0);
            const std::uint8_t d_vmovq[] = {0xC4, 0xE1, 0xF9, 0x6E, 0xC3};
            CHECK(decode_x86(d_vmovq, sizeof(d_vmovq)).length == 5);
            CHECK(decode_x86(d_vmovq, sizeof(d_vmovq)).supported);

            // 32-bit mem -> xmm
            Machine m3{0xC5, 0xF9, 0x6E, 0x03, 0xC3}; // vmovd xmm0, [rbx]; ret
            m3.cpu.state.gpr[3] = DATA;
            const std::uint32_t val32 = 0x87654321U;
            CHECK(m3.mem.store(DATA, &val32, 4) == MemoryError::ok);
            m3.cpu.state.xmm[0][0] = ~0ULL; m3.cpu.state.xmm[0][1] = ~0ULL;
            m3.cpu.state.ymm_hi[0][0] = ~0ULL; m3.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m3.run().stop == CpuStop::trap);
            CHECK(m3.cpu.state.xmm[0][0] == 0x87654321ULL);
            CHECK(m3.cpu.state.xmm[0][1] == 0);
            CHECK(m3.cpu.state.ymm_hi[0][0] == 0 && m3.cpu.state.ymm_hi[0][1] == 0);

            // 64-bit mem -> xmm
            Machine m4{0xC4, 0xE1, 0xF9, 0x6E, 0x03, 0xC3}; // vmovq xmm0, [rbx]; ret
            m4.cpu.state.gpr[3] = DATA;
            const std::uint64_t val64 = 0xFEDCBA9876543210ULL;
            CHECK(m4.mem.store(DATA, &val64, 8) == MemoryError::ok);
            m4.cpu.state.xmm[0][0] = ~0ULL; m4.cpu.state.xmm[0][1] = ~0ULL;
            m4.cpu.state.ymm_hi[0][0] = ~0ULL; m4.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m4.run().stop == CpuStop::trap);
            CHECK(m4.cpu.state.xmm[0][0] == 0xFEDCBA9876543210ULL);
            CHECK(m4.cpu.state.xmm[0][1] == 0);
            CHECK(m4.cpu.state.ymm_hi[0][0] == 0 && m4.cpu.state.ymm_hi[0][1] == 0);
        }
        {   // VMOVD / VMOVQ (0x7E): xmm -> reg/mem (pp=1) and xmm/mem -> xmm (pp=2)
            // 32-bit xmm -> reg (zero-extends GPR)
            Machine m1{0xC5, 0xF9, 0x7E, 0xC3, 0xC3}; // vmovd ebx, xmm0; ret
            m1.cpu.state.xmm[0][0] = 0xDEADBEEFCAFEBABEULL;
            m1.cpu.state.gpr[3] = ~0ULL;
            CHECK(m1.run().stop == CpuStop::trap);
            CHECK(m1.cpu.state.gpr[3] == 0xCAFEBABEULL);
            const std::uint8_t d_vmovd_inv[] = {0xC5, 0xF9, 0x7E, 0xC3};
            CHECK(decode_x86(d_vmovd_inv, sizeof(d_vmovd_inv)).length == 4);
            CHECK(decode_x86(d_vmovd_inv, sizeof(d_vmovd_inv)).supported);

            // 64-bit xmm -> reg
            Machine m2{0xC4, 0xE1, 0xF9, 0x7E, 0xC3, 0xC3}; // vmovq rbx, xmm0; ret
            m2.cpu.state.xmm[0][0] = 0xDEADBEEFCAFEBABEULL;
            m2.cpu.state.gpr[3] = 0;
            CHECK(m2.run().stop == CpuStop::trap);
            CHECK(m2.cpu.state.gpr[3] == 0xDEADBEEFCAFEBABEULL);
            const std::uint8_t d_vmovq_inv[] = {0xC4, 0xE1, 0xF9, 0x7E, 0xC3};
            CHECK(decode_x86(d_vmovq_inv, sizeof(d_vmovq_inv)).length == 5);
            CHECK(decode_x86(d_vmovq_inv, sizeof(d_vmovq_inv)).supported);

            // 32-bit xmm -> mem
            Machine m3{0xC5, 0xF9, 0x7E, 0x03, 0xC3}; // vmovd [rbx], xmm0; ret
            m3.cpu.state.gpr[3] = DATA;
            m3.cpu.state.xmm[0][0] = 0x11223344AABBCCDDULL;
            CHECK(m3.run().stop == CpuStop::trap);
            std::uint32_t out32 = 0;
            CHECK(m3.mem.load(DATA, &out32, 4) == MemoryError::ok);
            CHECK(out32 == 0xAABBCCDD);

            // 64-bit xmm -> mem
            Machine m4{0xC4, 0xE1, 0xF9, 0x7E, 0x03, 0xC3}; // vmovq [rbx], xmm0; ret
            m4.cpu.state.gpr[3] = DATA;
            m4.cpu.state.xmm[0][0] = 0x11223344AABBCCDDULL;
            CHECK(m4.run().stop == CpuStop::trap);
            std::uint64_t out64 = 0;
            CHECK(m4.mem.load(DATA, &out64, 8) == MemoryError::ok);
            CHECK(out64 == 0x11223344AABBCCDDULL);

            // pp=2 (F3 0F 7E): vmovq xmm0, xmm1
            Machine m5{0xC5, 0xFA, 0x7E, 0xC1, 0xC3}; // vmovq xmm0, xmm1; ret
            m5.cpu.state.xmm[1][0] = 0x0123456789ABCDEFULL;
            m5.cpu.state.xmm[1][1] = 0xFEDCBA9876543210ULL;
            m5.cpu.state.xmm[0][0] = ~0ULL; m5.cpu.state.xmm[0][1] = ~0ULL;
            m5.cpu.state.ymm_hi[0][0] = ~0ULL; m5.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m5.run().stop == CpuStop::trap);
            CHECK(m5.cpu.state.xmm[0][0] == 0x0123456789ABCDEFULL);
            CHECK(m5.cpu.state.xmm[0][1] == 0);
            CHECK(m5.cpu.state.ymm_hi[0][0] == 0 && m5.cpu.state.ymm_hi[0][1] == 0);
            const std::uint8_t d_vmovq_xmm[] = {0xC5, 0xFA, 0x7E, 0xC1};
            CHECK(decode_x86(d_vmovq_xmm, sizeof(d_vmovq_xmm)).length == 4);
            CHECK(decode_x86(d_vmovq_xmm, sizeof(d_vmovq_xmm)).supported);

            // pp=2 (F3 0F 7E): vmovq xmm0, [rbx]
            Machine m6{0xC5, 0xFA, 0x7E, 0x03, 0xC3}; // vmovq xmm0, [rbx]; ret
            m6.cpu.state.gpr[3] = DATA;
            const std::uint64_t v_f3_7e = 0xA1B2C3D4E5F60718ULL;
            CHECK(m6.mem.store(DATA, &v_f3_7e, 8) == MemoryError::ok);
            m6.cpu.state.xmm[0][0] = ~0ULL; m6.cpu.state.xmm[0][1] = ~0ULL;
            m6.cpu.state.ymm_hi[0][0] = ~0ULL; m6.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m6.run().stop == CpuStop::trap);
            CHECK(m6.cpu.state.xmm[0][0] == 0xA1B2C3D4E5F60718ULL);
            CHECK(m6.cpu.state.xmm[0][1] == 0);
            CHECK(m6.cpu.state.ymm_hi[0][0] == 0 && m6.cpu.state.ymm_hi[0][1] == 0);

            // pp=1 (66 0F D6): vmovq [rbx], xmm0 (0xD6)
            Machine m7{0xC5, 0xF9, 0xD6, 0x03, 0xC3}; // vmovq [rbx], xmm0; ret
            m7.cpu.state.gpr[3] = DATA;
            m7.cpu.state.xmm[0][0] = 0x1234567890ABCDEFULL;
            CHECK(m7.run().stop == CpuStop::trap);
            std::uint64_t v_d6_mem = 0;
            CHECK(m7.mem.load(DATA, &v_d6_mem, 8) == MemoryError::ok);
            CHECK(v_d6_mem == 0x1234567890ABCDEFULL);
            const std::uint8_t d_vmovq_d6[] = {0xC5, 0xF9, 0xD6, 0x03};
            CHECK(decode_x86(d_vmovq_d6, sizeof(d_vmovq_d6)).length == 4);
            CHECK(decode_x86(d_vmovq_d6, sizeof(d_vmovq_d6)).supported);

            // pp=1 (66 0F D6): vmovq xmm1, xmm0 (0xD6 reg-reg: dest=rm=xmm1, src=reg=xmm0)
            Machine m8{0xC5, 0xF9, 0xD6, 0xC1, 0xC3}; // vmovq xmm1, xmm0; ret
            m8.cpu.state.xmm[0][0] = 0xCAFEBABE11223344ULL;
            m8.cpu.state.xmm[1][0] = ~0ULL; m8.cpu.state.xmm[1][1] = ~0ULL;
            m8.cpu.state.ymm_hi[1][0] = ~0ULL; m8.cpu.state.ymm_hi[1][1] = ~0ULL;
            CHECK(m8.run().stop == CpuStop::trap);
            CHECK(m8.cpu.state.xmm[1][0] == 0xCAFEBABE11223344ULL);
            CHECK(m8.cpu.state.xmm[1][1] == 0);
            CHECK(m8.cpu.state.ymm_hi[1][0] == 0 && m8.cpu.state.ymm_hi[1][1] == 0);
        }
        {   // VCVTSI2SS / VCVTSI2SD (0x2A): r32/r64/mem -> xmm, merges with src1 (vvvv)
            // vcvtsi2ss xmm0, xmm1, ebx (W=0)
            Machine m1{0xC5, 0xF2, 0x2A, 0xC3, 0xC3}; // vcvtsi2ss xmm0, xmm1, ebx; ret
            m1.cpu.state.gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(-500));
            m1.cpu.state.xmm[1][0] = 0x1111111100000000ULL;
            m1.cpu.state.xmm[1][1] = 0x2222222233333333ULL;
            m1.cpu.state.ymm_hi[0][0] = ~0ULL; m1.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m1.run().stop == CpuStop::trap);
            const float f_expected1 = -500.0f;
            std::uint32_t uf1; std::memcpy(&uf1, &f_expected1, 4);
            CHECK((m1.cpu.state.xmm[0][0] & 0xFFFFFFFFULL) == uf1);
            CHECK((m1.cpu.state.xmm[0][0] & ~0xFFFFFFFFULL) == 0x1111111100000000ULL);
            CHECK(m1.cpu.state.xmm[0][1] == 0x2222222233333333ULL);
            CHECK(m1.cpu.state.ymm_hi[0][0] == 0 && m1.cpu.state.ymm_hi[0][1] == 0);
            const std::uint8_t d_vcvtsi2ss[] = {0xC5, 0xF2, 0x2A, 0xC3};
            CHECK(decode_x86(d_vcvtsi2ss, sizeof(d_vcvtsi2ss)).length == 4);
            CHECK(decode_x86(d_vcvtsi2ss, sizeof(d_vcvtsi2ss)).supported);

            // vcvtsi2ss xmm0, xmm1, rbx (W=1)
            Machine m2{0xC4, 0xE1, 0xF2, 0x2A, 0xC3, 0xC3}; // vcvtsi2ss xmm0, xmm1, rbx; ret
            m2.cpu.state.gpr[3] = static_cast<std::uint64_t>(-1234567890123LL);
            m2.cpu.state.xmm[1][0] = 0xAAAAAAAAAAAAAAAAULL;
            m2.cpu.state.xmm[1][1] = 0xBBBBBBBBBBBBBBBBULL;
            m2.cpu.state.ymm_hi[0][0] = ~0ULL; m2.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m2.run().stop == CpuStop::trap);
            const float f_expected2 = static_cast<float>(-1234567890123LL);
            std::uint32_t uf2; std::memcpy(&uf2, &f_expected2, 4);
            CHECK((m2.cpu.state.xmm[0][0] & 0xFFFFFFFFULL) == uf2);
            CHECK((m2.cpu.state.xmm[0][0] & ~0xFFFFFFFFULL) == 0xAAAAAAAA00000000ULL);
            CHECK(m2.cpu.state.xmm[0][1] == 0xBBBBBBBBBBBBBBBBULL);
            CHECK(m2.cpu.state.ymm_hi[0][0] == 0 && m2.cpu.state.ymm_hi[0][1] == 0);

            // vcvtsi2sd xmm0, xmm1, ebx (W=0)
            Machine m3{0xC5, 0xF3, 0x2A, 0xC3, 0xC3}; // vcvtsi2sd xmm0, xmm1, ebx; ret
            m3.cpu.state.gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(-12345));
            m3.cpu.state.xmm[1][0] = 0x7777777788888888ULL;
            m3.cpu.state.xmm[1][1] = 0x123456789ABCDEF0ULL;
            m3.cpu.state.ymm_hi[0][0] = ~0ULL; m3.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m3.run().stop == CpuStop::trap);
            const double d_expected3 = -12345.0;
            std::uint64_t ud3; std::memcpy(&ud3, &d_expected3, 8);
            CHECK(m3.cpu.state.xmm[0][0] == ud3);
            CHECK(m3.cpu.state.xmm[0][1] == 0x123456789ABCDEF0ULL);
            CHECK(m3.cpu.state.ymm_hi[0][0] == 0 && m3.cpu.state.ymm_hi[0][1] == 0);
            const std::uint8_t d_vcvtsi2sd[] = {0xC5, 0xF3, 0x2A, 0xC3};
            CHECK(decode_x86(d_vcvtsi2sd, sizeof(d_vcvtsi2sd)).length == 4);
            CHECK(decode_x86(d_vcvtsi2sd, sizeof(d_vcvtsi2sd)).supported);

            // vcvtsi2sd xmm0, xmm1, rbx (W=1)
            Machine m4{0xC4, 0xE1, 0xF3, 0x2A, 0xC3, 0xC3}; // vcvtsi2sd xmm0, xmm1, rbx; ret
            m4.cpu.state.gpr[3] = 0x100000001LL;
            m4.cpu.state.xmm[1][0] = 0x1111111122222222ULL;
            m4.cpu.state.xmm[1][1] = 0x3333333344444444ULL;
            m4.cpu.state.ymm_hi[0][0] = ~0ULL; m4.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m4.run().stop == CpuStop::trap);
            const double d_expected4 = static_cast<double>(0x100000001LL);
            std::uint64_t ud4; std::memcpy(&ud4, &d_expected4, 8);
            CHECK(m4.cpu.state.xmm[0][0] == ud4);
            CHECK(m4.cpu.state.xmm[0][1] == 0x3333333344444444ULL);
            CHECK(m4.cpu.state.ymm_hi[0][0] == 0 && m4.cpu.state.ymm_hi[0][1] == 0);

            // vcvtsi2ss xmm0, xmm1, [rbx] (mem)
            Machine m5{0xC5, 0xF2, 0x2A, 0x03, 0xC3}; // vcvtsi2ss xmm0, xmm1, [rbx]; ret
            m5.cpu.state.gpr[3] = DATA;
            const std::int32_t mem_int = 42;
            CHECK(m5.mem.store(DATA, &mem_int, 4) == MemoryError::ok);
            m5.cpu.state.xmm[1][0] = 0x5555555500000000ULL;
            m5.cpu.state.xmm[1][1] = 0x6666666677777777ULL;
            CHECK(m5.run().stop == CpuStop::trap);
            const float f_expected5 = 42.0f;
            std::uint32_t uf5; std::memcpy(&uf5, &f_expected5, 4);
            CHECK((m5.cpu.state.xmm[0][0] & 0xFFFFFFFFULL) == uf5);
            CHECK((m5.cpu.state.xmm[0][0] & ~0xFFFFFFFFULL) == 0x5555555500000000ULL);
            CHECK(m5.cpu.state.xmm[0][1] == 0x6666666677777777ULL);
            CHECK(m5.cpu.state.ymm_hi[0][0] == 0 && m5.cpu.state.ymm_hi[0][1] == 0);
        }
        {   // VCVTTSS2SI / VCVTTSD2SI (0x2C): xmm/mem -> r32/r64 truncating
            // vcvttss2si ebx, xmm0 (W=0)
            Machine m1{0xC5, 0xFA, 0x2C, 0xD8, 0xC3}; // vcvttss2si ebx, xmm0; ret
            const float f1 = -42.7f;
            std::uint32_t uf1; std::memcpy(&uf1, &f1, 4);
            m1.cpu.state.xmm[0][0] = uf1;
            m1.cpu.state.gpr[3] = ~0ULL;
            CHECK(m1.run().stop == CpuStop::trap);
            CHECK(m1.cpu.state.gpr[3] == static_cast<std::uint32_t>(-42));
            const std::uint8_t d_vcvttss2si[] = {0xC5, 0xFA, 0x2C, 0xD8};
            CHECK(decode_x86(d_vcvttss2si, sizeof(d_vcvttss2si)).length == 4);
            CHECK(decode_x86(d_vcvttss2si, sizeof(d_vcvttss2si)).supported);

            // vcvttss2si rbx, xmm0 (W=1)
            Machine m2{0xC4, 0xE1, 0xFA, 0x2C, 0xD8, 0xC3}; // vcvttss2si rbx, xmm0; ret
            m2.cpu.state.xmm[0][0] = uf1;
            m2.cpu.state.gpr[3] = 0;
            CHECK(m2.run().stop == CpuStop::trap);
            CHECK(static_cast<std::int64_t>(m2.cpu.state.gpr[3]) == -42);

            // vcvttsd2si ebx, xmm0 (W=0)
            Machine m3{0xC5, 0xFB, 0x2C, 0xD8, 0xC3}; // vcvttsd2si ebx, xmm0; ret
            const double d3 = 12345.8;
            std::uint64_t ud3; std::memcpy(&ud3, &d3, 8);
            m3.cpu.state.xmm[0][0] = ud3;
            m3.cpu.state.gpr[3] = ~0ULL;
            CHECK(m3.run().stop == CpuStop::trap);
            CHECK(m3.cpu.state.gpr[3] == 12345ULL);
            const std::uint8_t d_vcvttsd2si[] = {0xC5, 0xFB, 0x2C, 0xD8};
            CHECK(decode_x86(d_vcvttsd2si, sizeof(d_vcvttsd2si)).length == 4);
            CHECK(decode_x86(d_vcvttsd2si, sizeof(d_vcvttsd2si)).supported);

            // vcvttsd2si rbx, xmm0 (W=1)
            Machine m4{0xC4, 0xE1, 0xFB, 0x2C, 0xD8, 0xC3}; // vcvttsd2si rbx, xmm0; ret
            const double d4 = 12345678901234.0;
            std::uint64_t ud4; std::memcpy(&ud4, &d4, 8);
            m4.cpu.state.xmm[0][0] = ud4;
            m4.cpu.state.gpr[3] = 0;
            CHECK(m4.run().stop == CpuStop::trap);
            CHECK(static_cast<std::int64_t>(m4.cpu.state.gpr[3]) == 12345678901234LL);

            // vcvttss2si ebx, [rbx] (mem)
            Machine m5{0xC5, 0xFA, 0x2C, 0x1B, 0xC3}; // vcvttss2si ebx, [rbx]; ret
            m5.cpu.state.gpr[3] = DATA;
            const float f_mem = 99.9f;
            CHECK(m5.mem.store(DATA, &f_mem, 4) == MemoryError::ok);
            CHECK(m5.run().stop == CpuStop::trap);
            CHECK(m5.cpu.state.gpr[3] == 99ULL);
        }
        {   // VMOVSS / VMOVSD (0x10 / 0x11): reg-reg merge, mem load, mem store
            // vmovss xmm0, xmm1, xmm2 (0x10 reg-reg: dest=xmm0, src1=xmm1, src2=xmm2)
            Machine m1{0xC5, 0xF2, 0x10, 0xC2, 0xC3}; // vmovss xmm0, xmm1, xmm2; ret
            m1.cpu.state.xmm[1][0] = 0xAAAAAAAAAAAAAAAAULL; m1.cpu.state.xmm[1][1] = 0xBBBBBBBBBBBBBBBBULL;
            m1.cpu.state.xmm[2][0] = 0xCCCCCCCCDDDDDDDDULL; m1.cpu.state.xmm[2][1] = 0xEEEEEEEEFFFFFFFFULL;
            m1.cpu.state.ymm_hi[0][0] = ~0ULL; m1.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m1.run().stop == CpuStop::trap);
            CHECK(m1.cpu.state.xmm[0][0] == 0xAAAAAAAADDDDDDDDULL);
            CHECK(m1.cpu.state.xmm[0][1] == 0xBBBBBBBBBBBBBBBBULL);
            CHECK(m1.cpu.state.ymm_hi[0][0] == 0 && m1.cpu.state.ymm_hi[0][1] == 0);
            const std::uint8_t d_vmovss[] = {0xC5, 0xF2, 0x10, 0xC2};
            CHECK(decode_x86(d_vmovss, sizeof(d_vmovss)).length == 4);
            CHECK(decode_x86(d_vmovss, sizeof(d_vmovss)).supported);

            // vmovss xmm0, [rbx] (0x10 mem load: zeroes bits 127:32 and ymm_hi)
            Machine m2{0xC5, 0xFA, 0x10, 0x03, 0xC3}; // vmovss xmm0, [rbx]; ret
            m2.cpu.state.gpr[3] = DATA;
            const std::uint32_t v_ss = 0x12345678;
            CHECK(m2.mem.store(DATA, &v_ss, 4) == MemoryError::ok);
            m2.cpu.state.xmm[0][0] = ~0ULL; m2.cpu.state.xmm[0][1] = ~0ULL;
            m2.cpu.state.ymm_hi[0][0] = ~0ULL; m2.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m2.run().stop == CpuStop::trap);
            CHECK(m2.cpu.state.xmm[0][0] == 0x12345678ULL);
            CHECK(m2.cpu.state.xmm[0][1] == 0);
            CHECK(m2.cpu.state.ymm_hi[0][0] == 0 && m2.cpu.state.ymm_hi[0][1] == 0);

            // vmovss [rbx], xmm0 (0x11 mem store: writes 4 bytes)
            Machine m3{0xC5, 0xFA, 0x11, 0x03, 0xC3}; // vmovss [rbx], xmm0; ret
            m3.cpu.state.gpr[3] = DATA;
            m3.cpu.state.xmm[0][0] = 0xAABBCCDD55667788ULL;
            CHECK(m3.run().stop == CpuStop::trap);
            std::uint32_t out_ss = 0;
            CHECK(m3.mem.load(DATA, &out_ss, 4) == MemoryError::ok);
            CHECK(out_ss == 0x55667788);
            const std::uint8_t d_vmovss_st[] = {0xC5, 0xFA, 0x11, 0x03};
            CHECK(decode_x86(d_vmovss_st, sizeof(d_vmovss_st)).length == 4);
            CHECK(decode_x86(d_vmovss_st, sizeof(d_vmovss_st)).supported);

            // vmovss xmm0, xmm1, xmm2 (0x11 reg-reg: dest=xmm0, src1=xmm1, src2=xmm2)
            Machine m4{0xC5, 0xF2, 0x11, 0xD0, 0xC3}; // vmovss xmm0, xmm1, xmm2
            m4.cpu.state.xmm[1][0] = 0x1111111122222222ULL; m4.cpu.state.xmm[1][1] = 0x3333333344444444ULL;
            m4.cpu.state.xmm[2][0] = 0x5555555566666666ULL; m4.cpu.state.xmm[2][1] = 0x7777777788888888ULL;
            m4.cpu.state.ymm_hi[0][0] = ~0ULL; m4.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m4.run().stop == CpuStop::trap);
            CHECK(m4.cpu.state.xmm[0][0] == 0x1111111166666666ULL);
            CHECK(m4.cpu.state.xmm[0][1] == 0x3333333344444444ULL);
            CHECK(m4.cpu.state.ymm_hi[0][0] == 0 && m4.cpu.state.ymm_hi[0][1] == 0);

            // vmovsd xmm0, xmm1, xmm2 (0x10 reg-reg: dest=xmm0, src1=xmm1, src2=xmm2)
            Machine m5{0xC5, 0xF3, 0x10, 0xC2, 0xC3}; // vmovsd xmm0, xmm1, xmm2; ret
            m5.cpu.state.xmm[1][0] = 0x1111111111111111ULL; m5.cpu.state.xmm[1][1] = 0x2222222222222222ULL;
            m5.cpu.state.xmm[2][0] = 0x3333333333333333ULL; m5.cpu.state.xmm[2][1] = 0x4444444444444444ULL;
            m5.cpu.state.ymm_hi[0][0] = ~0ULL; m5.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m5.run().stop == CpuStop::trap);
            CHECK(m5.cpu.state.xmm[0][0] == 0x3333333333333333ULL);
            CHECK(m5.cpu.state.xmm[0][1] == 0x2222222222222222ULL);
            CHECK(m5.cpu.state.ymm_hi[0][0] == 0 && m5.cpu.state.ymm_hi[0][1] == 0);
            const std::uint8_t d_vmovsd[] = {0xC5, 0xF3, 0x10, 0xC2};
            CHECK(decode_x86(d_vmovsd, sizeof(d_vmovsd)).length == 4);
            CHECK(decode_x86(d_vmovsd, sizeof(d_vmovsd)).supported);

            // vmovsd xmm0, [rbx] (0x10 mem load: zeroes bits 127:64 and ymm_hi)
            Machine m6{0xC5, 0xFB, 0x10, 0x03, 0xC3}; // vmovsd xmm0, [rbx]; ret
            m6.cpu.state.gpr[3] = DATA;
            const std::uint64_t v_sd = 0x0123456789ABCDEFULL;
            CHECK(m6.mem.store(DATA, &v_sd, 8) == MemoryError::ok);
            m6.cpu.state.xmm[0][0] = ~0ULL; m6.cpu.state.xmm[0][1] = ~0ULL;
            m6.cpu.state.ymm_hi[0][0] = ~0ULL; m6.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m6.run().stop == CpuStop::trap);
            CHECK(m6.cpu.state.xmm[0][0] == 0x0123456789ABCDEFULL);
            CHECK(m6.cpu.state.xmm[0][1] == 0);
            CHECK(m6.cpu.state.ymm_hi[0][0] == 0 && m6.cpu.state.ymm_hi[0][1] == 0);

            // vmovsd [rbx], xmm0 (0x11 mem store: writes 8 bytes)
            Machine m7{0xC5, 0xFB, 0x11, 0x03, 0xC3}; // vmovsd [rbx], xmm0; ret
            m7.cpu.state.gpr[3] = DATA;
            m7.cpu.state.xmm[0][0] = 0xCAFEBABE11223344ULL;
            CHECK(m7.run().stop == CpuStop::trap);
            std::uint64_t out_sd = 0;
            CHECK(m7.mem.load(DATA, &out_sd, 8) == MemoryError::ok);
            CHECK(out_sd == 0xCAFEBABE11223344ULL);
            const std::uint8_t d_vmovsd_st[] = {0xC5, 0xFB, 0x11, 0x03};
            CHECK(decode_x86(d_vmovsd_st, sizeof(d_vmovsd_st)).length == 4);
            CHECK(decode_x86(d_vmovsd_st, sizeof(d_vmovsd_st)).supported);

            // vmovsd xmm0, xmm1, xmm2 (0x11 reg-reg)
            Machine m8{0xC5, 0xF3, 0x11, 0xD0, 0xC3};
            m8.cpu.state.xmm[1][0] = 0x1111111111111111ULL; m8.cpu.state.xmm[1][1] = 0x2222222222222222ULL;
            m8.cpu.state.xmm[2][0] = 0x3333333333333333ULL; m8.cpu.state.xmm[2][1] = 0x4444444444444444ULL;
            m8.cpu.state.ymm_hi[0][0] = ~0ULL; m8.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m8.run().stop == CpuStop::trap);
            CHECK(m8.cpu.state.xmm[0][0] == 0x3333333333333333ULL);
            CHECK(m8.cpu.state.xmm[0][1] == 0x2222222222222222ULL);
            CHECK(m8.cpu.state.ymm_hi[0][0] == 0 && m8.cpu.state.ymm_hi[0][1] == 0);
        }
        {   // Scalar VEX rejection: reserved vvvv != 0 on mem operands and L=1
            for (auto bytes : {
                     std::initializer_list<std::uint8_t>{0xC5, 0xF2, 0x10, 0x03}, // vmovss mem with vvvv!=0
                     std::initializer_list<std::uint8_t>{0xC5, 0xF3, 0x10, 0x03}, // vmovsd mem with vvvv!=0
                     std::initializer_list<std::uint8_t>{0xC5, 0xF1, 0x6E, 0xC3}, // vmovd with vvvv!=0
                     std::initializer_list<std::uint8_t>{0xC5, 0xFD, 0x6E, 0xC3}, // vmovd with L=1
                     std::initializer_list<std::uint8_t>{0xC5, 0xF6, 0x2A, 0xC3}, // vcvtsi2ss with L=1
                     std::initializer_list<std::uint8_t>{0xC5, 0xFE, 0x2C, 0xD8}, // vcvttss2si with L=1
                 }) {
                Machine m(bytes);
                CHECK(m.run().stop == CpuStop::unsupported_opcode);
            }
        }
        {   // Deterministic differential check: scanner length vs executed next RIP.
            // These are generated synthetic single instructions, not linear code coverage.
            std::uint32_t seed = 0x51A0B1C2;
            for (int i = 0; i < 512; ++i) {
                seed = seed * 1664525u + 1013904223u;
                std::vector<std::uint8_t> code;
                const bool has_p66 = (seed & 1) != 0;
                if (has_p66) code.push_back(0x66);
                const bool has_rex = (seed & 2) != 0;
                const uint8_t rex_byte = static_cast<std::uint8_t>(0x40 | ((seed >> 4) & 15));
                if (has_rex) code.push_back(rex_byte);
                const bool is_w = has_rex && ((rex_byte & 8) != 0);
                const bool imm16 = has_p66 && !is_w;
                switch ((seed >> 8) % 5) {
                case 0: code.insert(code.end(), {0x83, static_cast<std::uint8_t>(0xC0 | ((seed >> 16) & 7)), 3}); break;
                case 1: code.insert(code.end(), {0x89, static_cast<std::uint8_t>(0xC0 | ((seed >> 16) & 63))}); break;
                case 2:
                    code.insert(code.end(), {0xC7, static_cast<std::uint8_t>(0xC0 | ((seed >> 16) & 7)), 1, 0});
                    if (!imm16) code.insert(code.end(), {0, 0});
                    break;
                case 3: code.insert(code.end(), {0x0F, 0x57, static_cast<std::uint8_t>(0xC0 | ((seed >> 16) & 63))}); break;
                default: code.insert(code.end(), {0x0F, 0xAF, static_cast<std::uint8_t>(0xC0 | ((seed >> 16) & 63))}); break;
                }
                const auto d = decode_x86(code.data(), code.size());
                CHECK(d.length == code.size());
                Machine m{0x90};
                CHECK(m.mem.store(CODE, code.data(), code.size()) == MemoryError::ok);
                const auto r = m.run(1);
                CHECK(r.stop == CpuStop::step_limit);
                CHECK(m.cpu.state.rip == CODE + d.length);
            }
        }
        {   // BMI1 ANDN (0xF2): dest = ~src1 & src2
            // 64-bit reg-reg (W=1): andn rax, rbx, rcx
            // C4 E2 E0 F2 C1: W=1, vvvv=rbx(3), ModRM: reg=rax(0), rm=rcx(1)
            Machine m1{0xC4, 0xE2, 0xE0, 0xF2, 0xC1, 0xC3};
            m1.cpu.state.gpr[3] = 0x0000FFFF0000FFFFULL; // rbx (src1)
            m1.cpu.state.gpr[1] = 0x123456789ABCDEF0ULL; // rcx (src2)
            m1.cpu.state.rflags |= FLAG_CF | FLAG_OF | FLAG_ZF | FLAG_SF;
            CHECK(m1.run().stop == CpuStop::trap);
            CHECK(m1.cpu.state.gpr[0] == 0x123400009ABC0000ULL);
            CHECK((m1.cpu.state.rflags & (FLAG_CF | FLAG_OF | FLAG_ZF | FLAG_SF)) == 0);
            const std::uint8_t d_andn64[] = {0xC4, 0xE2, 0xE0, 0xF2, 0xC1};
            CHECK(decode_x86(d_andn64, sizeof(d_andn64)).length == 5);
            CHECK(decode_x86(d_andn64, sizeof(d_andn64)).supported);

            // 64-bit negative result sets SF: andn rax, rbx, rcx
            Machine m2{0xC4, 0xE2, 0xE0, 0xF2, 0xC1, 0xC3};
            m2.cpu.state.gpr[3] = 0; // rbx -> ~rbx = ~0
            m2.cpu.state.gpr[1] = 0x8000000000000001ULL; // rcx
            CHECK(m2.run().stop == CpuStop::trap);
            CHECK(m2.cpu.state.gpr[0] == 0x8000000000000001ULL);
            CHECK((m2.cpu.state.rflags & (FLAG_CF | FLAG_OF | FLAG_ZF | FLAG_SF)) == FLAG_SF);

            // 64-bit zero result sets ZF: andn rax, rbx, rcx
            Machine m3{0xC4, 0xE2, 0xE0, 0xF2, 0xC1, 0xC3};
            m3.cpu.state.gpr[3] = ~0ULL; // rbx -> ~rbx = 0
            m3.cpu.state.gpr[1] = 0x1234ULL; // rcx
            CHECK(m3.run().stop == CpuStop::trap);
            CHECK(m3.cpu.state.gpr[0] == 0);
            CHECK((m3.cpu.state.rflags & (FLAG_CF | FLAG_OF | FLAG_ZF | FLAG_SF)) == FLAG_ZF);

            // 32-bit mem (W=0): andn eax, ebx, [rcx]
            Machine m4{0xC4, 0xE2, 0x60, 0xF2, 0x01, 0xC3};
            m4.cpu.state.gpr[3] = 0x0F0F0F0FULL; // ebx
            m4.cpu.state.gpr[1] = DATA;
            const std::uint32_t mem_v = 0xF0F0F0F0U;
            CHECK(m4.mem.store(DATA, &mem_v, 4) == MemoryError::ok);
            m4.cpu.state.gpr[0] = ~0ULL;
            CHECK(m4.run().stop == CpuStop::trap);
            CHECK(m4.cpu.state.gpr[0] == 0xF0F0F0F0ULL);
            CHECK((m4.cpu.state.rflags & (FLAG_CF | FLAG_OF | FLAG_ZF | FLAG_SF)) == FLAG_SF);
            const std::uint8_t d_andn32[] = {0xC4, 0xE2, 0x60, 0xF2, 0x01};
            CHECK(decode_x86(d_andn32, sizeof(d_andn32)).length == 5);
            CHECK(decode_x86(d_andn32, sizeof(d_andn32)).supported);

            // Actual libc.prx sequence: c4 e2 f8 f2 c6 = andn rax, rax, rsi
            Machine m5{0xC4, 0xE2, 0xF8, 0xF2, 0xC6, 0xC3};
            m5.cpu.state.gpr[0] = 0x00FF00FF00FF00FFULL;
            m5.cpu.state.gpr[6] = 0xAAAAAAAAAAAAAAAAULL;
            CHECK(m5.run().stop == CpuStop::trap);
            CHECK(m5.cpu.state.gpr[0] == (~0x00FF00FF00FF00FFULL & 0xAAAAAAAAAAAAAAAAULL));
            const std::uint8_t d_libc_andn[] = {0xC4, 0xE2, 0xF8, 0xF2, 0xC6};
            CHECK(decode_x86(d_libc_andn, sizeof(d_libc_andn)).length == 5);
            CHECK(decode_x86(d_libc_andn, sizeof(d_libc_andn)).supported);
        }
        {   // BMI1 BEXTR (0xF7): dest = (src >> start) & mask
            // 32-bit: bextr eax, ebx, ecx (W=0, vvvv=ecx(1), reg=eax(0), rm=ebx(3))
            Machine m1{0xC4, 0xE2, 0x70, 0xF7, 0xC3, 0xC3};
            m1.cpu.state.gpr[3] = 0x12345678ULL;
            m1.cpu.state.gpr[1] = 0x0808ULL; // start=8, len=8
            CHECK(m1.run().stop == CpuStop::trap);
            CHECK(m1.cpu.state.gpr[0] == 0x56ULL);
            CHECK((m1.cpu.state.rflags & (FLAG_CF | FLAG_OF | FLAG_ZF | FLAG_SF)) == 0);
            const std::uint8_t d_bextr32[] = {0xC4, 0xE2, 0x70, 0xF7, 0xC3};
            CHECK(decode_x86(d_bextr32, sizeof(d_bextr32)).length == 5);
            CHECK(decode_x86(d_bextr32, sizeof(d_bextr32)).supported);

            // 64-bit: bextr rax, rbx, rcx (W=1)
            Machine m2{0xC4, 0xE2, 0xF0, 0xF7, 0xC3, 0xC3};
            m2.cpu.state.gpr[3] = 0x0123456789ABCDEFULL;
            m2.cpu.state.gpr[1] = 0x1020ULL; // start=32, len=16
            CHECK(m2.run().stop == CpuStop::trap);
            CHECK(m2.cpu.state.gpr[0] == 0x4567ULL);
        }
        {   // VMOVDQU (pp=2) and VMOVDQA (pp=1): 128-bit (L=0) and 256-bit (L=1)
            // vmovdqu xmm0, xmm1 (128-bit reg-reg, zeroes ymm_hi)
            Machine m1{0xC5, 0xFA, 0x6F, 0xC1, 0xC3}; // vmovdqu xmm0, xmm1; ret
            m1.cpu.state.xmm[1][0] = 0x1111222233334444ULL;
            m1.cpu.state.xmm[1][1] = 0x5555666677778888ULL;
            m1.cpu.state.ymm_hi[0][0] = ~0ULL; m1.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m1.run().stop == CpuStop::trap);
            CHECK(m1.cpu.state.xmm[0][0] == 0x1111222233334444ULL);
            CHECK(m1.cpu.state.xmm[0][1] == 0x5555666677778888ULL);
            CHECK(m1.cpu.state.ymm_hi[0][0] == 0 && m1.cpu.state.ymm_hi[0][1] == 0);
            const std::uint8_t d_vmovdqu128[] = {0xC5, 0xFA, 0x6F, 0xC1};
            CHECK(decode_x86(d_vmovdqu128, sizeof(d_vmovdqu128)).length == 4);
            CHECK(decode_x86(d_vmovdqu128, sizeof(d_vmovdqu128)).supported);

            // vmovdqu [rbx], xmm0 (128-bit store)
            Machine m2{0xC5, 0xFA, 0x7F, 0x03, 0xC3}; // vmovdqu [rbx], xmm0; ret
            m2.cpu.state.gpr[3] = DATA;
            m2.cpu.state.xmm[0][0] = 0x0102030405060708ULL;
            m2.cpu.state.xmm[0][1] = 0x090A0B0C0D0E0F10ULL;
            CHECK(m2.run().stop == CpuStop::trap);
            std::uint64_t mem128[2] = {};
            CHECK(m2.mem.load(DATA, mem128, 16) == MemoryError::ok);
            CHECK(mem128[0] == 0x0102030405060708ULL && mem128[1] == 0x090A0B0C0D0E0F10ULL);

            // vmovdqa xmm0, [rbx] (128-bit load, pp=1)
            Machine m3{0xC5, 0xF9, 0x6F, 0x03, 0xC3}; // vmovdqa xmm0, [rbx]; ret
            m3.cpu.state.gpr[3] = DATA;
            const std::uint64_t v_dqa[2] = {0x0102030405060708ULL, 0x090A0B0C0D0E0F10ULL};
            CHECK(m3.mem.store(DATA, v_dqa, 16) == MemoryError::ok);
            m3.cpu.state.ymm_hi[0][0] = ~0ULL; m3.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m3.run().stop == CpuStop::trap);
            CHECK(m3.cpu.state.xmm[0][0] == 0x0102030405060708ULL && m3.cpu.state.xmm[0][1] == 0x090A0B0C0D0E0F10ULL);
            CHECK(m3.cpu.state.ymm_hi[0][0] == 0 && m3.cpu.state.ymm_hi[0][1] == 0);
            const std::uint8_t d_vmovdqa128[] = {0xC5, 0xF9, 0x6F, 0x03};
            CHECK(decode_x86(d_vmovdqa128, sizeof(d_vmovdqa128)).length == 4);
            CHECK(decode_x86(d_vmovdqa128, sizeof(d_vmovdqa128)).supported);

            // vmovdqu ymm0, [rbx] (256-bit load, L=1, pp=2)
            Machine m4{0xC5, 0xFE, 0x6F, 0x03, 0xC3}; // vmovdqu ymm0, [rbx]; ret
            m4.cpu.state.gpr[3] = DATA;
            const std::uint64_t mem256[4] = {0x1111ULL, 0x2222ULL, 0x3333ULL, 0x4444ULL};
            CHECK(m4.mem.store(DATA, mem256, 32) == MemoryError::ok);
            CHECK(m4.run().stop == CpuStop::trap);
            CHECK(m4.cpu.state.xmm[0][0] == 0x1111ULL && m4.cpu.state.xmm[0][1] == 0x2222ULL);
            CHECK(m4.cpu.state.ymm_hi[0][0] == 0x3333ULL && m4.cpu.state.ymm_hi[0][1] == 0x4444ULL);
            const std::uint8_t d_vmovdqu256[] = {0xC5, 0xFE, 0x6F, 0x03};
            CHECK(decode_x86(d_vmovdqu256, sizeof(d_vmovdqu256)).length == 4);
            CHECK(decode_x86(d_vmovdqu256, sizeof(d_vmovdqu256)).supported);

            // vmovdqa ymm1, ymm0 (256-bit reg-reg, L=1, pp=1)
            Machine m5{0xC5, 0xFD, 0x6F, 0xC8, 0xC3}; // vmovdqa ymm1, ymm0; ret
            m5.cpu.state.xmm[0][0] = 0xAAAAULL; m5.cpu.state.xmm[0][1] = 0xBBBBULL;
            m5.cpu.state.ymm_hi[0][0] = 0xCCCCULL; m5.cpu.state.ymm_hi[0][1] = 0xDDDDULL;
            CHECK(m5.run().stop == CpuStop::trap);
            CHECK(m5.cpu.state.xmm[1][0] == 0xAAAAULL && m5.cpu.state.xmm[1][1] == 0xBBBBULL);
            CHECK(m5.cpu.state.ymm_hi[1][0] == 0xCCCCULL && m5.cpu.state.ymm_hi[1][1] == 0xDDDDULL);

            // vmovdqu [rbx], ymm1 (256-bit store, L=1, pp=2)
            Machine m6{0xC5, 0xFE, 0x7F, 0x0B, 0xC3}; // vmovdqu [rbx], ymm1; ret
            m6.cpu.state.gpr[3] = DATA;
            m6.cpu.state.xmm[1][0] = 0x5555ULL; m6.cpu.state.xmm[1][1] = 0x6666ULL;
            m6.cpu.state.ymm_hi[1][0] = 0x7777ULL; m6.cpu.state.ymm_hi[1][1] = 0x8888ULL;
            CHECK(m6.run().stop == CpuStop::trap);
            std::uint64_t out256[4] = {};
            CHECK(m6.mem.load(DATA, out256, 32) == MemoryError::ok);
            CHECK(out256[0] == 0x5555ULL && out256[1] == 0x6666ULL && out256[2] == 0x7777ULL && out256[3] == 0x8888ULL);
        }
        {   // Unpacks (map 0F, pp=1, L=0 e L=1)
            // vpunpcklqdq xmm0, xmm1, xmm2 (0x6C, L=0): low qwords from src1 and src2
            Machine m1{0xC5, 0xF1, 0x6C, 0xC2, 0xC3}; // vpunpcklqdq xmm0, xmm1, xmm2; ret
            m1.cpu.state.xmm[1][0] = 0x1111111111111111ULL; m1.cpu.state.xmm[1][1] = 0x2222222222222222ULL;
            m1.cpu.state.xmm[2][0] = 0x3333333333333333ULL; m1.cpu.state.xmm[2][1] = 0x4444444444444444ULL;
            m1.cpu.state.ymm_hi[0][0] = ~0ULL; m1.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m1.run().stop == CpuStop::trap);
            CHECK(m1.cpu.state.xmm[0][0] == 0x1111111111111111ULL);
            CHECK(m1.cpu.state.xmm[0][1] == 0x3333333333333333ULL);
            CHECK(m1.cpu.state.ymm_hi[0][0] == 0 && m1.cpu.state.ymm_hi[0][1] == 0);
            const std::uint8_t d_vpunpcklqdq[] = {0xC5, 0xF1, 0x6C, 0xC2};
            CHECK(decode_x86(d_vpunpcklqdq, sizeof(d_vpunpcklqdq)).length == 4);
            CHECK(decode_x86(d_vpunpcklqdq, sizeof(d_vpunpcklqdq)).supported);

            // Actual eboot.bin sequence: c5 f9 6c c1 = vpunpcklqdq xmm0, xmm0, xmm1
            Machine m1_eboot{0xC5, 0xF9, 0x6C, 0xC1, 0xC3};
            m1_eboot.cpu.state.xmm[0][0] = 0xAAAAAAAAAAAAAAAAULL; m1_eboot.cpu.state.xmm[0][1] = 0xBBBBBBBBBBBBBBBBULL;
            m1_eboot.cpu.state.xmm[1][0] = 0xCCCCCCCCCCCCCCCCULL; m1_eboot.cpu.state.xmm[1][1] = 0xDDDDDDDDDDDDDDDDULL;
            CHECK(m1_eboot.run().stop == CpuStop::trap);
            CHECK(m1_eboot.cpu.state.xmm[0][0] == 0xAAAAAAAAAAAAAAAAULL);
            CHECK(m1_eboot.cpu.state.xmm[0][1] == 0xCCCCCCCCCCCCCCCCULL);

            // vpunpckhqdq xmm0, xmm1, xmm2 (0x6D, L=0): high qwords from src1 and src2
            Machine m2{0xC5, 0xF1, 0x6D, 0xC2, 0xC3};
            m2.cpu.state.xmm[1][0] = 0x1111ULL; m2.cpu.state.xmm[1][1] = 0x2222ULL;
            m2.cpu.state.xmm[2][0] = 0x3333ULL; m2.cpu.state.xmm[2][1] = 0x4444ULL;
            CHECK(m2.run().stop == CpuStop::trap);
            CHECK(m2.cpu.state.xmm[0][0] == 0x2222ULL);
            CHECK(m2.cpu.state.xmm[0][1] == 0x4444ULL);

            // vpunpckldq xmm0, xmm1, xmm2 (0x62, L=0): low dwords
            Machine m3{0xC5, 0xF1, 0x62, 0xC2, 0xC3};
            m3.cpu.state.xmm[1][0] = 0x0000000200000001ULL; m3.cpu.state.xmm[1][1] = 0;
            m3.cpu.state.xmm[2][0] = 0x0000000400000003ULL; m3.cpu.state.xmm[2][1] = 0;
            CHECK(m3.run().stop == CpuStop::trap);
            CHECK(m3.cpu.state.xmm[0][0] == 0x0000000300000001ULL);
            CHECK(m3.cpu.state.xmm[0][1] == 0x0000000400000002ULL);

            // vpunpckhdq xmm0, xmm1, xmm2 (0x6A, L=0): high dwords
            Machine m4{0xC5, 0xF1, 0x6A, 0xC2, 0xC3};
            m4.cpu.state.xmm[1][0] = 0; m4.cpu.state.xmm[1][1] = 0x0000000600000005ULL;
            m4.cpu.state.xmm[2][0] = 0; m4.cpu.state.xmm[2][1] = 0x0000000800000007ULL;
            CHECK(m4.run().stop == CpuStop::trap);
            CHECK(m4.cpu.state.xmm[0][0] == 0x0000000700000005ULL);
            CHECK(m4.cpu.state.xmm[0][1] == 0x0000000800000006ULL);

            // vpunpcklbw xmm0, xmm1, xmm2 (0x60, L=0): low bytes
            Machine m5{0xC5, 0xF1, 0x60, 0xC2, 0xC3};
            m5.cpu.state.xmm[1][0] = 0x04030201ULL;
            m5.cpu.state.xmm[2][0] = 0x08070605ULL;
            CHECK(m5.run().stop == CpuStop::trap);
            CHECK((m5.cpu.state.xmm[0][0] & 0xFFFFFFFFULL) == 0x06020501ULL);

            // vpunpcklwd xmm0, xmm1, xmm2 (0x61, L=0): low words
            Machine m6{0xC5, 0xF1, 0x61, 0xC2, 0xC3};
            m6.cpu.state.xmm[1][0] = 0x00020001ULL;
            m6.cpu.state.xmm[2][0] = 0x00040003ULL;
            CHECK(m6.run().stop == CpuStop::trap);
            CHECK(m6.cpu.state.xmm[0][0] == 0x0004000200030001ULL);

            // 256-bit unpack (L=1): vpunpcklqdq ymm0, ymm1, ymm2
            Machine m7{0xC5, 0xF5, 0x6C, 0xC2, 0xC3}; // L=1
            m7.cpu.state.xmm[1][0] = 1; m7.cpu.state.xmm[1][1] = 2;
            m7.cpu.state.ymm_hi[1][0] = 5; m7.cpu.state.ymm_hi[1][1] = 6;
            m7.cpu.state.xmm[2][0] = 3; m7.cpu.state.xmm[2][1] = 4;
            m7.cpu.state.ymm_hi[2][0] = 7; m7.cpu.state.ymm_hi[2][1] = 8;
            CHECK(m7.run().stop == CpuStop::trap);
            CHECK(m7.cpu.state.xmm[0][0] == 1 && m7.cpu.state.xmm[0][1] == 3);
            CHECK(m7.cpu.state.ymm_hi[0][0] == 5 && m7.cpu.state.ymm_hi[0][1] == 7);
        }
        {   // VPADD and VPSUB (map 0F, pp=1, L=0 and L=1)
            // vpaddq xmm0, xmm1, xmm2 (0xD4, L=0)
            Machine m1{0xC5, 0xF1, 0xD4, 0xC2, 0xC3};
            m1.cpu.state.xmm[1][0] = 100ULL; m1.cpu.state.xmm[1][1] = 200ULL;
            m1.cpu.state.xmm[2][0] = 50ULL;  m1.cpu.state.xmm[2][1] = 30ULL;
            m1.cpu.state.ymm_hi[0][0] = ~0ULL; m1.cpu.state.ymm_hi[0][1] = ~0ULL;
            CHECK(m1.run().stop == CpuStop::trap);
            CHECK(m1.cpu.state.xmm[0][0] == 150ULL);
            CHECK(m1.cpu.state.xmm[0][1] == 230ULL);
            CHECK(m1.cpu.state.ymm_hi[0][0] == 0 && m1.cpu.state.ymm_hi[0][1] == 0);
            const std::uint8_t d_vpaddq[] = {0xC5, 0xF1, 0xD4, 0xC2};
            CHECK(decode_x86(d_vpaddq, sizeof(d_vpaddq)).length == 4);
            CHECK(decode_x86(d_vpaddq, sizeof(d_vpaddq)).supported);

            // Actual eboot.bin sequence: c4 c1 79 d4 46 18 = vpaddq xmm0, xmm0, [r14+0x18]
            Machine m_eboot_add{0xC4, 0xC1, 0x79, 0xD4, 0x46, 0x18, 0xC3};
            m_eboot_add.cpu.state.gpr[14] = DATA;
            const std::uint64_t mem_qwords[2] = {111ULL, 222ULL};
            CHECK(m_eboot_add.mem.store(DATA + 0x18, mem_qwords, 16) == MemoryError::ok);
            m_eboot_add.cpu.state.xmm[0][0] = 10ULL; m_eboot_add.cpu.state.xmm[0][1] = 20ULL;
            CHECK(m_eboot_add.run().stop == CpuStop::trap);
            CHECK(m_eboot_add.cpu.state.xmm[0][0] == 121ULL);
            CHECK(m_eboot_add.cpu.state.xmm[0][1] == 242ULL);

            // vpaddd xmm0, xmm1, xmm2 (0xFE, L=0)
            Machine m2{0xC5, 0xF1, 0xFE, 0xC2, 0xC3};
            m2.cpu.state.xmm[1][0] = 0x0000000A00000005ULL;
            m2.cpu.state.xmm[2][0] = 0x0000000300000002ULL;
            CHECK(m2.run().stop == CpuStop::trap);
            CHECK(m2.cpu.state.xmm[0][0] == 0x0000000D00000007ULL);

            // vpaddw (0xFD)
            Machine m3{0xC5, 0xF1, 0xFD, 0xC2, 0xC3};
            m3.cpu.state.xmm[1][0] = 0x00100020ULL;
            m3.cpu.state.xmm[2][0] = 0x00050002ULL;
            CHECK(m3.run().stop == CpuStop::trap);
            CHECK(m3.cpu.state.xmm[0][0] == 0x00150022ULL);

            // vpaddb (0xFC)
            Machine m3b{0xC5, 0xF1, 0xFC, 0xC2, 0xC3};
            m3b.cpu.state.xmm[1][0] = 0x01020304ULL;
            m3b.cpu.state.xmm[2][0] = 0x05060708ULL;
            CHECK(m3b.run().stop == CpuStop::trap);
            CHECK(m3b.cpu.state.xmm[0][0] == 0x06080A0CULL);

            // vpsubq xmm0, xmm1, xmm2 (0xFB, L=0)
            Machine m4{0xC5, 0xF1, 0xFB, 0xC2, 0xC3};
            m4.cpu.state.xmm[1][0] = 500ULL; m4.cpu.state.xmm[1][1] = 300ULL;
            m4.cpu.state.xmm[2][0] = 200ULL; m4.cpu.state.xmm[2][1] = 100ULL;
            CHECK(m4.run().stop == CpuStop::trap);
            CHECK(m4.cpu.state.xmm[0][0] == 300ULL && m4.cpu.state.xmm[0][1] == 200ULL);

            // vpsubd (0xFA), vpsubw (0xF9), vpsubb (0xF8)
            Machine m5{0xC5, 0xF1, 0xFA, 0xC2, 0xC3};
            m5.cpu.state.xmm[1][0] = 0x0000001000000020ULL;
            m5.cpu.state.xmm[2][0] = 0x0000000500000008ULL;
            CHECK(m5.run().stop == CpuStop::trap);
            CHECK(m5.cpu.state.xmm[0][0] == 0x0000000B00000018ULL);

            // 256-bit vpaddq (L=1)
            Machine m6{0xC5, 0xF5, 0xD4, 0xC2, 0xC3}; // L=1
            m6.cpu.state.xmm[1][0] = 1; m6.cpu.state.xmm[1][1] = 2;
            m6.cpu.state.ymm_hi[1][0] = 3; m6.cpu.state.ymm_hi[1][1] = 4;
            m6.cpu.state.xmm[2][0] = 10; m6.cpu.state.xmm[2][1] = 20;
            m6.cpu.state.ymm_hi[2][0] = 30; m6.cpu.state.ymm_hi[2][1] = 40;
            CHECK(m6.run().stop == CpuStop::trap);
            CHECK(m6.cpu.state.xmm[0][0] == 11 && m6.cpu.state.xmm[0][1] == 22);
            CHECK(m6.cpu.state.ymm_hi[0][0] == 33 && m6.cpu.state.ymm_hi[0][1] == 44);
        }
        {   // AVX move-high/low: aliasing, exact memory width and reserved forms.
            Machine lh{0xC5, 0xF0, 0x16, 0xC1, 0xC3}; // vmovlhps xmm0, xmm1, xmm1
            lh.cpu.state.xmm[1][0] = 0x11; lh.cpu.state.xmm[1][1] = 0x22;
            CHECK(lh.run().stop == CpuStop::trap);
            CHECK(lh.cpu.state.xmm[0][0] == 0x11 && lh.cpu.state.xmm[0][1] == 0x11);
            Machine lh_alias{0xC5, 0xF8, 0x16, 0xC0, 0xC3}; // all operands xmm0
            lh_alias.cpu.state.xmm[0][0] = 0x11; lh_alias.cpu.state.xmm[0][1] = 0x22;
            CHECK(lh_alias.run().stop == CpuStop::trap);
            CHECK(lh_alias.cpu.state.xmm[0][0] == 0x11 && lh_alias.cpu.state.xmm[0][1] == 0x11);
            Machine hl{0xC5, 0xF0, 0x12, 0xC1, 0xC3}; // vmovhlps xmm0, xmm1, xmm1
            hl.cpu.state.xmm[1][1] = 0x22;
            CHECK(hl.run().stop == CpuStop::trap);
            CHECK(hl.cpu.state.xmm[0][0] == 0x22 && hl.cpu.state.xmm[0][1] == 0x22);
            Machine dup{0xC5, 0xFF, 0x12, 0xC0, 0xC3}; // vmovddup ymm0, ymm0
            dup.cpu.state.xmm[0][0] = 1; dup.cpu.state.xmm[0][1] = 2;
            dup.cpu.state.ymm_hi[0][0] = 3; dup.cpu.state.ymm_hi[0][1] = 4;
            CHECK(dup.run().stop == CpuStop::trap);
            CHECK(dup.cpu.state.xmm[0][0] == 1 && dup.cpu.state.xmm[0][1] == 1);
            CHECK(dup.cpu.state.ymm_hi[0][0] == 3 && dup.cpu.state.ymm_hi[0][1] == 3);
            Machine dup_mem{0xC5, 0xFB, 0x12, 0x03, 0xC3}; // vmovddup xmm0, m64 at page end
            dup_mem.cpu.state.gpr[3] = DATA + 0xFF8;
            const std::uint64_t value = 0x1122334455667788ULL;
            CHECK(dup_mem.mem.store(DATA + 0xFF8, &value, 8) == MemoryError::ok);
            CHECK(dup_mem.run().stop == CpuStop::trap);
            CHECK(dup_mem.cpu.state.xmm[0][0] == value && dup_mem.cpu.state.xmm[0][1] == value);
            Machine low_mem{0xC5, 0xF0, 0x12, 0x03, 0xC3}; // vmovlps xmm0, xmm1, m64
            low_mem.cpu.state.gpr[3] = DATA + 0xFF8;
            low_mem.cpu.state.xmm[1][1] = 0x44;
            CHECK(low_mem.mem.store(DATA + 0xFF8, &value, 8) == MemoryError::ok);
            CHECK(low_mem.run().stop == CpuStop::trap);
            CHECK(low_mem.cpu.state.xmm[0][0] == value && low_mem.cpu.state.xmm[0][1] == 0x44);
            Machine bad{0xC5, 0xF9, 0x12, 0xC1}; // vmovlpd rejects register operand
            CHECK(bad.run().stop == CpuStop::unsupported_opcode);
            Machine bad_len{0xC5, 0xFC, 0x16, 0xC1}; // vmovlhps has no 256-bit form
            CHECK(bad_len.run().stop == CpuStop::unsupported_opcode);
            Machine bad_vvvv{0xC5, 0xF1, 0x13, 0x03}; // store requires reserved vvvv
            CHECK(bad_vvvv.run().stop == CpuStop::unsupported_opcode);
        }
        {   // Horizontal AVX: source pairs grouped per 128-bit lane.
            // c5 fb 7c c0 and c5 f3 7c c9 are VHADDPS, not VPCMPGTQ.
            const std::uint8_t observed[] = {0xC5, 0xFB, 0x7C, 0xC0};
            CHECK(decode_x86(observed, sizeof(observed)).supported);
            Machine alias{0xC5, 0xF3, 0x7C, 0xC9, 0xC3};
            const float a[4] = {1, 2, 10, 20};
            const float alias_expected[4] = {3, 30, 3, 30};
            std::memcpy(alias.cpu.state.xmm[1], a, 16);
            alias.cpu.state.ymm_hi[1][0] = ~0ULL;
            CHECK(alias.run().stop == CpuStop::trap);
            CHECK(std::memcmp(alias.cpu.state.xmm[1], alias_expected, 16) == 0);
            CHECK(alias.cpu.state.ymm_hi[1][0] == 0 && alias.cpu.state.ymm_hi[1][1] == 0);

            // 128-bit PS, destination aliases second source.
            Machine ps{0xC5, 0xF3, 0x7D, 0xC2, 0xC3};
            const float src1[4] = {20, 3, 40, 7}, src2[4] = {100, 11, 200, 13};
            const float horizontal_ps_expected[4] = {17, 33, 89, 187};
            std::memcpy(ps.cpu.state.xmm[1], src1, 16);
            std::memcpy(ps.cpu.state.xmm[2], src2, 16);
            CHECK(ps.run().stop == CpuStop::trap);
            CHECK(std::memcmp(ps.cpu.state.xmm[0], horizontal_ps_expected, 16) == 0);

            // 256-bit PS with memory operand: independently grouped lanes.
            Machine horizontal_ps_mem{0xC5, 0xF7, 0x7C, 0x03, 0xC3};
            horizontal_ps_mem.cpu.state.gpr[3] = DATA;
            const float mem[8] = {100, 1, 200, 2, 300, 3, 400, 4};
            const float src[8] = {1, 2, 10, 20, 3, 4, 30, 40};
            const float expected[8] = {3, 30, 101, 202, 7, 70, 303, 404};
            CHECK(horizontal_ps_mem.mem.store(DATA, mem, sizeof(mem)) == MemoryError::ok);
            std::memcpy(horizontal_ps_mem.cpu.state.xmm[1], src, 16);
            std::memcpy(horizontal_ps_mem.cpu.state.ymm_hi[1], src + 4, 16);
            CHECK(horizontal_ps_mem.run().stop == CpuStop::trap);
            CHECK(std::memcmp(horizontal_ps_mem.cpu.state.xmm[0], expected, 16) == 0);
            CHECK(std::memcmp(horizontal_ps_mem.cpu.state.ymm_hi[0], expected + 4, 16) == 0);

            // 66 prefix selects PD; 128-bit register and 256-bit memory subtraction.
            Machine pd{0xC5, 0xF1, 0x7C, 0xC2, 0xC3};
            const double pd1[2] = {1, 3}, pd2[2] = {10, 20};
            const double pd_expected[2] = {4, 30};
            std::memcpy(pd.cpu.state.xmm[1], pd1, 16);
            std::memcpy(pd.cpu.state.xmm[2], pd2, 16);
            pd.cpu.state.ymm_hi[0][0] = ~0ULL;
            CHECK(pd.run().stop == CpuStop::trap);
            CHECK(std::memcmp(pd.cpu.state.xmm[0], pd_expected, 16) == 0);
            CHECK(pd.cpu.state.ymm_hi[0][0] == 0 && pd.cpu.state.ymm_hi[0][1] == 0);
            Machine pd_mem{0xC5, 0xF5, 0x7D, 0x03, 0xC3};
            pd_mem.cpu.state.gpr[3] = DATA;
            const double pdsrc[4] = {20, 3, 40, 7};
            const double pdmem[4] = {100, 11, 200, 13};
            const double pdout[4] = {17, 89, 33, 187};
            CHECK(pd_mem.mem.store(DATA, pdmem, sizeof(pdmem)) == MemoryError::ok);
            std::memcpy(pd_mem.cpu.state.xmm[1], pdsrc, 16);
            std::memcpy(pd_mem.cpu.state.ymm_hi[1], pdsrc + 2, 16);
            CHECK(pd_mem.run().stop == CpuStop::trap);
            CHECK(std::memcmp(pd_mem.cpu.state.xmm[0], pdout, 16) == 0);
            CHECK(std::memcmp(pd_mem.cpu.state.ymm_hi[0], pdout + 2, 16) == 0);

            // No integer compare or PHADD exists in the 0F 7C..7F map.
            const std::uint8_t invalid[][4] = {
                {0xC5, 0xF2, 0x7C, 0xC0}, // F3 7C
                {0xC5, 0xF3, 0x7F, 0xC0}, // F2 7F is not an integer compare
                {0xC5, 0xF3, 0x7D, 0xC0}  // valid VHSUBPS, checked below
            };
            CHECK(!decode_x86(invalid[0], 4).supported);
            CHECK(!decode_x86(invalid[1], 4).supported);
            CHECK(decode_x86(invalid[2], 4).supported);

            // VPSHUFD (0x70): vpshufd xmm0, xmm1, 0x1B (reverse dwords)
            Machine m_vpshufd{0xC5, 0xF9, 0x70, 0xC1, 0x1B, 0xC3};
            m_vpshufd.cpu.state.xmm[1][0] = 0x0000000200000001ULL;
            m_vpshufd.cpu.state.xmm[1][1] = 0x0000000400000003ULL;
            m_vpshufd.cpu.state.ymm_hi[0][0] = ~0ULL;
            CHECK(m_vpshufd.run().stop == CpuStop::trap);
            CHECK(m_vpshufd.cpu.state.xmm[0][0] == 0x0000000300000004ULL);
            CHECK(m_vpshufd.cpu.state.xmm[0][1] == 0x0000000100000002ULL);
            CHECK(m_vpshufd.cpu.state.ymm_hi[0][0] == 0ULL);
            Machine shuf_alias{0xC5, 0xFD, 0x70, 0xC0, 0x1B, 0xC3}; // ymm0 aliases source
            const std::uint32_t words[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            const std::uint32_t reversed[8] = {4, 3, 2, 1, 8, 7, 6, 5};
            std::memcpy(shuf_alias.cpu.state.xmm[0], words, 16);
            std::memcpy(shuf_alias.cpu.state.ymm_hi[0], words + 4, 16);
            CHECK(shuf_alias.run().stop == CpuStop::trap);
            CHECK(std::memcmp(shuf_alias.cpu.state.xmm[0], reversed, 16) == 0);
            CHECK(std::memcmp(shuf_alias.cpu.state.ymm_hi[0], reversed + 4, 16) == 0);
            Machine shuf_mem{0xC5, 0xF9, 0x70, 0x03, 0x1B, 0xC3}; // reads exactly m128
            shuf_mem.cpu.state.gpr[3] = DATA + 0xFF0;
            CHECK(shuf_mem.mem.store(DATA + 0xFF0, words, 16) == MemoryError::ok);
            CHECK(shuf_mem.run().stop == CpuStop::trap);
            CHECK(std::memcmp(shuf_mem.cpu.state.xmm[0], reversed, 16) == 0);
            Machine shuf_reserved{0xC5, 0xF1, 0x70, 0xC0, 0x1B};
            CHECK(shuf_reserved.run().stop == CpuStop::unsupported_opcode);

            // VSHUFPS (0xC6, pp=0): exact Bloodborne instruction:
            // c5 d0 c6 ec f0 = vshufps xmm5, xmm5, xmm4, 0xf0
            Machine m_vshufps{0xC5, 0xD0, 0xC6, 0xEC, 0xF0, 0xC3};
            m_vshufps.cpu.state.xmm[5][0] = 0x0000002000000010ULL;
            m_vshufps.cpu.state.xmm[5][1] = 0x0000004000000030ULL;
            m_vshufps.cpu.state.xmm[4][0] = 0x0000006000000050ULL;
            m_vshufps.cpu.state.xmm[4][1] = 0x0000008000000070ULL;
            m_vshufps.cpu.state.ymm_hi[5][0] = ~0ULL;
            CHECK(m_vshufps.run().stop == CpuStop::trap);
            CHECK(m_vshufps.cpu.state.xmm[5][0] == 0x0000001000000010ULL); // dword 0 from src1, dword 0 from src1
            CHECK(m_vshufps.cpu.state.xmm[5][1] == 0x0000008000000080ULL); // dword 3 from src2, dword 3 from src2
            CHECK(m_vshufps.cpu.state.ymm_hi[5][0] == 0ULL);

            // VSHUFPD (0xC6, pp=1): vshufpd xmm0, xmm1, xmm2, 0x01
            Machine m_vshufpd{0xC5, 0xF1, 0xC6, 0xC2, 0x01, 0xC3};
            m_vshufpd.cpu.state.xmm[1][0] = 100ULL;
            m_vshufpd.cpu.state.xmm[1][1] = 200ULL;
            m_vshufpd.cpu.state.xmm[2][0] = 300ULL;
            m_vshufpd.cpu.state.xmm[2][1] = 400ULL;
            CHECK(m_vshufpd.run().stop == CpuStop::trap);
            CHECK(m_vshufpd.cpu.state.xmm[0][0] == 200ULL); // bit 0 = 1 -> xmm1[1]
            CHECK(m_vshufpd.cpu.state.xmm[0][1] == 300ULL); // bit 1 = 0 -> xmm2[0]

            // 256-bit VSHUFPS: each 128-bit lane selects independently; rm is memory.
            Machine m_ps256{0xC5, 0xF4, 0xC6, 0x03, 0x4E, 0xC3};
            m_ps256.cpu.state.gpr[3] = DATA;
            const std::uint32_t ps_mem[8] = {10, 11, 12, 13, 20, 21, 22, 23};
            const std::uint32_t ps_src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            CHECK(m_ps256.mem.store(DATA, ps_mem, sizeof(ps_mem)) == MemoryError::ok);
            std::memcpy(m_ps256.cpu.state.xmm[1], ps_src, 16);
            std::memcpy(m_ps256.cpu.state.ymm_hi[1], ps_src + 4, 16);
            CHECK(m_ps256.run().stop == CpuStop::trap);
            const std::uint32_t ps_expected[8] = {3, 4, 10, 11, 7, 8, 20, 21};
            CHECK(std::memcmp(m_ps256.cpu.state.xmm[0], ps_expected, 16) == 0);
            CHECK(std::memcmp(m_ps256.cpu.state.ymm_hi[0], ps_expected + 4, 16) == 0);

            // 256-bit VSHUFPD: immediate bits 2/3 select the upper lane.
            Machine m_pd256{0xC5, 0xF5, 0xC6, 0xC2, 0x06, 0xC3};
            m_pd256.cpu.state.xmm[1][0] = 1; m_pd256.cpu.state.xmm[1][1] = 2;
            m_pd256.cpu.state.ymm_hi[1][0] = 3; m_pd256.cpu.state.ymm_hi[1][1] = 4;
            m_pd256.cpu.state.xmm[2][0] = 10; m_pd256.cpu.state.xmm[2][1] = 20;
            m_pd256.cpu.state.ymm_hi[2][0] = 30; m_pd256.cpu.state.ymm_hi[2][1] = 40;
            CHECK(m_pd256.run().stop == CpuStop::trap);
            CHECK(m_pd256.cpu.state.xmm[0][0] == 1 && m_pd256.cpu.state.xmm[0][1] == 20);
            CHECK(m_pd256.cpu.state.ymm_hi[0][0] == 4 && m_pd256.cpu.state.ymm_hi[0][1] == 30);
        }
        {   // BT / BTS / BTR / BTC with memory operand (0x0F A3, AB, B3, BB)
            // bt [rbx], rcx (64-bit with bit offset crossing qwords)
            Machine m_bt{0x48, 0x0F, 0xA3, 0x0B, 0xC3}; // bt [rbx], rcx; ret
            m_bt.cpu.state.gpr[3] = DATA + 16;
            const std::uint64_t words[4] = {0x1ULL, 0x2ULL, 0x4ULL, 0x8ULL};
            CHECK(m_bt.mem.store(DATA, words, sizeof(words)) == MemoryError::ok);

            // rcx = 65 -> tests bit 1 of qword at [rbx+8] (DATA+24 -> words[3] = 0x8 = bit 3 -> 0)
            m_bt.cpu.state.gpr[1] = 65;
            CHECK(m_bt.run().stop == CpuStop::trap);
            CHECK((m_bt.cpu.state.rflags & FLAG_CF) == 0);

            // rcx = 67 -> tests bit 3 of qword at [rbx+8] (words[3] bit 3 == 1)
            Machine m_bt2{0x48, 0x0F, 0xA3, 0x0B, 0xC3};
            CHECK(m_bt2.mem.store(DATA, words, sizeof(words)) == MemoryError::ok);
            m_bt2.cpu.state.gpr[3] = DATA + 16;
            m_bt2.cpu.state.gpr[1] = 67;
            CHECK(m_bt2.run().stop == CpuStop::trap);
            CHECK((m_bt2.cpu.state.rflags & FLAG_CF) != 0);

            // negative offset: rcx = -63 -> tests bit 1 of [rbx - 8] (DATA+8 -> words[1] = 2 = bit 1)
            Machine m_bt_neg{0x48, 0x0F, 0xA3, 0x0B, 0xC3};
            CHECK(m_bt_neg.mem.store(DATA, words, sizeof(words)) == MemoryError::ok);
            m_bt_neg.cpu.state.gpr[3] = DATA + 16;
            m_bt_neg.cpu.state.gpr[1] = static_cast<std::uint64_t>(-63LL); // bit 1 of [DATA+8] -> 1
            CHECK(m_bt_neg.run().stop == CpuStop::trap);
            CHECK((m_bt_neg.cpu.state.rflags & FLAG_CF) != 0);

            // bts [rbx], rcx: sets bit
            Machine m_bts{0x48, 0x0F, 0xAB, 0x0B, 0xC3};
            m_bts.cpu.state.gpr[3] = DATA;
            m_bts.cpu.state.gpr[1] = 10; // bit 2 of byte 1
            std::uint8_t zero_byte = 0;
            CHECK(m_bts.mem.store(DATA + 1, &zero_byte, 1) == MemoryError::ok);
            CHECK(m_bts.run().stop == CpuStop::trap);
            CHECK((m_bts.cpu.state.rflags & FLAG_CF) == 0);
            std::uint8_t modified_byte = 0;
            CHECK(m_bts.mem.load(DATA + 1, &modified_byte, 1) == MemoryError::ok);
            CHECK(modified_byte == (1u << 2));

            // btr [rbx], rcx: clears bit
            Machine m_btr{0x48, 0x0F, 0xB3, 0x0B, 0xC3};
            m_btr.cpu.state.gpr[3] = DATA;
            m_btr.cpu.state.gpr[1] = 10;
            std::uint8_t bit_set = (1u << 2);
            CHECK(m_btr.mem.store(DATA + 1, &bit_set, 1) == MemoryError::ok);
            CHECK(m_btr.run().stop == CpuStop::trap);
            CHECK((m_btr.cpu.state.rflags & FLAG_CF) != 0);
            CHECK(m_btr.mem.load(DATA + 1, &modified_byte, 1) == MemoryError::ok);
            CHECK(modified_byte == 0);

            // btc [rbx], rcx: complements bit
            Machine m_btc{0x48, 0x0F, 0xBB, 0x0B, 0xC3};
            m_btc.cpu.state.gpr[3] = DATA;
            m_btc.cpu.state.gpr[1] = 10;
            CHECK(m_btc.run().stop == CpuStop::trap);
            CHECK((m_btc.cpu.state.rflags & FLAG_CF) == 0);
            CHECK(m_btc.mem.load(DATA + 1, &modified_byte, 1) == MemoryError::ok);
            CHECK(modified_byte == (1u << 2));

            const std::uint8_t d_bt_mem[] = {0x48, 0x0F, 0xA3, 0x0B};
            CHECK(decode_x86(d_bt_mem, sizeof(d_bt_mem)).length == 4);
            CHECK(decode_x86(d_bt_mem, sizeof(d_bt_mem)).supported);
        }
        {   // ud2
            Machine m{0x0F, 0x0B};
            CHECK(m.run().stop == CpuStop::undefined);
        }
    });
}
