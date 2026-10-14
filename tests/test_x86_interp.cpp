#include "check.h"
#include "../core/x86_interp.h"
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
        {   // ud2
            Machine m{0x0F, 0x0B};
            CHECK(m.run().stop == CpuStop::undefined);
        }
    });
}
