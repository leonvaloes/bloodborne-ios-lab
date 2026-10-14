#include "ps4_loader.h"
#include "ps4_bootstrap.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace bb {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t limit = 512ull << 20;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
bool is_system_module(const std::string& name) {
    static const std::unordered_set<std::string> system_modules = {
        "libkernel.prx", "libkernel_web.prx", "libSceDiscMap.prx", "libSceSysmodule.prx",
        "libSceLibcInternal.prx", "libScePad.prx", "libSceVideoOut.prx", "libSceAudioOut.prx",
        "libSceUserService.prx", "libSceNet.prx", "libSceNetCtl.prx", "libSceHttp.prx",
        "libSceSsl.prx", "libSceAjm.prx", "libSceSaveData.prx", "libSceNpMatching2.prx",
        "libSceNpCommon.prx", "libSceNpManager.prx", "libSceNpUtility.prx", "libSceNpSignaling.prx",
        "libSceMouse.prx", "libSceSystemService.prx", "libSceNpWebApi.prx", "libSceNpAuth.prx",
        "libSceAudioIn.prx", "libSceCommonDialog.prx", "libSceWebBrowserDialog.prx",
        "libSceImeDialog.prx", "libSceAppContent.prx", "libSceSaveDataDialog.prx",
        "libSceGameLiveStreaming.prx", "libSceVideoRecording.prx", "libSceScreenShot.prx",
        "libSceNpProfileDialog.prx", "libSceVoice.prx", "libSceMsgDialog.prx", "libSceNpCommerce.prx",
        "libSceNpTrophy.prx", "libSceAvPlayer.prx", "libScePlayGo.prx", "libSceNpScoreRanking.prx",
        "libSceRtc.prx"
    };
    if (system_modules.count(name)) return true;
    if (name.rfind("libSceGnmDriver", 0) == 0) return true;
    return false;
}
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
using Identity = Ps4SymbolId;
struct Module {
    Bytes elf, image;
    std::vector<Ph> ph;
    std::vector<Sym> symbols;
    std::vector<Rel> relocs;
    std::map<std::uint64_t, std::uint64_t> tags;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> dynamic;
    std::vector<std::string> needed;
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
    for (const auto& t:m.dynamic) if (t.first==1) m.needed.push_back(str(t.second)); // DT_NEEDED
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

namespace {
Ps4Image link(GuestMemory& memory, Module main, std::vector<std::pair<std::string,Module>> libs,
              std::uint64_t image_base,std::uint64_t code_traps,std::uint64_t data_traps) {
    require(libs.size()<=62,"excesso de modulos TLS");
    std::vector<std::uint64_t> bases;
    std::uint64_t end=main.size;
    for (const auto& item:libs) {
        require(end<=limit-65535,"imagem vinculada excessiva");
        end=(end+65535)&~std::uint64_t(65535);
        require(item.second.size<=limit-end,"imagem vinculada excessiva");
        bases.push_back(end); end+=item.second.size;
    }
    require(image_base<=UINT64_MAX-end,"base invalida");
    require(code_traps <= UINT64_MAX-131072 && data_traps <= UINT64_MAX-16777216 &&
            image_base+end<=data_traps && data_traps+16777216<=code_traps,
            "regioes de imagem/traps sobrepostas");
    Ps4Image result;
    result.base=image_base;
    result.size=main.size;
    result.code_traps=code_traps;
    result.data_traps=data_traps;
    result.next_module_handle=0;
    result.entry=image_base+main.entry;
    result.procparam=main.procparam ? image_base+main.procparam : 0;
    result.tls=main.tls; result.tls.address+=image_base; result.tls.module_id=1;
    for (std::size_t i=0;i<libs.size();++i) {
        auto& lib=libs[i].second;
        lib.tls.module_id=static_cast<std::uint32_t>(i+2);
        Ps4Module info;
        info.handle=++result.next_module_handle;
        info.name=libs[i].first; info.base=image_base+bases[i]; info.size=lib.size;
        auto it=lib.tags.find(12);
        if (it!=lib.tags.end()) { require(lib.mapped(it->second,1),"init de modulo fora da imagem"); info.init=info.base+it->second; }
        auto it_fini=lib.tags.find(13);
        if (it_fini!=lib.tags.end()) { require(lib.mapped(it_fini->second,1),"fini de modulo fora da imagem"); info.fini=info.base+it_fini->second; }
        for (const auto& h:lib.ph) {
            if (h.type==0x61000010) {
                if (info.data_size==0) { info.data_base=info.base+h.vaddr; info.data_size=h.memsz; }
            } else if (h.type==1) {
                if ((h.flags&1) && info.code_size==0) { info.code_base=info.base+h.vaddr; info.code_size=h.memsz; }
                else if (info.data_size==0) { info.data_base=info.base+h.vaddr; info.data_size=h.memsz; }
            }
        }
        info.tls=lib.tls; info.tls.address+=info.base; info.tls.module_id=static_cast<std::uint32_t>(i+2);
        result.modules.push_back(info);
        for (const auto& s:lib.symbols) if (s.section && (s.binding==1 || s.binding==2) &&
            (s.type==1 || s.type==2) && s.name.find('#')!=std::string::npos) {
            require(lib.mapped(s.value,std::max<std::uint64_t>(1,s.size)),"export fora do modulo");
            auto key=lib.identity(s.name);
            require(result.exports.emplace(key,info.base+s.value).second,"export duplicado");
        }
    }
    result.next_tls_module_id=static_cast<std::uint32_t>(libs.size()+2);
    for (const auto& s:main.symbols) if (s.section && (s.binding==1 || s.binding==2) &&
        (s.type==1 || s.type==2) && s.name.find('#')!=std::string::npos) {
        if (main.mapped(s.value,std::max<std::uint64_t>(1,s.size))) {
            auto key=main.identity(s.name);
            if (!key.nid.empty()) result.exports[key]=image_base+s.value;
        }
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
                if (r.kind==16 && &m!=&main) {
                    require(s.name.empty() && s.type==3 && r.addend==0 && m.tls.memsz,"TLS externo nao suportado");
                    value=m.tls.module_id;
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
                            auto ex=result.exports.find(key);
                            // libSceLibcInternal compartilha NIDs com libc, se não houver ambiguidade.
                            if (ex==result.exports.end() && key.lib=="libSceLibcInternal") {
                                Identity alias=key; alias.lib="libc";
                                ex=result.exports.find(alias);
                            }
                            imp.address=ex==result.exports.end()?imp.trap:ex->second;
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
    for (std::size_t i=0;i<libs.size();++i) process(libs[i].second,image_base+bases[i]);
    auto map=[&](const Module& m,std::uint64_t addr) {
        require(memory.map(addr,m.image.size(),Permission::read|Permission::write)==MemoryError::ok,"falha ao mapear imagem");
        require(memory.store(addr,m.image.data(),m.image.size())==MemoryError::ok,"falha ao gravar imagem");
    };
    map(main,image_base);
    for (std::size_t i=0;i<libs.size();++i) map(libs[i].second,image_base+bases[i]);
    if (!result.imports.empty()) {
        require(result.imports.size() <= (UINT64_MAX-data_traps)/4096, "traps de dados excedem endereco");
        require(memory.map(data_traps,result.imports.size()*4096,Permission::read|Permission::write)==MemoryError::ok,
                "falha ao mapear traps de dados");
    }
    return result;
}
} // namespace

Ps4Image Ps4Loader::load(GuestMemory& memory,const Bytes& eboot,const Bytes& libc,
                         std::uint64_t image_base,std::uint64_t code_traps,std::uint64_t data_traps) {
    auto main=parse(eboot);
    std::vector<std::pair<std::string,Module>> libs;
    if (!libc.empty()) libs.emplace_back("libc.prx",parse(libc));
    return link(memory,std::move(main),std::move(libs),image_base,code_traps,data_traps);
}
Ps4Image Ps4Loader::load(GuestMemory& memory,const Bytes& eboot,const std::map<std::string,Bytes>& available,
                         std::uint64_t image_base,std::uint64_t code_traps,std::uint64_t data_traps) {
    auto main=parse(eboot);
    std::map<std::string,unsigned> state;
    std::vector<std::pair<std::string,Module>> ordered;
    auto visit=[&](auto&& self,const std::string& name)->void {
        require(!name.empty() && name.find('/')==std::string::npos && name.find('\\')==std::string::npos &&
                name!="." && name!="..", "DT_NEEDED invalido");
        auto it=available.find(name);
        if (it==available.end()) {
            if (is_system_module(name)) return;
            throw std::runtime_error("modulo DT_NEEDED ausente: "+name);
        }
        require(state[name]!=1,"ciclo DT_NEEDED: "+name);
        if (state[name]==2) return;
        require(state.size()<64,"excesso de modulos");
        state[name]=1;
        auto module=parse(it->second);
        for (const auto& dep:module.needed) self(self,dep);
        state[name]=2;
        ordered.emplace_back(name,std::move(module));
    };
    for (const auto& dep:main.needed) visit(visit,dep);
    if (main.needed.empty()) {
        for (const auto& kv:available) visit(visit,kv.first);
    }
    return link(memory,std::move(main),std::move(ordered),image_base,code_traps,data_traps);
}

std::uint64_t Ps4Loader::load_dynamic_module(GuestMemory& mem,
                                             Ps4Image& image,
                                             const std::string& name,
                                             const std::vector<std::uint8_t>& bytes,
                                             std::uint64_t flags) {
    (void)flags;
    require(!name.empty(), "nome de modulo vazio");
    require(!bytes.empty(), "bytes de modulo vazios");

    Module m = parse(bytes);

    std::uint64_t search_start = image.base + image.size;
    for (const auto& mod : image.modules) {
        search_start = std::max(search_start, mod.base + mod.size);
    }

    std::uint64_t align = 65536; // 64KB
    for (const auto& h : m.ph) {
        if ((h.type == 1 || h.type == 0x61000010) && h.align > align) {
            align = h.align;
        }
    }

    std::uint64_t candidate = (search_start + align - 1) & ~(align - 1);
    constexpr std::uint64_t max_allowed = Ps4Bootstrap::tls_base;

    if (candidate > max_allowed || m.size > max_allowed - candidate) {
        throw std::runtime_error("espaco de enderecamento insuficiente para modulo dinamico");
    }

    MemoryError err = mem.map(candidate, static_cast<std::size_t>(m.size), Permission::read | Permission::write);
    while (err == MemoryError::overlap) {
        candidate += align;
        if (candidate > max_allowed || m.size > max_allowed - candidate) {
            throw std::runtime_error("colisao ou espaco insuficiente para modulo dinamico");
        }
        err = mem.map(candidate, static_cast<std::size_t>(m.size), Permission::read | Permission::write);
    }
    if (err == MemoryError::budget) {
        throw std::runtime_error("orcamento de memoria insuficiente para modulo dinamico");
    }
    if (err != MemoryError::ok) {
        throw std::runtime_error("falha ao mapear memoria para modulo dinamico");
    }

    try {
        std::uint32_t mod_tls_id = 0;
        if (m.tls.memsz > 0) {
            mod_tls_id = image.next_tls_module_id++;
            m.tls.module_id = mod_tls_id;
        }

        for (const auto& r : m.relocs) {
            require(m.mapped(r.target, 8), "relocacao fora de PT_LOAD no modulo dinamico");
            std::uint64_t value = 0;
            if (r.kind == 8) { // R_X86_64_RELATIVE
                value = candidate + r.addend;
            } else if (r.kind == 16) { // R_X86_64_DTPMOD64
                require(mod_tls_id != 0, "DTPMOD64 em modulo sem TLS");
                value = mod_tls_id;
            } else {
                require(r.kind == 1 || r.kind == 6 || r.kind == 7, "tipo de relocacao nao suportado no modulo dinamico");
                require(r.sym < m.symbols.size(), "indice de simbolo invalido no modulo dinamico");
                const auto& s = m.symbols[static_cast<std::size_t>(r.sym)];
                if (s.section) {
                    value = (s.section == 0xfff1 ? 0 : candidate) + s.value + r.addend;
                } else if (s.binding == 2 && s.name.find('#') == std::string::npos) {
                    value = 0;
                } else {
                    auto key = m.identity(s.name);
                    require(!key.nid.empty(), "import sem identidade no modulo dinamico");
                    auto ex = image.exports.find(key);
                    if (ex == image.exports.end() && key.lib == "libSceLibcInternal") {
                        Ps4SymbolId alias = key;
                        alias.lib = "libc";
                        ex = image.exports.find(alias);
                    }

                    if (ex != image.exports.end()) {
                        value = ex->second + r.addend;
                    } else {
                        auto trap_it = image.import_traps.find(s.name);
                        std::uint64_t trap_addr = 0;
                        if (trap_it != image.import_traps.end()) {
                            trap_addr = trap_it->second;
                        } else {
                            std::size_t index = image.imports.size();
                            Ps4Import imp;
                            imp.name = s.name;
                            imp.nid = key.nid;
                            imp.data = (s.type == 1);
                            require(index < 100000 &&
                                    index < (UINT64_MAX - image.code_traps) / 32 &&
                                    index < (UINT64_MAX - image.data_traps) / 4096,
                                    "excesso de imports no modulo dinamico");
                            imp.trap = (imp.data ? image.data_traps + 4096 * index : image.code_traps + 32 * index);
                            imp.address = imp.trap;
                            if (imp.data) {
                                mem.map(imp.trap, 4096, Permission::read | Permission::write);
                            }
                            image.import_traps.emplace(imp.name, imp.trap);
                            image.imports.push_back(imp);
                            trap_addr = imp.trap;
                        }
                        require((s.type == 1 || r.addend == 0) && r.addend < 4096, "addend de import nao suportado");
                        value = trap_addr + r.addend;
                    }
                }
            }
            put(m.image, r.target, value);
        }

        require(mem.store(candidate, m.image.data(), m.image.size()) == MemoryError::ok,
                "falha ao gravar imagem do modulo dinamico");

        Ps4Module info;
        info.name = name;
        info.handle = ++image.next_module_handle;
        info.base = candidate;
        info.size = m.size;
        info.unloaded = false;

        auto it_init = m.tags.find(12);
        if (it_init != m.tags.end()) {
            require(m.mapped(it_init->second, 1), "init fora do modulo dinamico");
            info.init = candidate + it_init->second;
        }
        auto it_fini = m.tags.find(13);
        if (it_fini != m.tags.end()) {
            require(m.mapped(it_fini->second, 1), "fini fora do modulo dinamico");
            info.fini = candidate + it_fini->second;
        }

        for (const auto& h : m.ph) {
            if (h.type == 0x61000010) {
                if (info.data_size == 0) {
                    info.data_base = candidate + h.vaddr;
                    info.data_size = h.memsz;
                }
            } else if (h.type == 1) {
                if ((h.flags & 1) && info.code_size == 0) {
                    info.code_base = candidate + h.vaddr;
                    info.code_size = h.memsz;
                } else if (info.data_size == 0) {
                    info.data_base = candidate + h.vaddr;
                    info.data_size = h.memsz;
                }
            }
        }

        if (m.tls.memsz > 0) {
            info.tls = m.tls;
            info.tls.address = candidate + m.tls.address;
            info.tls.module_id = mod_tls_id;
            Ps4Bootstrap::register_dynamic_tls(mem, image, info.tls);
        }

        for (const auto& s : m.symbols) {
            if (s.section && (s.binding == 1 || s.binding == 2) &&
                (s.type == 1 || s.type == 2) && s.name.find('#') != std::string::npos) {
                if (m.mapped(s.value, std::max<std::uint64_t>(1, s.size))) {
                    auto key = m.identity(s.name);
                    if (!key.nid.empty()) {
                        image.exports[key] = candidate + s.value;
                    }
                }
            }
        }

        image.modules.push_back(info);
        return info.handle;
    } catch (...) {
        mem.unmap(candidate);
        throw;
    }
}

bool Ps4Loader::unload_dynamic_module(GuestMemory& mem, Ps4Image& image, std::uint64_t handle) {
    if (handle == 0) return false;
    for (auto& m : image.modules) {
        if (m.handle == handle) {
            if (m.unloaded) return false;
            m.unloaded = true;
            if (m.base != 0 && m.size != 0) {
                mem.unmap(m.base);
            }
            if (m.tls.module_id != 0) {
                std::uint64_t align = image.tls.align ? image.tls.align : 16;
                std::uint64_t main_size = (image.tls.memsz + align - 1) & ~(align - 1);
                std::uint64_t tcb = Ps4Bootstrap::tls_base + main_size;
                std::uint64_t check_tcb = 0;
                if (mem.load(tcb, &check_tcb, 8) == MemoryError::ok && check_tcb == tcb) {
                    std::uint64_t dtv = tcb + 64;
                    std::uint64_t zero = 0;
                    mem.store(dtv + 8 * m.tls.module_id, &zero, 8);
                }
            }
            return true;
        }
    }
    return false;
}

bool Ps4Loader::get_module_info(const Ps4Image& image, std::uint64_t handle, Ps4ModuleInfo& out_info) {
    if (handle == 0) return false;
    for (const auto& m : image.modules) {
        if (m.handle == handle && !m.unloaded) {
            out_info.name = m.name;
            out_info.handle = m.handle;
            out_info.code_base = m.code_base;
            out_info.code_size = m.code_size;
            out_info.data_base = m.data_base;
            out_info.data_size = m.data_size;
            out_info.tls_address = m.tls.address;
            out_info.tls_size = m.tls.memsz;
            out_info.tls_module_id = m.tls.module_id;
            out_info.init = m.init;
            out_info.fini = m.fini;
            return true;
        }
    }
    return false;
}

const Ps4Module* Ps4Loader::find_module_by_address(const Ps4Image& image, std::uint64_t guest_addr) {
    for (const auto& m : image.modules) {
        if (!m.unloaded && guest_addr >= m.base && guest_addr < m.base + m.size) {
            return &m;
        }
    }
    return nullptr;
}

void Ps4Loader::get_module_list(const Ps4Image& image, std::vector<std::uint64_t>& out_handles) {
    out_handles.clear();
    for (const auto& m : image.modules) {
        if (!m.unloaded) {
            out_handles.push_back(m.handle);
        }
    }
}
} // namespace bb
