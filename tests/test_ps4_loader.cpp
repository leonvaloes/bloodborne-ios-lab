#include "check.h"
#include "../core/ps4_loader.h"
#include "../core/ps4_bootstrap.h"
#include <algorithm>
#include <cstring>
using namespace bb;
namespace {
using B=std::vector<std::uint8_t>;
void put(B& b,std::size_t p,std::uint64_t v,unsigned n=8) { for (unsigned i=0;i<n;++i) b.at(p+i)=static_cast<std::uint8_t>(v>>(8*i)); }
void ph(B& b,int i,std::uint32_t type,std::uint64_t off,std::uint64_t va,std::uint64_t fs,std::uint64_t ms) {
    auto p=0x40+i*56; put(b,p,type,4); put(b,p+4,7,4); put(b,p+8,off); put(b,p+16,va);
    put(b,p+32,fs); put(b,p+40,ms);
}
void tag(B& b,std::size_t& p,std::uint64_t t,std::uint64_t v) { put(b,p,t);put(b,p+8,v);p+=16; }
B elf(bool library=false) {
    B b(0x900,0); b[0]=0x7f;b[1]='E';b[2]='L';b[3]='F';b[4]=2;b[5]=1;b[6]=1;
    put(b,18,62,2);put(b,24,0x400);put(b,32,0x40);put(b,54,56,2);put(b,56,6,2);
    ph(b,0,1,0x400,0x400,0x200,0x200); ph(b,1,0x61000010,0x600,0x600,0x100,0x100);
    ph(b,2,2,0x700,0,0x100,0x100);ph(b,3,0x61000000,0x800,0,0x100,0x100);
    ph(b,4,7,0x480,0x480,4,16);ph(b,5,0x61000001,0x500,0x500,8,8);
    b[0x400]=0xc3; b[0x480]=1;b[0x481]=2;b[0x482]=3;b[0x483]=4;
    std::size_t p=0x700;
    tag(b,p,0x61000035,0);tag(b,p,0x61000037,64);tag(b,p,0x61000039,64);tag(b,p,0x6100003f,48);
    tag(b,p,0x61000029,112);tag(b,p,0x6100002d,48);tag(b,p,0x6100002f,160);tag(b,p,0x61000031,0);
    tag(b,p,0x61000013,(1ull<<48)|4);tag(b,p,0x6100000d,(1ull<<48)|9);
    if (library) tag(b,p,12,0x400);
    const char strings[]="\0libc\0module\0aaaaaaaaaaa#B#B\0";
    std::memcpy(b.data()+0x800,strings,sizeof(strings));
    // simbolo zero vazio, simbolo um: import/export (endereco 0x410 na libc)
    put(b,0x800+64+24,13,4);b[0x800+64+28]=library?0x12:0x12;
    put(b,0x800+64+30,library?1:0,2);put(b,0x800+64+32,0x410);
    put(b,0x800+112,0x620);put(b,0x800+120,(1ull<<32)|7);put(b,0x800+136,0);
    put(b,0x800+136,0x628);put(b,0x800+144,8);put(b,0x800+152,0x411);
    return b;
}
B self(const B& e) {
    // cabeçalho e segmentos SELF plaintext para os seis PH; nenhum byte comercial.
    B s(32+6*32+0x900,0);put(s,0,0x1d3d154f,4);put(s,24,6,2);
    std::copy(e.begin(),e.begin()+0x190,s.begin()+224);
    for (unsigned i=0;i<6;++i) {
        auto off=i==0?0x400:i==1?0x600:i==2?0x700:i==3?0x800:i==4?0x480:0x500;
        auto len=i==0?0x200:i==1||i==2||i==3?0x100:i==4?4:8;
        put(s,32+i*32,0x800ull|(static_cast<std::uint64_t>(i)<<20));
        put(s,40+i*32,224+off);put(s,48+i*32,len);put(s,56+i*32,len);
        std::copy_n(e.begin()+off,len,s.begin()+224+off);
    }
    return s;
}
}
int main() { return run([] {
    CHECK(Ps4Loader::nid("_init_env")=="bzQExy189ZI");
    CHECK(Ps4Loader::nid("malloc")=="gQX+4GDQjpM");
    auto main=elf(), lib=elf(true);
    GuestMemory mem(32u<<20);
    auto image=Ps4Loader::load(mem,self(main),self(lib));
    CHECK(image.entry==0x10000400);CHECK(image.procparam==0x10000500);
    CHECK(image.tls.filesz==4 && image.tls.memsz==16);
    CHECK(image.modules.size()==1 && image.modules[0].init==0x10010400);
    CHECK(image.imports.size()==1 && image.imports[0].name=="aaaaaaaaaaa#B#B");
    CHECK(image.imports[0].address==0x10010410);
    std::uint64_t value=0;CHECK(mem.load(0x10000620,&value,8)==MemoryError::ok && value==image.imports[0].address);
    CHECK(mem.load(0x10000628,&value,8)==MemoryError::ok && value==0x10000411);
    CpuState cpu;
    const auto tcb=Ps4Bootstrap::prepare(mem,cpu,image);
    CHECK(cpu.fs_base==tcb && cpu.rip==image.entry && cpu.gpr[7]==Ps4Bootstrap::param_base);
    CHECK(cpu.gpr[6]==Ps4Bootstrap::exit_trap);
    CHECK(mem.load(tcb,&value,8)==MemoryError::ok && value==tcb);
    CHECK(mem.load(tcb+8,&value,8)==MemoryError::ok && value==tcb+64);
    CHECK(mem.load(tcb+64+16,&value,8)==MemoryError::ok && value!=0);
    std::uint8_t initial[16]{};
    CHECK(mem.load(Ps4Bootstrap::tls_base,initial,sizeof(initial))==MemoryError::ok);
    CHECK(initial[0]==1 && initial[3]==4 && initial[4]==0);
    CHECK(mem.load(cpu.gpr[4],&value,8)==MemoryError::ok && value==Ps4Bootstrap::exit_trap);
    CHECK(mem.load(Ps4Bootstrap::param_base,&value,8)==MemoryError::ok && value==1);
    Ps4Bootstrap::module_call(mem,cpu,image.modules[0]);
    CHECK(cpu.rip==image.modules[0].init && cpu.gpr[7]==0 && cpu.gpr[6]==0 && cpu.gpr[2]==0);
    CHECK(mem.load(cpu.gpr[4],&value,8)==MemoryError::ok && value==Ps4Bootstrap::exit_trap+32);
    Ps4Bootstrap::game_entry(mem,cpu,image);
    CHECK(cpu.rip==image.entry && cpu.gpr[7]==Ps4Bootstrap::param_base);
    // mov rax, fs:[0]; ret — verifica o caminho real do interpretador.
    const std::uint8_t fs_self[]={0x64,0x48,0x8b,0x04,0x25,0,0,0,0,0xc3};
    CHECK(mem.store(image.entry+0x20,fs_self,sizeof(fs_self))==MemoryError::ok);
    X86Interpreter interp(mem);
    interp.add_trap_range(Ps4Bootstrap::exit_trap,64);
    interp.state=cpu;
    interp.state.rip=image.entry+0x20;
    auto fs_result=interp.run(8);
    CHECK(fs_result.stop==CpuStop::trap && fs_result.rip==Ps4Bootstrap::exit_trap);
    CHECK(interp.state.gpr[0]==tcb);
    GuestMemory mem2(1<<20);auto fallback=Ps4Loader::load(mem2,main);
    CHECK(fallback.imports[0].address==0x7f000000);
    CHECK(mem2.load(0x10000620,&value,8)==MemoryError::ok && value==0x7f000000);
    auto data=main;data[0x800+64+28]=0x11; // import de dado
    GuestMemory mem3(1<<20);auto data_image=Ps4Loader::load(mem3,data);
    CHECK(data_image.imports.size()==1 && data_image.imports[0].data);
    CHECK(data_image.imports[0].trap==0x7e000000);
    CHECK(mem3.load(0x10000620,&value,8)==MemoryError::ok && value==0x7e000000);
    auto tls=lib;put(tls,0x800+120,(1ull<<32)|16);put(tls,0x800+64+24,0,4);
    tls[0x800+64+28]=3;put(tls,0x800+64+30,0,2);
    GuestMemory mem4(1<<20);auto tls_image=Ps4Loader::load(mem4,main,tls);
    CHECK(tls_image.modules[0].tls.module_id==2);
    CHECK(mem4.load(0x10010620,&value,8)==MemoryError::ok && value==2);
    // Synthetic dependency graph: B before A before eboot, with cross-module export.
    auto dependent=main, a=lib, b=lib;
    std::memcpy(dependent.data()+0x800+32,"A.prx",6);
    std::size_t dt=0x700+10*16;tag(dependent,dt,1,32);
    std::memcpy(a.data()+0x800+32,"B.prx",6);
    dt=0x700+11*16;tag(a,dt,1,32);
    // A imports B's export, and has a distinct non-colliding export identity.
    put(a,0x800+64+30,0,2);
    GuestMemory graphmem(32u<<20);
    auto graph=Ps4Loader::load(graphmem,dependent,std::map<std::string,B>{{"A.prx",a},{"B.prx",b}});
    CHECK(graph.modules.size()==2 && graph.modules[0].name=="B.prx" && graph.modules[1].name=="A.prx");
    CHECK(graph.modules[0].tls.module_id==2 && graph.modules[1].tls.module_id==3);
    CHECK(graphmem.load(graph.modules[1].base+0x620,&value,8)==MemoryError::ok && value==graph.modules[0].base+0x410);
    CpuState graphcpu;
    auto graphtcb=Ps4Bootstrap::prepare(graphmem,graphcpu,graph);
    CHECK(graphcpu.fs_base==graphtcb);
    CHECK(graphmem.load(graphtcb+64+16,&value,8)==MemoryError::ok && value!=0); // module 2 (B.prx)
    CHECK(graphmem.load(graphtcb+64+24,&value,8)==MemoryError::ok && value!=0); // module 3 (A.prx)
    bool missing_root=false;try { GuestMemory m(32u<<20);Ps4Loader::load(m,dependent,std::map<std::string,B>{}); }
        catch (const std::runtime_error&) { missing_root=true; } CHECK(missing_root);
    bool missing_dep=false;try { GuestMemory m(32u<<20);Ps4Loader::load(m,dependent,std::map<std::string,B>{{"A.prx",a}}); }
        catch (const std::runtime_error&) { missing_dep=true; } CHECK(missing_dep);
    auto cycle=b;std::memcpy(cycle.data()+0x800+32,"A.prx",6);
    dt=0x700+11*16;tag(cycle,dt,1,32);
    bool cyclic=false;try { GuestMemory m(32u<<20);Ps4Loader::load(m,dependent,std::map<std::string,B>{{"A.prx",a},{"B.prx",cycle}}); }
        catch (const std::runtime_error&) { cyclic=true; } CHECK(cyclic);
    bool limited=false;try { GuestMemory m(1u<<20);Ps4Loader::load(m,dependent,std::map<std::string,B>{{"A.prx",a},{"B.prx",b}},0x7dff0000); }
        catch (const std::runtime_error&) { limited=true; } CHECK(limited);
    auto broken=main; put(broken,0x800+112,0x720);
    bool rejected=false;try { GuestMemory m(1<<20);Ps4Loader::load(m,broken); } catch (const std::runtime_error&) { rejected=true; }
    CHECK(rejected);
    broken=self(main);put(broken,32,0x80a); // segmento SELF criptografado
    rejected=false;try { GuestMemory m(1<<20);Ps4Loader::load(m,broken); } catch (const std::runtime_error&) { rejected=true; }
    CHECK(rejected);

    // Dynamic module loading, module info inspection, and address lookup
    auto dyn_elf = elf(true);
    std::size_t dt_fini = 0x700 + 11 * 16;
    tag(dyn_elf, dt_fini, 13, 0x430);
    put(dyn_elf, 0x800 + 64 + 30, 0, 2);
    dyn_elf[0x480] = 0x11; dyn_elf[0x481] = 0x22; dyn_elf[0x482] = 0x33; dyn_elf[0x483] = 0x44;

    GuestMemory dyn_mem(64u << 20);
    auto dyn_image = Ps4Loader::load(dyn_mem, self(main), self(lib));
    CpuState dyn_cpu;
    const auto dyn_tcb = Ps4Bootstrap::prepare(dyn_mem, dyn_cpu, dyn_image);
    CHECK(dyn_tcb != 0);

    std::vector<std::uint64_t> handles_init;
    Ps4Loader::get_module_list(dyn_image, handles_init);
    CHECK(handles_init.size() == 1 && handles_init[0] == 1);

    Ps4ModuleInfo base_info{};
    CHECK(Ps4Loader::get_module_info(dyn_image, 1, base_info));
    CHECK(base_info.name == "libc.prx" && base_info.handle == 1);
    CHECK(base_info.code_base == dyn_image.modules[0].base + 0x400 && base_info.code_size == 0x200);
    CHECK(base_info.data_base == dyn_image.modules[0].base + 0x600 && base_info.data_size == 0x100);
    CHECK(base_info.init == dyn_image.modules[0].base + 0x400);
    CHECK(base_info.tls_module_id == 2);

    auto dyn_handle = Ps4Loader::load_dynamic_module(dyn_mem, dyn_image, "libdyn.prx", self(dyn_elf));
    CHECK(dyn_handle == 2);
    CHECK(dyn_image.modules.size() == 2);

    const auto& dyn_mod = dyn_image.modules[1];
    CHECK(dyn_mod.handle == dyn_handle);
    CHECK(dyn_mod.name == "libdyn.prx");
    CHECK(dyn_mod.base > dyn_image.modules[0].base);
    CHECK((dyn_mod.base & 0xffff) == 0);
    CHECK(dyn_mod.init == dyn_mod.base + 0x400);
    CHECK(dyn_mod.fini == dyn_mod.base + 0x430);
    CHECK(dyn_mod.code_base == dyn_mod.base + 0x400 && dyn_mod.code_size == 0x200);
    CHECK(dyn_mod.data_base == dyn_mod.base + 0x600 && dyn_mod.data_size == 0x100);
    CHECK(dyn_mod.tls.module_id == 3);
    CHECK(dyn_mod.tls.address == dyn_mod.base + 0x480);

    // Verificação de relocações no módulo dinâmico
    std::uint64_t reloc_val = 0;
    CHECK(dyn_mem.load(dyn_mod.base + 0x628, &reloc_val, 8) == MemoryError::ok && reloc_val == dyn_mod.base + 0x411);
    CHECK(dyn_mem.load(dyn_mod.base + 0x620, &reloc_val, 8) == MemoryError::ok && reloc_val == dyn_image.modules[0].base + 0x410);

    // Verificação do registro TLS dinâmico no DTV
    std::uint64_t dtv_slot3 = 0;
    CHECK(dyn_mem.load(dyn_tcb + 64 + 8 * 3, &dtv_slot3, 8) == MemoryError::ok && dtv_slot3 != 0);
    std::uint8_t dtv_tls[4]{};
    CHECK(dyn_mem.load(dtv_slot3, dtv_tls, 4) == MemoryError::ok);
    CHECK(dtv_tls[0] == 0x11 && dtv_tls[1] == 0x22 && dtv_tls[2] == 0x33 && dtv_tls[3] == 0x44);

    // Consulta de informações de módulo
    Ps4ModuleInfo dyn_info{};
    CHECK(Ps4Loader::get_module_info(dyn_image, dyn_handle, dyn_info));
    CHECK(dyn_info.name == "libdyn.prx");
    CHECK(dyn_info.handle == dyn_handle);
    CHECK(dyn_info.code_base == dyn_mod.code_base && dyn_info.code_size == dyn_mod.code_size);
    CHECK(dyn_info.data_base == dyn_mod.data_base && dyn_info.data_size == dyn_mod.data_size);
    CHECK(dyn_info.tls_address == dyn_mod.tls.address && dyn_info.tls_size == 16);
    CHECK(dyn_info.tls_module_id == 3);
    CHECK(dyn_info.init == dyn_mod.init);
    CHECK(dyn_info.fini == dyn_mod.fini);

    // Busca de módulo por endereço
    auto found_dyn = Ps4Loader::find_module_by_address(dyn_image, dyn_mod.base + 0x10);
    CHECK(found_dyn != nullptr && found_dyn->handle == dyn_handle);
    auto found_base = Ps4Loader::find_module_by_address(dyn_image, dyn_image.modules[0].base + 0x10);
    CHECK(found_base != nullptr && found_base->handle == 1);
    CHECK(Ps4Loader::find_module_by_address(dyn_image, 0x50000000) == nullptr);

    // Listagem de módulos ativos
    std::vector<std::uint64_t> active_handles;
    Ps4Loader::get_module_list(dyn_image, active_handles);
    CHECK(active_handles.size() == 2 && active_handles[0] == 1 && active_handles[1] == dyn_handle);

    // Descarga de módulo
    CHECK(Ps4Loader::unload_dynamic_module(dyn_mem, dyn_image, dyn_handle));

    // Verificação de erro ao consultar handle descarregado ou inválido
    Ps4ModuleInfo invalid_info{};
    CHECK(!Ps4Loader::get_module_info(dyn_image, dyn_handle, invalid_info));
    CHECK(!Ps4Loader::get_module_info(dyn_image, 99999, invalid_info));
    CHECK(Ps4Loader::find_module_by_address(dyn_image, dyn_mod.base + 0x10) == nullptr);

    Ps4Loader::get_module_list(dyn_image, active_handles);
    CHECK(active_handles.size() == 1 && active_handles[0] == 1);

    CHECK(dyn_mem.load(dyn_mod.base, &reloc_val, 8) == MemoryError::unmapped);
    CHECK(dyn_mem.load(dyn_tcb + 64 + 8 * 3, &dtv_slot3, 8) == MemoryError::ok && dtv_slot3 == 0);

    CHECK(!Ps4Loader::unload_dynamic_module(dyn_mem, dyn_image, dyn_handle));
    CHECK(!Ps4Loader::unload_dynamic_module(dyn_mem, dyn_image, 99999));

    // Recusa por falta de memória (budget)
    GuestMemory budget_mem(32768);
    auto budget_img = Ps4Loader::load(budget_mem, main);
    bool budget_fail = false;
    try { Ps4Loader::load_dynamic_module(budget_mem, budget_img, "dyn.prx", dyn_elf); }
    catch (const std::runtime_error&) { budget_fail = true; }
    CHECK(budget_fail);

    // Recusa por colisão / espaço de endereçamento insuficiente
    GuestMemory col_mem(64u << 20);
    auto col_img = Ps4Loader::load(col_mem, main);
    col_mem.map(0x10010000, static_cast<std::size_t>(0x6e000000 - 0x10010000), Permission::read | Permission::write);
    bool col_fail = false;
    try { Ps4Loader::load_dynamic_module(col_mem, col_img, "dyn.prx", dyn_elf); }
    catch (const std::runtime_error&) { col_fail = true; }
    CHECK(col_fail);
}); }
