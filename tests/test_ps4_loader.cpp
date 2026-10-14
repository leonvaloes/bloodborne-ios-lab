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
    auto broken=main; put(broken,0x800+112,0x720);
    bool rejected=false;try { GuestMemory m(1<<20);Ps4Loader::load(m,broken); } catch (const std::runtime_error&) { rejected=true; }
    CHECK(rejected);
    broken=self(main);put(broken,32,0x80a); // segmento SELF criptografado
    rejected=false;try { GuestMemory m(1<<20);Ps4Loader::load(m,broken); } catch (const std::runtime_error&) { rejected=true; }
    CHECK(rejected);
}); }
