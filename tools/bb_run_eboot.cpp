// Diagnostico local: recebe dumps plaintext do usuario; nunca persiste bytes do jogo.
#include "../core/ps4_bootstrap.h"
#include "../core/orbis_runtime.h"
#if defined(_WIN32)
#include <windows.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file || file.tellg()<0 || file.tellg()>(512ll<<20)) throw std::runtime_error("arquivo ausente ou excessivo: "+path.string());
    std::vector<std::uint8_t> data(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()));
    if (!file) throw std::runtime_error("arquivo truncado: "+path.string());
    return data;
}
}
int main(int argc, char** argv) {
    std::vector<std::filesystem::path> args;
#if defined(_WIN32)
    int wargc = 0;
    if (auto* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        for (int i = 0; i < wargc; ++i) args.emplace_back(wargv[i]);
        LocalFree(wargv);
    }
#endif
    if (args.empty()) {
        for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    }
    if (args.size()<2) { std::fprintf(stderr,"uso: bb-run-eboot Image0 [max_passos] [--libc] [--hle]\n"); return 2; }
    try {
        std::uint64_t max_steps=50000000;
        bool linked=false, hle=false, budget_set=false;
        for (std::size_t a=2;a<args.size();++a) {
            const std::string arg = args[a].string();
            if (arg=="--libc") linked=true;
            else if (arg=="--hle") hle=true;
            else if (!budget_set && !arg.empty() && arg.find_first_not_of("0123456789")==std::string::npos) {
                max_steps=std::strtoull(arg.c_str(),nullptr,10);budget_set=true;
            } else throw std::runtime_error("opcao desconhecida: "+arg);
        }
        bb::GuestMemory memory(768u<<20);
        const std::filesystem::path root = args[1];
        auto eboot=read_file(root/"eboot.bin");
        std::map<std::string,std::vector<std::uint8_t>> modules;
        std::uint64_t total=eboot.size();
        const auto directory=root/"sce_module";
        if (std::filesystem::is_directory(directory)) for (const auto& item:std::filesystem::directory_iterator(directory)) {
            if (!item.is_regular_file() || item.path().extension()!=".prx") continue;
            auto bytes=read_file(item.path());
            if (bytes.size()>(512ull<<20)-total) throw std::runtime_error("total de arquivos excessivo");
            total+=bytes.size();
            modules.emplace(item.path().filename().string(),std::move(bytes));
        }
        if (linked && !modules.count("libc.prx")) throw std::runtime_error("libc.prx ausente");
        auto image=linked ? bb::Ps4Loader::load(memory,eboot,modules.at("libc.prx"))
                          : bb::Ps4Loader::load(memory,eboot,modules);
        bb::X86Interpreter cpu(memory);
        cpu.add_trap_range(0x7f000000,32*(image.imports.size()+1));
        cpu.add_trap_range(0x7e000000,4096*(image.imports.size()+1));
        cpu.add_trap_range(bb::Ps4Bootstrap::exit_trap,64);
        auto tcb=bb::Ps4Bootstrap::prepare(memory,cpu.state,image);
        std::printf("entrada=0x%llx, imports=%zu, modulos=%zu, tcb=0x%llx\n",
                    static_cast<unsigned long long>(image.entry),image.imports.size(),image.modules.size(),
                    static_cast<unsigned long long>(tcb));
        bb::OrbisRuntime runtime(memory);
        runtime.set_procparam(image.procparam);
        std::uint64_t steps=0;
        bb::CpuRunResult result;
        auto execute=[&]() {
            for (;;) {
                result=cpu.run(max_steps-steps);
                steps+=result.steps;
                if (!hle || result.stop!=bb::CpuStop::trap || result.rip<0x7f000000 ||
                    result.rip>=0x7f000000+32*image.imports.size() || steps>=max_steps) {
                    break;
                }
                auto index=(result.rip-0x7f000000)/32;
                const auto& imp=image.imports[static_cast<std::size_t>(index)];
                if (imp.trap!=result.rip) {
                    std::printf("trap rip 0x%llx != imp.trap 0x%llx\n", (unsigned long long)result.rip, (unsigned long long)imp.trap);
                    break;
                }
                std::uint64_t value=0;
                try { value=runtime.invoke(imp.name,cpu.state); }
                catch (const std::exception& e) { std::printf("HLE: %s (import: %s)\n",e.what(), imp.name.c_str()); break; }
                std::uint64_t ret=0;
                if (memory.load(cpu.state.gpr[4],&ret,8)!=bb::MemoryError::ok) {
                    std::printf("falha ao desempilhar retorno do trap\n");
                    break;
                }
                cpu.state.gpr[0]=value;
                cpu.state.gpr[4]+=8;
                cpu.state.rip=ret;
            }
        };
        for (std::size_t i=0;i<image.modules.size();++i) {
            if (!image.modules[i].init) continue;
            bb::Ps4Bootstrap::module_call(memory,cpu.state,image.modules[i]);
            execute();
            if (result.stop!=bb::CpuStop::trap || result.rip!=bb::Ps4Bootstrap::exit_trap+32 || cpu.state.gpr[0]!=0) {
                std::printf("modulo %zu (%s) nao inicializado: %s\n",i,image.modules[i].name.c_str(),result.describe().c_str());
                return 1;
            }
            std::printf("modulo %zu (%s) inicializado apos %llu passos\n",i,image.modules[i].name.c_str(),static_cast<unsigned long long>(steps));
        }
        bb::Ps4Bootstrap::game_entry(memory,cpu.state,image);
        execute();
        std::printf("parou: %s\n",result.describe().c_str());
        std::printf("rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx\n",
                    (unsigned long long)cpu.state.gpr[0], (unsigned long long)cpu.state.gpr[3],
                    (unsigned long long)cpu.state.gpr[1], (unsigned long long)cpu.state.gpr[2]);
        std::printf("rsi=0x%llx rdi=0x%llx rbp=0x%llx rsp=0x%llx\n",
                    (unsigned long long)cpu.state.gpr[6], (unsigned long long)cpu.state.gpr[7],
                    (unsigned long long)cpu.state.gpr[5], (unsigned long long)cpu.state.gpr[4]);
        std::printf("r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx\n",
                    (unsigned long long)cpu.state.gpr[8], (unsigned long long)cpu.state.gpr[9],
                    (unsigned long long)cpu.state.gpr[10], (unsigned long long)cpu.state.gpr[11]);
        std::printf("r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n",
                    (unsigned long long)cpu.state.gpr[12], (unsigned long long)cpu.state.gpr[13],
                    (unsigned long long)cpu.state.gpr[14], (unsigned long long)cpu.state.gpr[15]);
        std::printf("rflags=0x%llx\n", (unsigned long long)cpu.state.rflags);
        for (const auto& imp:image.imports) if (result.rip==imp.trap)
            std::printf("import chamado: %s (%s)\n",imp.name.c_str(), runtime.resolve(imp.name).c_str());
        if (result.rip>=0x10000000 && result.rip<0x70000000)
            std::printf("rip(imagem+)=0x%llx\n",static_cast<unsigned long long>(result.rip-0x10000000));
        // Disassemble 64 bytes around RIP
        std::uint8_t code_peek[64] = {};
        std::uint64_t peek_addr = result.rip >= 32 ? result.rip - 32 : result.rip;
        if (memory.load(peek_addr, code_peek, 64) == bb::MemoryError::ok) {
            std::printf("code at [rip-32 .. rip+32] (rip at offset +32):\n");
            for (int k = 0; k < 64; ++k) {
                if (k == 32) std::printf(" [RIP->]");
                std::printf("%02x ", code_peek[k]);
                if (k == 31 || k == 63) std::printf("\n");
            }
        }
        return result.stop==bb::CpuStop::trap && result.rip==bb::Ps4Bootstrap::exit_trap ? 0 : 1;
    } catch (const std::exception& e) { std::fprintf(stderr,"erro: %s\n",e.what()); return 2; }
}
