#include "ps4_bootstrap.h"
#include <algorithm>
#include <stdexcept>

namespace bb {
namespace {
void check(MemoryError error) { if (error != MemoryError::ok) throw std::runtime_error("falha ao preparar memoria de bootstrap"); }
void word(GuestMemory& mem, std::uint64_t at, std::uint64_t value) { check(mem.store(at, &value, 8)); }
std::uint64_t aligned(std::uint64_t size, std::uint64_t align) {
    if (!align) align = 16;
    if (align > 4096 || (align & (align-1)) || size > UINT64_MAX-align)
        throw std::runtime_error("alinhamento TLS invalido");
    return (size+align-1)&~(align-1);
}
void copy_tls(GuestMemory& mem, const Ps4Tls& tls, std::uint64_t dest) {
    if (tls.filesz > tls.memsz || tls.memsz > (1u<<20)) throw std::runtime_error("template TLS invalido");
    if (tls.filesz) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(tls.filesz));
        check(mem.load(tls.address, bytes.data(), bytes.size()));
        check(mem.store(dest, bytes.data(), bytes.size()));
    }
}
void return_to(GuestMemory& mem, CpuState& cpu, std::uint64_t target) {
    cpu.gpr[4] = Ps4Bootstrap::stack_base + Ps4Bootstrap::stack_size - 72;
    word(mem, cpu.gpr[4], target);
}
} // namespace

std::uint64_t Ps4Bootstrap::prepare(GuestMemory& mem, CpuState& cpu, const Ps4Image& image) {
    check(mem.map(stack_base, stack_size, Permission::read|Permission::write));
    check(mem.map(param_base, 4096, Permission::read|Permission::write));
    // Variant II: static TLS lives below fs_base, TCB self/dtv/thread at +0/+8/+16.
    // Keep module blocks in their own address space, indexed by the ELF TLS module ID.
    constexpr std::uint64_t region_size = 16u << 20;
    check(mem.map(tls_base, region_size, Permission::read|Permission::write));
    std::uint64_t main_size = aligned(image.tls.memsz, image.tls.align);
    if (main_size > (1u<<20)) throw std::runtime_error("TLS principal excessivo");
    const std::uint64_t tcb = tls_base + main_size;
    if (tcb + 256 >= tls_base + region_size) throw std::runtime_error("TCB fora da regiao TLS");
    copy_tls(mem, image.tls, tls_base);
    word(mem, tcb, tcb);
    const std::uint64_t dtv = tcb + 64;
    word(mem, tcb+8, dtv);
    word(mem, tcb+16, tcb+256); // registro guest da thread principal
    word(mem, dtv, image.modules.size()+1);
    word(mem, dtv+8, tls_base);
    std::uint64_t next = (tcb + 512 + 63)&~std::uint64_t(63);
    for (const auto& module : image.modules) {
        const auto& tls = module.tls;
        if (!tls.module_id) continue;
        if (tls.module_id > 64 || dtv+8*tls.module_id >= tcb+256)
            throw std::runtime_error("indice TLS de modulo excessivo");
        next = aligned(next, tls.align ? tls.align : 16);
        auto size = aligned(tls.memsz, tls.align);
        if (next > tls_base + region_size || size > tls_base + region_size-next)
            throw std::runtime_error("TLS de modulos excessivo");
        copy_tls(mem, tls, next);
        word(mem, dtv+8*tls.module_id, next);
        next += size;
    }
    const char path[] = "/app0/eboot.bin";
    word(mem, param_base, 1);
    word(mem, param_base+8, param_base+64);
    word(mem, param_base+16, 0);
    check(mem.store(param_base+64, path, sizeof(path)));
    cpu.fs_base = tcb;
    game_entry(mem, cpu, image);
    return tcb;
}
void Ps4Bootstrap::module_call(GuestMemory& mem, CpuState& cpu, const Ps4Module& module) {
    if (!module.init) throw std::runtime_error("modulo sem inicializador");
    return_to(mem, cpu, exit_trap+32);
    cpu.rip = module.init;
    cpu.gpr[7] = cpu.gpr[6] = cpu.gpr[2] = 0;
}
void Ps4Bootstrap::game_entry(GuestMemory& mem, CpuState& cpu, const Ps4Image& image) {
    return_to(mem, cpu, exit_trap);
    cpu.rip = image.entry;
    cpu.gpr[7] = param_base;
    cpu.gpr[6] = exit_trap;
}
std::uint64_t Ps4Bootstrap::register_dynamic_tls(GuestMemory& mem, const Ps4Image& image, const Ps4Tls& tls) {
    if (!tls.module_id) return 0;
    std::uint64_t main_size = aligned(image.tls.memsz, image.tls.align);
    if (main_size > (1u<<20)) throw std::runtime_error("TLS principal excessivo");
    const std::uint64_t tcb = tls_base + main_size;
    constexpr std::uint64_t region_size = 16u << 20;

    std::uint64_t check_tcb = 0;
    if (mem.load(tcb, &check_tcb, 8) != MemoryError::ok || check_tcb != tcb) {
        throw std::runtime_error("TCB nao inicializado no bootstrap");
    }

    const std::uint64_t dtv = tcb + 64;
    if (tls.module_id > 64 || dtv + 8 * tls.module_id >= tcb + 256) {
        throw std::runtime_error("indice TLS de modulo excessivo");
    }

    if (tls.memsz == 0) {
        word(mem, dtv + 8 * tls.module_id, 0);
        return 0;
    }

    std::uint64_t next = (tcb + 512 + 63) & ~std::uint64_t(63);
    for (const auto& module : image.modules) {
        const auto& m_tls = module.tls;
        if (!m_tls.module_id || !m_tls.memsz || m_tls.module_id == tls.module_id) continue;
        next = aligned(next, m_tls.align ? m_tls.align : 16);
        next += aligned(m_tls.memsz, m_tls.align);
    }

    next = aligned(next, tls.align ? tls.align : 16);
    auto size = aligned(tls.memsz, tls.align);
    if (next > tls_base + region_size || size > tls_base + region_size - next) {
        throw std::runtime_error("TLS de modulos excessivo");
    }

    copy_tls(mem, tls, next);

    std::uint64_t count = 0;
    if (mem.load(dtv, &count, 8) == MemoryError::ok) {
        if (tls.module_id >= count) {
            word(mem, dtv, tls.module_id + 1);
        }
    }

    word(mem, dtv + 8 * tls.module_id, next);
    return next;
}
} // namespace bb
