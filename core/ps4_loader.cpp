#include "ps4_loader.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace bb {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t limit = 512ull << 20;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool span(std::uint64_t off, std::uint64_t len, std::size_t total) {
    return off <= total && len <= total - off;
}
std::uint64_t get(const Bytes& b, std::uint64_t off, unsigned n) {
    require(span(off, n, b.size()), "SELF/ELF truncado");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i) value |= std::uint64_t(b[static_cast<std::size_t>(off) + i]) << (i * 8);
    return value;
}
struct Ph { std::uint64_t type, flags, off, vaddr, filesz, memsz, align; };
struct Sym { std::string name; std::uint64_t value, size; unsigned type, binding, section; };
struct Rel { std::uint64_t target, kind, sym, addend; };
struct Identity {
    std::string nid, lib, mod;
    unsigned libver = 0, modver = 0;
    bool operator<(const Identity& b) const { return std::tie(nid,lib,libver,mod,modver) < std::tie(b.nid,b.lib,b.libver,b.mod,b.modver); }
};
struct Module {
    Bytes elf, image;
    std::vector<Ph> ph;
    std::vector<Sym> symbols;
    std::vector<Rel> relocs;
    std::map<std::uint64_t, std::uint64_t> tags;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> dynamic;
    std::map<std::string, std::pair<std::string,unsigned>> libs, mods;
    std::uint64_t entry = 0, size = 0, procparam = 0;
    Ps4Tls tls;
    bool mapped(std::uint64_t off, std::uint64_t n) const {
        for (const auto& p : ph) if ((p.type == 1 || p.type == 0x61000010) &&
            off >= p.vaddr && n <= p.memsz && off - p.vaddr <= p.memsz - n) return true;
        return false;
    }
    Identity identity(const std::string& name) const {
        auto a = name.find('#'), b = a == std::string::npos ? a : name.find('#', a + 1);
        if (a == std::string::npos || b == std::string::npos || name.find('#', b + 1) != std::string::npos) return {};
        auto l = libs.find(name.substr(a + 1, b - a - 1)), m = mods.find(name.substr(b + 1));
        require(l != libs.end() && m != mods.end(), "identidade de import desconhecida");
        return {name.substr(0,a), l->second.first, m->second.first, l->second.second, m->second.second};
    }
};
std::string id(std::uint64_t v) {
    static constexpr char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string s;
    do { s.insert(s.begin(), chars[v & 63]); v >>= 6; } while (v);
    return s;
}
Module parse(const Bytes& source) {
    require(source.size() >= 64, "SELF/ELF truncado");
    Module m;
    std::uint64_t base = 0, count = 0;
    bool self = get(source, 0, 4) == 0x1d3d154f;
    if (self) { count = get(source,24,2); base = 32 + count * 32; }
    require(span(base,64,source.size()) && get(source,base,4) == 0x464c457f &&
            get(source,base+4,1) == 2 && get(source,base+5,1) == 1 &&
            get(source,base+6,1) == 1 && get(source,base+18,2) == 62,
            "ELF x86-64 little-endian esperado");
    m.entry = get(source,base+24,8);
    auto phoff = get(source,base+32,8), entsize = get(source,base+54,2), nph = get(source,base+56,2);
    require(entsize == 56 && nph && nph < 256 && phoff <= limit &&
            base <= source.size() && span(phoff,nph*entsize,source.size()-base), "program headers invalidos");
    std::uint64_t file_end = phoff + nph*entsize;
    for (std::uint64_t i=0; i<nph; ++i) {
        auto p = base + phoff + i*entsize;
        Ph h{get(source,p,4),get(source,p+4,4),get(source,p+8,8),get(source,p+16,8),
             get(source,p+32,8),get(source,p+40,8),get(source,p+48,8)};
        require(h.filesz <= limit && h.off <= limit - h.filesz, "segmento ELF excessivo");
        file_end = std::max(file_end,h.off+h.filesz);
        if (h.type == 1 || h.type == 0x61000010) {
            require(h.filesz <= h.memsz && h.vaddr <= limit && h.memsz <= limit-h.vaddr, "PT_LOAD invalido");
            m.size = std::max(m.size,h.vaddr+h.memsz);
        }
        if (h.type == 7) m.tls = {h.vaddr,h.filesz,h.memsz,h.align,0};
        if (h.type == 0x61000001) m.procparam = h.vaddr;
        m.ph.push_back(h);
    }
    require(m.size && m.mapped(m.entry,1) && file_end <= limit, "entrada ou imagem invalida");
    m.elf.resize(static_cast<std::size_t>(file_end));
    if (!self) {
        require(span(0,file_end,source.size()), "ELF truncado");
        std::copy_n(source.begin(),static_cast<std::size_t>(file_end),m.elf.begin());
    } else {
        require(span(base,phoff+nph*entsize,source.size()), "cabecalho SELF truncado");
        std::copy_n(source.begin()+static_cast<std::ptrdiff_t>(base),static_cast<std::size_t>(phoff+nph*entsize),m.elf.begin());
        std::vector<std::pair<std::uint64_t,std::uint64_t>> covered{{0,phoff+nph*entsize}};
        for (std::uint64_t i=0;i<count;++i) {
            auto p=32+i*32, flags=get(source,p,8), off=get(source,p+8,8), len=get(source,p+16,8), mem=get(source,p+24,8);
            if (!(flags & 0x800)) continue;
            require(!(flags & 10), "SELF criptografado/comprimido nao suportado");
            auto index=(flags>>20)&4095;
            require(index<m.ph.size(), "indice SELF invalido");
            const auto& h=m.ph[static_cast<std::size_t>(index)];
            require(len==h.filesz && len==mem && span(off,len,source.size()), "segmento SELF indisponivel");
            std::copy_n(source.begin()+static_cast<std::ptrdiff_t>(off),static_cast<std::size_t>(len),m.elf.begin()+static_cast<std::ptrdiff_t>(h.off));
            covered.emplace_back(h.off,h.off+len);
        }
        for (const auto& h:m.ph) if (h.filesz && (h.type==1 || h.type==2 || h.type==0x61000000 || h.type==0x61000010)) {
            bool ok=false;
            for (auto c:covered) if (c.first<=h.off && h.off<=c.second && h.filesz<=c.second-h.off) ok=true;
            require(ok,"segmento SELF obrigatorio indisponivel");
        }
    }
    m.image.resize(static_cast<std::size_t>(m.size));
    for (const auto& h:m.ph) if (h.type==1 || h.type==0x61000010) {
        require(span(h.off,h.filesz,m.elf.size()), "PT_LOAD truncado");
        std::copy_n(m.elf.begin()+static_cast<std::ptrdiff_t>(h.off),static_cast<std::size_t>(h.filesz),m.image.begin()+static_cast<std::ptrdiff_t>(h.vaddr));
    }
    require(m.tls.filesz <= m.tls.memsz && m.tls.memsz <= (1u<<20) &&
            (!m.tls.memsz || m.mapped(m.tls.address,m.tls.memsz)), "TLS invalido");
    const Ph* dyn=nullptr; const Ph* blob=nullptr;
    for (const auto& h:m.ph) { if (h.type==2) dyn=&h; if (h.type==0x61000000) blob=&h; }
    require(dyn && blob && dyn->filesz%16==0 && span(dyn->off,dyn->filesz,m.elf.size()) &&
            span(blob->off,blob->filesz,m.elf.size()), "dynlibdata invalida");
    for (std::uint64_t i=0;i<dyn->filesz;i+=16) {
        auto tag=get(m.elf,dyn->off+i,8), val=get(m.elf,dyn->off+i+8,8);
        if (!tag) break;
        m.tags[tag]=val;
        m.dynamic.emplace_back(tag,val);
    }
    auto table=[&](std::uint64_t off_tag,std::uint64_t size_tag,std::uint64_t stride) {
        auto a=m.tags.find(off_tag), b=m.tags.find(size_tag);
        require(a!=m.tags.end() && b!=m.tags.end() && span(a->second,b->second,blob->filesz) &&
                b->second%stride==0, "tabela dynlibdata invalida");
        return std::pair<std::uint64_t,std::uint64_t>{blob->off+a->second,b->second};
    };
    auto strings=table(0x61000035,0x61000037,1);
    auto str=[&](std::uint64_t off) {
        require(off<strings.second,"offset de string invalido");
        auto begin=m.elf.begin()+static_cast<std::ptrdiff_t>(strings.first+off);
        auto end=m.elf.begin()+static_cast<std::ptrdiff_t>(strings.first+strings.second);
        auto zero=std::find(begin,end,0);
        require(zero!=end,"string sem terminador");
        return std::string(begin,zero);
    };
    for (const auto& t:m.dynamic) if (t.first==0x61000013 || t.first==0x61000015 || t.first==0x6100000d || t.first==0x6100000f) {
        auto& dst=(t.first==0x61000013 || t.first==0x61000015) ? m.libs : m.mods;
        auto key=id(t.second>>48);
        auto value=std::make_pair(str(t.second&0xffffffff),static_cast<unsigned>((t.second>>32)&65535));
        require(!dst.count(key) || dst.at(key)==value,"identidade duplicada");
        dst[key]=value;
    }
    auto syms=table(0x61000039,0x6100003f,24);
    for (std::uint64_t i=0;i<syms.second;i+=24) {
        auto p=syms.first+i, info=get(m.elf,p+4,1);
        m.symbols.push_back({str(get(m.elf,p,4)),get(m.elf,p+8,8),get(m.elf,p+16,8),
                             static_cast<unsigned>(info&15),static_cast<unsigned>(info>>4),static_cast<unsigned>(get(m.elf,p+6,2))});
    }
    for (auto pair: {std::pair<std::uint64_t,std::uint64_t>{0x61000029,0x6100002d},{0x6100002f,0x61000031}}) {
        auto rel=table(pair.first,pair.second,24);
        for (std::uint64_t i=0;i<rel.second;i+=24) {
            auto p=rel.first+i, info=get(m.elf,p+8,8);
            m.relocs.push_back({get(m.elf,p,8),info&0xffffffff,info>>32,get(m.elf,p+16,8)});
        }
    }
    return m;
}
void put(Bytes& b,std::uint64_t off,std::uint64_t value) {
    require(span(off,8,b.size()),"escrita fora da imagem");
    for (unsigned i=0;i<8;++i) b[static_cast<std::size_t>(off)+i]=static_cast<std::uint8_t>(value>>(i*8));
}
std::uint32_t rol(std::uint32_t x,unsigned n) { return (x<<n)|(x>>(32-n)); }
} // namespace

std::string Ps4Loader::nid(const std::string& name) {
    static constexpr std::uint8_t salt[]={0x51,0x8d,0x64,0xa6,0x35,0xde,0xd8,0xc1,0xe6,0xb0,0x39,0xb1,0xc3,0xe5,0x52,0x30};
    Bytes bytes(name.begin(),name.end());
    for (unsigned char c:name) require(c<128,"NID requer nome ASCII");
    bytes.insert(bytes.end(),std::begin(salt),std::end(salt));
    std::uint64_t bits=static_cast<std::uint64_t>(bytes.size())*8;
    bytes.push_back(0x80);
    while (bytes.size()%64!=56) bytes.push_back(0);
    for (int i=7;i>=0;--i) bytes.push_back(static_cast<std::uint8_t>(bits>>(i*8)));
    std::array<std::uint32_t,5> h{{0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0}};
    for (std::size_t pos=0;pos<bytes.size();pos+=64) {
        std::uint32_t w[80]{};
        for (unsigned i=0;i<16;++i) for (unsigned j=0;j<4;++j) w[i]=(w[i]<<8)|bytes[pos+4*i+j];
        for (unsigned i=16;i<80;++i) w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (unsigned i=0;i<80;++i) {
            std::uint32_t f,k;
            if (i<20) { f=(b&c)|(~b&d); k=0x5a827999; }
            else if (i<40) { f=b^c^d; k=0x6ed9eba1; }
            else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8f1bbcdc; }
            else { f=b^c^d; k=0xca62c1d6; }
            auto t=rol(a,5)+f+e+k+w[i]; e=d; d=c; c=rol(b,30); b=a; a=t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    std::uint8_t reversed[8];
    for (unsigned i=0;i<8;++i) reversed[7-i]=static_cast<std::uint8_t>(h[i/4]>>(24-8*(i%4)));
    static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string result;
    unsigned acc=0, n=0;
    for (auto v:reversed) { acc=(acc<<8)|v; n+=8; while (n>=6) { n-=6; result+=alphabet[(acc>>n)&63]; } }
    if (n) result+=alphabet[(acc<<(6-n))&63];
    return result;
}

Ps4Image Ps4Loader::load(GuestMemory& memory,const Bytes& eboot,const Bytes& libc,
                         std::uint64_t image_base,std::uint64_t code_traps,std::uint64_t data_traps) {
    Module main=parse(eboot);
    Module lib;
    bool linked=!libc.empty();
    if (linked) lib=parse(libc);
    std::uint64_t lib_base=(main.size+65535)&~std::uint64_t(65535);
    require(!linked || (lib_base<=limit && lib.size<=limit-lib_base),"imagem vinculada excessiva");
    require(image_base<=UINT64_MAX-(linked?lib_base+lib.size:main.size),"base invalida");
    require(code_traps <= UINT64_MAX-131072 && data_traps <= UINT64_MAX-16777216 &&
            image_base+main.size<=data_traps &&
            (!linked || image_base+lib_base+lib.size<=data_traps) &&
            data_traps+16777216<=code_traps, "regioes de imagem/traps sobrepostas");
    Ps4Image result;
    result.entry=image_base+main.entry;
    result.procparam=main.procparam ? image_base+main.procparam : 0;
    result.tls=main.tls; result.tls.address+=image_base; result.tls.module_id=1;
    if (linked) {
        Ps4Module info;
        info.base=image_base+lib_base; info.size=lib.size;
        auto it=lib.tags.find(12);
        if (it!=lib.tags.end()) { require(lib.mapped(it->second,1),"init da libc fora da imagem"); info.init=info.base+it->second; }
        info.tls=lib.tls; info.tls.address+=info.base; info.tls.module_id=2;
        result.modules.push_back(info);
    }
    std::map<Identity,std::uint64_t> exports;
    if (linked) for (const auto& s:lib.symbols) if (s.section && (s.binding==1 || s.binding==2) &&
        (s.type==1 || s.type==2) && s.name.find('#')!=std::string::npos) {
        require(lib.mapped(s.value,std::max<std::uint64_t>(1,s.size)),"export fora da libc");
        auto key=lib.identity(s.name);
        require(exports.emplace(key,image_base+lib_base+s.value).second,"export duplicado");
    }
    std::map<Identity,std::size_t> imports;
    auto process=[&](Module& m,std::uint64_t base) {
        for (const auto& r:m.relocs) {
            require(m.mapped(r.target,8),"relocacao fora de PT_LOAD");
            std::uint64_t value=0;
            if (r.kind==8) value=base+r.addend;
            else {
                require(r.sym<m.symbols.size(),"indice de simbolo invalido");
                const auto& s=m.symbols[static_cast<std::size_t>(r.sym)];
                if (r.kind==16 && linked && &m==&lib) {
                    require(s.name.empty() && s.type==3 && r.addend==0 && lib.tls.memsz,"TLS externo nao suportado");
                    value=2;
                } else {
                    require(r.kind==1 || r.kind==6 || r.kind==7,"tipo de relocacao nao suportado");
                    if (s.section) value=(s.section==0xfff1 ? 0 : base)+s.value+r.addend;
                    else if (s.binding==2 && s.name.find('#')==std::string::npos) value=0;
                    else {
                        auto key=m.identity(s.name);
                        require(!key.nid.empty(),"import sem identidade");
                        auto found=imports.find(key);
                        std::size_t index;
                        if (found==imports.end()) {
                            index=result.imports.size(); imports.emplace(key,index);
                            Ps4Import imp; imp.name=s.name; imp.nid=key.nid; imp.data=s.type==1;
                            require(index<100000 && index<(UINT64_MAX-code_traps)/32 && index<(UINT64_MAX-data_traps)/4096,"excesso de imports");
                            imp.trap=(imp.data ? data_traps+4096*index : code_traps+32*index);
                            auto ex=exports.find(key);
                            // libSceLibcInternal compartilha NIDs com libc, se não houver ambiguidade.
                            if (ex==exports.end() && key.lib=="libSceLibcInternal") {
                                Identity alias=key; alias.lib="libc";
                                ex=exports.find(alias);
                            }
                            imp.address=ex==exports.end()?imp.trap:ex->second;
                            result.import_traps.emplace(imp.name,imp.trap);
                            result.imports.push_back(imp);
                        } else index=found->second;
                        const auto& imp=result.imports[index];
                        require((imp.data || r.addend==0) && r.addend<4096,"addend de import nao suportado");
                        value=imp.address+r.addend;
                    }
                }
            }
            put(m.image,r.target,value);
        }
    };
    process(main,image_base);
    if (linked) process(lib,image_base+lib_base);
    auto map=[&](const Module& m,std::uint64_t addr) {
        require(memory.map(addr,m.image.size(),Permission::read|Permission::write)==MemoryError::ok,"falha ao mapear imagem");
        require(memory.store(addr,m.image.data(),m.image.size())==MemoryError::ok,"falha ao gravar imagem");
    };
    map(main,image_base);
    if (linked) map(lib,image_base+lib_base);
    if (!result.imports.empty()) {
        require(result.imports.size() <= (UINT64_MAX-data_traps)/4096, "traps de dados excedem endereco");
        require(memory.map(data_traps,result.imports.size()*4096,Permission::read|Permission::write)==MemoryError::ok,
                "falha ao mapear traps de dados");
    }
    return result;
}
} // namespace bb
