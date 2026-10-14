// bb-run-boot: executa o ponto de entrada de uma imagem PS4 preparada (boot.bin BBPROBE2)
// no interpretador x86-64, ate o primeiro import, opcode nao suportado ou falha.
// Uso local apenas: o boot.bin deriva de dados comerciais e nunca vai ao repositorio.
#include "../core/guest_memory.h"
#include "../core/x86_interp.h"
#include "../core/orbis_runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace bb;

namespace {
constexpr std::uint64_t IMAGE_BASE = 0x10000000ULL, STACK_BASE = 0x70000000ULL, STACK_SIZE = 8u << 20,
                        PARAM_BASE = 0x6F000000ULL, TLS_BASE = 0x6E000000ULL, TLS_SIZE = 16u << 20,
                        CODE_TRAPS = 0x7F000000ULL, DATA_TRAPS = 0x7E000000ULL,
                        EXIT_TRAP = 0x7D000000ULL, PAGE = 4096;

struct Reader {
    std::ifstream f;
    std::uint64_t u64() { std::uint64_t v = 0; f.read(reinterpret_cast<char*>(&v), 8); if (!f) { std::fprintf(stderr, "boot.bin truncado\n"); std::exit(2); } return v; }
};
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "uso: bb-run-boot boot.bin [max_passos]\n"); return 2; }
    const std::uint64_t max_steps = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 50000000ULL;
    Reader r;
    r.f.open(argv[1], std::ios::binary);
    if (!r.f) { std::fprintf(stderr, "nao foi possivel abrir %s\n", argv[1]); return 2; }
    char magic[8];
    r.f.read(magic, 8);
    if (!r.f || std::memcmp(magic, "BBPROBE2", 8) != 0) { std::fprintf(stderr, "somente BBPROBE2 e suportado\n"); return 2; }
    const std::uint64_t size = r.u64(), entry = r.u64(), ns = r.u64(), nr = r.u64(), ni = r.u64();
    r.u64();  // capabilities
    if (!size || size > (512u << 20) || entry >= size || !ns || ns > 64 || nr > 1000000 || ni > 100000) {
        std::fprintf(stderr, "limites do boot.bin excedidos\n"); return 2;
    }
    for (std::uint64_t i = 0; i < ns; ++i) { r.u64(); r.u64(); r.u64(); }  // segmentos: a imagem ja vem expandida
    std::vector<std::string> names(ni);
    for (auto& n : names) {
        char b[128];
        r.f.read(b, 128);
        n.assign(b, strnlen(b, 128));
    }
    struct Reloc { std::uint64_t target, kind, value; std::int64_t addend; };
    std::vector<Reloc> relocs(nr);
    for (auto& x : relocs) { x.target = r.u64(); x.kind = r.u64(); x.value = r.u64(); x.addend = static_cast<std::int64_t>(r.u64()); }

    GuestMemory mem(static_cast<std::size_t>(3ULL << 30)); // teto lógico total, com alocação física sob demanda
    std::vector<std::uint8_t> image(size);
    r.f.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(size));
    if (!r.f) { std::fprintf(stderr, "imagem truncada\n"); return 2; }
    for (const auto& x : relocs) {
        std::uint64_t v = x.kind == 2 ? DATA_TRAPS + PAGE * x.value + static_cast<std::uint64_t>(x.addend)
                        : x.kind == 1 ? CODE_TRAPS + 32 * x.value
                        : IMAGE_BASE + x.value;
        if (x.kind == 0) v += static_cast<std::uint64_t>(x.addend);
        if (x.target + 8 > size) { std::fprintf(stderr, "relocacao fora da imagem\n"); return 2; }
        std::memcpy(image.data() + x.target, &v, 8);
    }
    if (mem.map(IMAGE_BASE, size, Permission::read | Permission::write) != MemoryError::ok ||
        mem.store(IMAGE_BASE, image.data(), size) != MemoryError::ok ||
        mem.map(STACK_BASE, STACK_SIZE, Permission::read | Permission::write) != MemoryError::ok ||
        mem.map(PARAM_BASE, PAGE, Permission::read | Permission::write) != MemoryError::ok ||
        mem.map(TLS_BASE, TLS_SIZE, Permission::read | Permission::write) != MemoryError::ok ||
        (ni && mem.map(DATA_TRAPS, PAGE * ni, Permission::read | Permission::write) != MemoryError::ok)) {
        std::fprintf(stderr, "falha ao mapear a memoria do guest\n"); return 2;
    }
    const char path[] = "/app0/eboot.bin";
    const std::uint64_t argc_v = 1, argv0 = PARAM_BASE + 64, zero = 0;
    mem.store(PARAM_BASE, &argc_v, 8);
    mem.store(PARAM_BASE + 8, &argv0, 8);
    mem.store(PARAM_BASE + 16, &zero, 8);
    mem.store(argv0, path, sizeof(path));

    const std::uint64_t tcb = TLS_BASE + 0x10000;
    const std::uint64_t dtv = tcb + 64;
    const std::uint64_t thread_rec = tcb + 256;
    const std::uint64_t dtv_gen = 1, dtv_tls0 = TLS_BASE;
    mem.store(tcb, &tcb, 8);
    mem.store(tcb + 8, &dtv, 8);
    mem.store(tcb + 16, &thread_rec, 8);
    mem.store(dtv, &dtv_gen, 8);
    mem.store(dtv + 8, &dtv_tls0, 8);

    X86Interpreter cpu(mem);
    cpu.add_trap_range(CODE_TRAPS, 32 * (ni + 1));
    cpu.add_trap_range(DATA_TRAPS, PAGE * (ni + 1));
    cpu.add_trap_range(EXIT_TRAP, 32);
    cpu.state.rip = IMAGE_BASE + entry;
    cpu.state.fs_base = tcb;
    cpu.state.gpr[7] = PARAM_BASE;  // rdi = params
    cpu.state.gpr[6] = EXIT_TRAP;   // rsi = exit
    cpu.state.gpr[4] = STACK_BASE + STACK_SIZE - 64;
    std::uint64_t ret = EXIT_TRAP;
    mem.store(cpu.state.gpr[4] - 8, &ret, 8);
    cpu.state.gpr[4] -= 8;

    std::printf("imagem=0x%llx bytes, entrada=0x%llx, imports=%llu, relocacoes=%llu\n",
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(entry),
                static_cast<unsigned long long>(ni), static_cast<unsigned long long>(nr));
    if (argc > 3 && std::strcmp(argv[3], "--hle") != 0) {
        std::fprintf(stderr, "opcao desconhecida: use --hle\n"); return 2;
    }
    const bool hle = argc > 3;
    OrbisRuntime runtime(mem);
    std::uint64_t total_steps = 0, import_calls = 0;
    std::vector<std::uint64_t> call_counts(ni, 0);
    CpuRunResult res;
    for (;;) {
        res = cpu.run(max_steps - total_steps);
        total_steps += res.steps;
        const bool code_trap = res.stop == CpuStop::trap && res.rip >= CODE_TRAPS && res.rip < CODE_TRAPS + 32 * ni;
        if (!(hle && code_trap) || total_steps >= max_steps) break;
        const std::uint64_t idx = (res.rip - CODE_TRAPS) / 32;
        if (call_counts[idx]++ == 0)
            std::printf("[import #%llu apos %llu passos] %s -> %s\n", static_cast<unsigned long long>(import_calls),
                        static_cast<unsigned long long>(total_steps), names[idx].c_str(), runtime.resolve(names[idx]).c_str());
        ++import_calls;
        std::uint64_t value = 0;
        try { value = runtime.invoke(names[idx], cpu.state); }
        catch (const std::exception& e) { std::printf("HLE: %s\n", e.what()); break; }
        std::uint64_t back = 0;
        if (mem.load(cpu.state.gpr[4], &back, 8) != MemoryError::ok) break;
        cpu.state.gpr[4] += 8;
        cpu.state.gpr[0] = value;
        cpu.state.rip = back;
    }
    std::printf("parou: %s (passos totais=%llu, chamadas a imports=%llu)\n", res.describe().c_str(),
                static_cast<unsigned long long>(total_steps), static_cast<unsigned long long>(import_calls));
    if (res.stop == CpuStop::trap) {
        if (res.rip >= EXIT_TRAP && res.rip < EXIT_TRAP + 32) std::printf("o guest retornou ao stub de saida\n");
        else if (res.rip >= CODE_TRAPS && res.rip < CODE_TRAPS + 32 * ni)
            std::printf("import chamado: %s\n", names[(res.rip - CODE_TRAPS) / 32].c_str());
        else if (res.rip >= DATA_TRAPS && res.rip < DATA_TRAPS + PAGE * ni)
            std::printf("salto para dado importado: %s\n", names[(res.rip - DATA_TRAPS) / PAGE].c_str());
    }
    if (res.rip >= IMAGE_BASE && res.rip - IMAGE_BASE < size)
        std::printf("rip(imagem+)=0x%llx\n", static_cast<unsigned long long>(res.rip - IMAGE_BASE));
    for (int i = 0; i < 16; ++i)
        std::printf("r%02d=%016llx%s", i, static_cast<unsigned long long>(cpu.state.gpr[i]), (i & 3) == 3 ? "\n" : " ");
    return 0;
}
