// Diagnostico local: recebe dumps plaintext do usuario; nunca persiste bytes do jogo.
#include "../core/ps4_bootstrap.h"
#include "../core/orbis_runtime.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file || file.tellg()<0 || file.tellg()>(512ll<<20)) throw std::runtime_error("arquivo ausente ou excessivo: "+path);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()));
    if (!file) throw std::runtime_error("arquivo truncado: "+path);
    return data;
}
}
int main(int argc,char** argv) {
    if (argc<2) { std::fprintf(stderr,"uso: bb-run-eboot Image0 [max_passos] [--libc] [--hle]\n"); return 2; }
    try {
        std::uint64_t max_steps=50000000;
        bool linked=false, hle=false, budget_set=false;
        for (int a=2;a<argc;++a) {
            const std::string arg=argv[a];
            if (arg=="--libc") linked=true;
            else if (arg=="--hle") hle=true;
            else if (!budget_set && !arg.empty() && arg.find_first_not_of("0123456789")==std::string::npos) {
                max_steps=std::strtoull(argv[a],nullptr,10);budget_set=true;
            } else throw std::runtime_error("opcao desconhecida: "+arg);
        }
        bb::GuestMemory memory(768u<<20);
        auto eboot=read_file(std::string(argv[1])+"/eboot.bin");
        auto libc=linked?read_file(std::string(argv[1])+"/sce_module/libc.prx"):std::vector<std::uint8_t>{};
        auto image=bb::Ps4Loader::load(memory,eboot,libc);
        bb::X86Interpreter cpu(memory);
        cpu.add_trap_range(0x7f000000,32*(image.imports.size()+1));
        cpu.add_trap_range(0x7e000000,4096*(image.imports.size()+1));
        cpu.add_trap_range(bb::Ps4Bootstrap::exit_trap,64);
        auto tcb=bb::Ps4Bootstrap::prepare(memory,cpu.state,image);
        std::printf("entrada=0x%llx, imports=%zu, modulos=%zu, tcb=0x%llx\n",
                    static_cast<unsigned long long>(image.entry),image.imports.size(),image.modules.size(),
                    static_cast<unsigned long long>(tcb));
        bb::OrbisRuntime runtime(memory);
        std::uint64_t steps=0;
        bb::CpuRunResult result;
        auto execute=[&]() {
            for (;;) {
                result=cpu.run(max_steps-steps);
                steps+=result.steps;
                if (!hle || result.stop!=bb::CpuStop::trap || result.rip<0x7f000000 ||
                    result.rip>=0x7f000000+32*image.imports.size() || steps>=max_steps) break;
                auto index=(result.rip-0x7f000000)/32;
                const auto& imp=image.imports[static_cast<std::size_t>(index)];
                if (imp.trap!=result.rip) break;
                std::uint64_t value=0;
                try { value=runtime.invoke(imp.name,cpu.state); }
                catch (const std::exception& e) { std::printf("HLE: %s\n",e.what()); break; }
                std::uint64_t ret=0;
                if (memory.load(cpu.state.gpr[4],&ret,8)!=bb::MemoryError::ok) break;
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
                std::printf("modulo %zu nao inicializado: %s\n",i,result.describe().c_str());
                return 1;
            }
            std::printf("modulo %zu inicializado apos %llu passos\n",i,static_cast<unsigned long long>(steps));
        }
        bb::Ps4Bootstrap::game_entry(memory,cpu.state,image);
        execute();
        std::printf("parou: %s\n",result.describe().c_str());
        for (const auto& imp:image.imports) if (result.rip==imp.trap)
            std::printf("import chamado: %s\n",imp.name.c_str());
        if (result.rip>=0x10000000 && result.rip<0x70000000)
            std::printf("rip(imagem+)=0x%llx\n",static_cast<unsigned long long>(result.rip-0x10000000));
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"erro: %s\n",e.what()); return 2; }
}
