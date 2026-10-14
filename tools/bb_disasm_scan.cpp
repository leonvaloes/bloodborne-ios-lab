// Varredura linear local de segmentos executáveis BBPROBE2; não publica bytes da imagem.
#include "../core/x86_interp.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "uso: bb-disasm-scan boot.bin\n"); return 2; }
    std::ifstream f(argv[1], std::ios::binary);
    char magic[8] = {};
    f.read(magic, 8);
    if (!f || std::memcmp(magic, "BBPROBE2", 8)) { std::fprintf(stderr, "BBPROBE2 invalido\n"); return 2; }
    auto u64 = [&]() { std::uint64_t v = 0; f.read(reinterpret_cast<char*>(&v), 8); return v; };
    const auto size = u64(); u64(); const auto ns = u64(), nr = u64(), ni = u64(); u64();
    if (!f || !size || size > (512u << 20) || !ns || ns > 64 || nr > 1000000 || ni > 100000) return 2;
    struct Segment { std::uint64_t offset, length, flags; };
    std::vector<Segment> segments;
    for (std::uint64_t i = 0; i < ns; ++i) {
        Segment s{u64(), u64(), u64()};
        if (!f || s.offset > size || s.length > size - s.offset) return 2;
        segments.push_back(s);
    }
    f.seekg(static_cast<std::streamoff>(ni * 128 + nr * 32), std::ios::cur);
    std::vector<std::uint8_t> image(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(size));
    if (!f) { std::fprintf(stderr, "imagem truncada\n"); return 2; }
    std::map<std::uint32_t, std::uint64_t> missing;
    std::uint64_t decoded = 0, undecodable = 0;
    for (const auto& s : segments) {
        if (!(s.flags & 4)) continue;
        for (std::uint64_t at = s.offset; at < s.offset + s.length;) {
            const auto d = bb::decode_x86(image.data() + at, static_cast<std::size_t>(std::min<std::uint64_t>(15, s.offset + s.length - at)));
            if (!d.length) { ++undecodable; ++at; continue; }
            ++decoded;
            if (!d.supported) ++missing[d.opcode];
            at += d.length;
        }
    }
    std::vector<std::pair<std::uint32_t, std::uint64_t>> rows(missing.begin(), missing.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second || (a.second == b.second && a.first < b.first); });
    std::printf("decodificadas=%llu recuperacoes=%llu (varredura linear; dados intercalados podem contaminar)\n",
                static_cast<unsigned long long>(decoded), static_cast<unsigned long long>(undecodable));
    for (const auto& row : rows) std::printf("%06x %llu\n", row.first, static_cast<unsigned long long>(row.second));
}
