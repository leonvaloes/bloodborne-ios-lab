// Diagnóstico local: recebe dump plaintext do usuário; não distribui nem altera arquivos do jogo.
#include "../core/ps4_loader.h"
#include "../core/x86_interp.h"
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
    if (argc<2) { std::fprintf(stderr,"uso: bb-run-eboot Image0 [max_passos] [--libc]\n"); return 2; }
    try {
        const std::uint64_t max_steps=argc>2?std::strtoull(argv[2],nullptr,10):50000000;
        const bool linked=argc>3 && std::string(argv[3])=="--libc";
        bb::GuestMemory memory(768u<<20);
        auto eboot=read_file(std::string(argv[1])+"/eboot.bin");
        auto libc=linked?read_file(std::string(argv[1])+"/sce_module/libc.prx"):std::vector<std::uint8_t>{};
        auto image=bb::Ps4Loader::load(memory,eboot,libc);
        constexpr std::uint64_t stack=0x70000000, param=0x6f000000, exit_trap=0x7d000000;
        if (memory.map(stack,8u<<20,bb::Permission::read|bb::Permission::write)!=bb::MemoryError::ok ||
            memory.map(param,4096,bb::Permission::read|bb::Permission::write)!=bb::MemoryError::ok)
            throw std::runtime_error("falha ao mapear stack/param");
        const char path[]="/app0/eboot.bin";
        std::uint64_t one=1, argv0=param+64, zero=0;
        memory.store(param,&one,8);memory.store(param+8,&argv0,8);memory.store(param+16,&zero,8);
        memory.store(argv0,path,sizeof(path));
        bb::X86Interpreter cpu(memory);
        cpu.add_trap_range(0x7f000000,32*(image.imports.size()+1));
        cpu.add_trap_range(0x7e000000,4096*(image.imports.size()+1));
        cpu.add_trap_range(exit_trap,32);
        cpu.state.rip=image.entry;cpu.state.gpr[7]=param;cpu.state.gpr[6]=exit_trap;
        cpu.state.gpr[4]=stack+(8u<<20)-72;
        memory.store(cpu.state.gpr[4],&exit_trap,8);
        std::printf("entrada=0x%llx, imports=%zu, modulos=%zu (init nao executado)\n",
                    static_cast<unsigned long long>(image.entry),image.imports.size(),image.modules.size());
        auto result=cpu.run(max_steps);
        std::printf("parou: %s\n",result.describe().c_str());
        for (const auto& imp:image.imports) if (result.rip==imp.trap)
            std::printf("import chamado: %s\n",imp.name.c_str());
        if (result.rip>=0x10000000 && result.rip<0x70000000)
            std::printf("rip(imagem+)=0x%llx\n",static_cast<unsigned long long>(result.rip-0x10000000));
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"erro: %s\n",e.what()); return 2; }
}
