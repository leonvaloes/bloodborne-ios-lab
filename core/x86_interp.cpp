#include "x86_interp.h"
#include <cstring>
#include <sstream>

namespace bb {
namespace {

struct Fault { std::uint64_t addr; };
struct Unsupported {};
struct DivErr {};
struct HaltEx {};
struct UdEx {};

inline std::uint64_t mask_of(int size) { return size == 8 ? ~0ULL : ((1ULL << (size * 8)) - 1); }
inline bool msb_of(std::uint64_t v, int size) { return ((v >> (size * 8 - 1)) & 1) != 0; }
inline std::int64_t sext(std::uint64_t v, int size) {
    switch (size) {
    case 1: return static_cast<std::int8_t>(v);
    case 2: return static_cast<std::int16_t>(v);
    case 4: return static_cast<std::int32_t>(v);
    default: return static_cast<std::int64_t>(v);
    }
}
inline bool parity_even(std::uint64_t v) {
    unsigned b = static_cast<unsigned>(v & 0xFF);
    b ^= b >> 4; b ^= b >> 2; b ^= b >> 1;
    return (b & 1) == 0;
}
void mul64(std::uint64_t a, std::uint64_t b, std::uint64_t& hi, std::uint64_t& lo) {
    const std::uint64_t a0 = a & 0xFFFFFFFFULL, a1 = a >> 32, b0 = b & 0xFFFFFFFFULL, b1 = b >> 32;
    const std::uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const std::uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
    lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
void neg128(std::uint64_t& hi, std::uint64_t& lo) {
    lo = ~lo + 1;
    hi = ~hi + (lo == 0 ? 1 : 0);
}
bool udiv128(std::uint64_t hi, std::uint64_t lo, std::uint64_t d, std::uint64_t& q, std::uint64_t& r) {
    if (d == 0 || hi >= d) return false;
    std::uint64_t rem = hi;
    q = 0;
    for (int i = 63; i >= 0; --i) {
        const bool carry = (rem >> 63) != 0;
        rem = (rem << 1) | ((lo >> i) & 1);
        q <<= 1;
        if (carry || rem >= d) { rem -= d; q |= 1; }
    }
    r = rem;
    return true;
}
bool cond_true(unsigned cc, std::uint64_t f) {
    const bool cf = f & FLAG_CF, zf = f & FLAG_ZF, sf = f & FLAG_SF, of = f & FLAG_OF, pf = f & FLAG_PF;
    bool r = false;
    switch (cc >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf || zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf != of; break;
    default: r = zf || (sf != of); break;
    }
    return (cc & 1) ? !r : r;
}

struct Rm { bool reg; int r; std::uint64_t addr; };

struct Exec {
    CpuState& s;
    GuestMemory& m;
    std::uint8_t buf[16] = {};
    std::size_t n = 0, pos = 0;
    std::uint64_t start = 0;
    bool p66 = false, rep = false, repne = false, rex = false, rw = false, rr = false, rx = false,
         rb = false, fs = false;
    bool branched = false;

    Exec(CpuState& st, GuestMemory& mem) : s(st), m(mem) {}

    // ---- fetch ----
    std::uint8_t u8() { if (pos >= n) throw Fault{start + pos}; return buf[pos++]; }
    std::uint16_t u16() { std::uint16_t a = u8(); a |= static_cast<std::uint16_t>(u8()) << 8; return a; }
    std::uint32_t u32() { std::uint32_t a = u16(); a |= static_cast<std::uint32_t>(u16()) << 16; return a; }
    std::uint64_t u64() { std::uint64_t a = u32(); a |= static_cast<std::uint64_t>(u32()) << 32; return a; }
    std::uint64_t imm_of(int sz) {  // immediate for operand size sz, sign-extended to 64
        if (sz == 1) return static_cast<std::uint64_t>(static_cast<std::int8_t>(u8()));
        if (sz == 2) return static_cast<std::uint64_t>(static_cast<std::int16_t>(u16()));
        return static_cast<std::uint64_t>(static_cast<std::int32_t>(u32()));
    }

    // ---- registers ----
    std::uint64_t rd_reg(int r, int sz) const {
        switch (sz) {
        case 8: return s.gpr[r];
        case 4: return s.gpr[r] & 0xFFFFFFFFULL;
        case 2: return s.gpr[r] & 0xFFFFULL;
        default:
            if (!rex && r >= 4 && r < 8) return (s.gpr[r - 4] >> 8) & 0xFF;
            return s.gpr[r] & 0xFF;
        }
    }
    void wr_reg(int r, int sz, std::uint64_t v) {
        switch (sz) {
        case 8: s.gpr[r] = v; break;
        case 4: s.gpr[r] = v & 0xFFFFFFFFULL; break;
        case 2: s.gpr[r] = (s.gpr[r] & ~0xFFFFULL) | (v & 0xFFFF); break;
        default:
            if (!rex && r >= 4 && r < 8)
                s.gpr[r - 4] = (s.gpr[r - 4] & ~0xFF00ULL) | ((v & 0xFF) << 8);
            else
                s.gpr[r] = (s.gpr[r] & ~0xFFULL) | (v & 0xFF);
        }
    }
    std::uint64_t rd_mem(std::uint64_t a, int sz) const {
        std::uint64_t v = 0;
        if (m.load(a, &v, static_cast<std::size_t>(sz)) != MemoryError::ok) throw Fault{a};
        return v;
    }
    void wr_mem(std::uint64_t a, int sz, std::uint64_t v) {
        if (m.store(a, &v, static_cast<std::size_t>(sz)) != MemoryError::ok) throw Fault{a};
    }
    std::uint64_t rd_rm(const Rm& r, int sz) const { return r.reg ? rd_reg(r.r, sz) : rd_mem(r.addr, sz); }
    void wr_rm(const Rm& r, int sz, std::uint64_t v) { if (r.reg) wr_reg(r.r, sz, v); else wr_mem(r.addr, sz, v); }
    void push(std::uint64_t v) { wr_mem(s.gpr[4] - 8, 8, v); s.gpr[4] -= 8; }
    std::uint64_t pop() { const std::uint64_t v = rd_mem(s.gpr[4], 8); s.gpr[4] += 8; return v; }

    // ---- ModRM; imm_after = immediate bytes that follow (for RIP-relative) ----
    Rm decode_rm(int& regfield, int imm_after) {
        const std::uint8_t mrm = u8();
        const int mod = mrm >> 6, rm = mrm & 7;
        regfield = ((mrm >> 3) & 7) | (rr ? 8 : 0);
        if (mod == 3) return Rm{true, rm | (rb ? 8 : 0), 0};
        std::uint64_t addr = 0;
        bool riprel = false;
        std::int64_t disp = 0;
        if (rm == 4) {
            const std::uint8_t sib = u8();
            const int scale = sib >> 6;
            const int idx = ((sib >> 3) & 7) | (rx ? 8 : 0);
            const int base = sib & 7;
            if (idx != 4) addr += s.gpr[idx] << scale;
            if (base == 5 && mod == 0) disp = static_cast<std::int32_t>(u32());
            else addr += s.gpr[base | (rb ? 8 : 0)];
        } else if (rm == 5 && mod == 0) {
            disp = static_cast<std::int32_t>(u32());
            riprel = true;
        } else {
            addr = s.gpr[rm | (rb ? 8 : 0)];
        }
        if (mod == 1) disp += static_cast<std::int8_t>(u8());
        else if (mod == 2) disp += static_cast<std::int32_t>(u32());
        addr += static_cast<std::uint64_t>(disp);
        if (riprel) addr += start + pos + static_cast<std::uint64_t>(imm_after);
        if (fs) addr += s.fs_base;
        return Rm{false, 0, addr};
    }

    // ---- flags ----
    void set_szp(std::uint64_t res, int sz) {
        std::uint64_t f = s.rflags & ~(FLAG_ZF | FLAG_SF | FLAG_PF);
        if ((res & mask_of(sz)) == 0) f |= FLAG_ZF;
        if (msb_of(res, sz)) f |= FLAG_SF;
        if (parity_even(res)) f |= FLAG_PF;
        s.rflags = f;
    }
    void set_flag(std::uint64_t bit, bool on) { s.rflags = on ? (s.rflags | bit) : (s.rflags & ~bit); }
    std::uint64_t alu(int op, std::uint64_t a, std::uint64_t b, int sz) {
        const std::uint64_t msk = mask_of(sz);
        a &= msk; b &= msk;
        std::uint64_t res = 0;
        bool cf = false, of = false, af = false;
        const std::uint64_t carry = (s.rflags & FLAG_CF) ? 1 : 0;
        switch (op) {
        case 0: case 2: {  // add / adc
            const std::uint64_t c = (op == 2) ? carry : 0;
            if (sz == 8) {
                const std::uint64_t t = a + b;
                res = t + c;
                cf = t < a || res < t;
            } else {
                const std::uint64_t full = a + b + c;
                res = full & msk;
                cf = ((full >> (sz * 8)) & 1) != 0;
            }
            of = msb_of((a ^ res) & (b ^ res), sz);
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
        }
        case 3: case 5: case 7: {  // sbb / sub / cmp
            const std::uint64_t c = (op == 3) ? carry : 0;
            const std::uint64_t t = (a - b) & msk;
            res = (t - c) & msk;
            cf = a < b || t < c;
            of = msb_of((a ^ b) & (a ^ res), sz);
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
        }
        case 1: res = a | b; break;
        case 4: res = a & b; break;
        default: res = a ^ b; break;  // 6
        }
        set_flag(FLAG_CF, cf);
        set_flag(FLAG_OF, of);
        set_flag(FLAG_AF, af);
        set_szp(res, sz);
        return res & msk;
    }
    std::uint64_t incdec(std::uint64_t a, int sz, bool dec) {
        const std::uint64_t keep = s.rflags & FLAG_CF;
        const std::uint64_t r = alu(dec ? 5 : 0, a, 1, sz);
        s.rflags = (s.rflags & ~FLAG_CF) | keep;
        return r;
    }
    std::uint64_t shift(int op, std::uint64_t a, unsigned count, int sz) {
        const unsigned bits = static_cast<unsigned>(sz) * 8;
        const std::uint64_t msk = mask_of(sz);
        count &= (sz == 8) ? 0x3F : 0x1F;
        a &= msk;
        if (count == 0) return a;
        std::uint64_t res = a;
        bool cf = false, of = false;
        switch (op) {
        case 4: case 6:  // shl / sal
            res = (count >= 64 ? 0 : (a << count)) & msk;
            cf = count <= bits ? (((a >> (bits - count)) & 1) != 0) : false;
            of = msb_of(res, sz) != cf;
            set_szp(res, sz);
            break;
        case 5:  // shr
            res = count >= 64 ? 0 : (a >> count);
            cf = count <= bits ? (((a >> (count - 1)) & 1) != 0) : false;
            of = msb_of(a, sz);
            set_szp(res, sz);
            break;
        case 7: {  // sar
            const std::int64_t sa = sext(a, sz);
            const unsigned c = count >= 64 ? 63 : count;
            res = static_cast<std::uint64_t>(sa >> c) & msk;
            cf = ((sa >> (count > 63 ? 63 : count - 1)) & 1) != 0;
            of = false;
            set_szp(res, sz);
            break;
        }
        case 0: {  // rol
            const unsigned c = count % bits;
            res = c ? (((a << c) | (a >> (bits - c))) & msk) : a;
            cf = (res & 1) != 0;
            of = msb_of(res, sz) != cf;
            break;
        }
        case 1: {  // ror
            const unsigned c = count % bits;
            res = c ? (((a >> c) | (a << (bits - c))) & msk) : a;
            cf = msb_of(res, sz);
            of = msb_of(res, sz) != (((res >> (bits - 2)) & 1) != 0);
            break;
        }
        default: throw Unsupported{};  // rcl / rcr
        }
        set_flag(FLAG_CF, cf);
        set_flag(FLAG_OF, of);
        return res;
    }

    // ---- SSE helpers ----
    static float f32(std::uint64_t v) { float f; std::uint32_t u = static_cast<std::uint32_t>(v); std::memcpy(&f, &u, 4); return f; }
    static double f64(std::uint64_t v) { double d; std::memcpy(&d, &v, 8); return d; }
    static std::uint64_t bits32(float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; }
    static std::uint64_t bits64(double d) { std::uint64_t u; std::memcpy(&u, &d, 8); return u; }
    void rd_xmm_rm(const Rm& r, std::uint64_t out[2]) const {
        if (r.reg) { out[0] = s.xmm[r.r][0]; out[1] = s.xmm[r.r][1]; return; }
        out[0] = rd_mem(r.addr, 8);
        out[1] = rd_mem(r.addr + 8, 8);
    }
    void wr_xmm_rm(const Rm& r, const std::uint64_t v[2]) {
        if (r.reg) { s.xmm[r.r][0] = v[0]; s.xmm[r.r][1] = v[1]; return; }
        wr_mem(r.addr, 8, v[0]);
        wr_mem(r.addr + 8, 8, v[1]);
    }

    void step();
    void two_byte(int opsz);
    void sse(std::uint8_t op2, int opsz);
};

void Exec::two_byte(int opsz) {
    const std::uint8_t op2 = u8();
    int rg = 0;
    if (op2 == 0x0B) throw UdEx{};
    if (op2 == 0x1F || op2 == 0x1E || op2 == 0x18 || op2 == 0x0D) {  // nop r/m, endbr64, prefetch
        decode_rm(rg, 0);
        return;
    }
    if (op2 >= 0x80 && op2 <= 0x8F) {
        const std::int32_t rel = static_cast<std::int32_t>(u32());
        if (cond_true(op2 & 15, s.rflags)) { s.rip = start + pos + static_cast<std::int64_t>(rel); branched = true; }
        return;
    }
    if (op2 >= 0x90 && op2 <= 0x9F) {
        const Rm rm = decode_rm(rg, 0);
        wr_rm(rm, 1, cond_true(op2 & 15, s.rflags) ? 1 : 0);
        return;
    }
    if (op2 >= 0x40 && op2 <= 0x4F) {
        const Rm rm = decode_rm(rg, 0);
        const std::uint64_t v = rd_rm(rm, opsz);
        if (cond_true(op2 & 15, s.rflags)) wr_reg(rg, opsz, v);
        else if (opsz == 4) wr_reg(rg, 4, rd_reg(rg, 4));
        return;
    }
    switch (op2) {
    case 0xAF: {  // imul r, r/m
        const Rm rm = decode_rm(rg, 0);
        const std::uint64_t a = rd_reg(rg, opsz), b = rd_rm(rm, opsz);
        bool ovf;
        std::uint64_t res;
        if (opsz == 8) {
            std::uint64_t hi, lo;
            const bool neg = (static_cast<std::int64_t>(a) < 0) != (static_cast<std::int64_t>(b) < 0);
            mul64(static_cast<std::int64_t>(a) < 0 ? ~a + 1 : a, static_cast<std::int64_t>(b) < 0 ? ~b + 1 : b, hi, lo);
            if (neg) neg128(hi, lo);
            res = lo;
            ovf = hi != (static_cast<std::int64_t>(lo) < 0 ? ~0ULL : 0ULL);
        } else {
            const std::int64_t p = sext(a, opsz) * sext(b, opsz);
            res = static_cast<std::uint64_t>(p) & mask_of(opsz);
            ovf = sext(res, opsz) != p;
        }
        wr_reg(rg, opsz, res);
        set_flag(FLAG_CF, ovf);
        set_flag(FLAG_OF, ovf);
        set_szp(res, opsz);
        return;
    }
    case 0xB6: case 0xB7: case 0xBE: case 0xBF: {
        const int ssz = (op2 & 1) ? 2 : 1;
        const Rm rm = decode_rm(rg, 0);
        const std::uint64_t v = rd_rm(rm, ssz);
        wr_reg(rg, opsz, (op2 >= 0xBE) ? static_cast<std::uint64_t>(sext(v, ssz)) : v);
        return;
    }
    case 0xB0: case 0xB1: {  // cmpxchg
        const int sz = (op2 & 1) ? opsz : 1;
        const Rm rm = decode_rm(rg, 0);
        const std::uint64_t dest = rd_rm(rm, sz), acc = rd_reg(0, sz);
        alu(7, acc, dest, sz);
        if (s.rflags & FLAG_ZF) wr_rm(rm, sz, rd_reg(rg, sz));
        else wr_reg(0, sz, dest);
        return;
    }
    case 0xC0: case 0xC1: {  // xadd
        const int sz = (op2 & 1) ? opsz : 1;
        const Rm rm = decode_rm(rg, 0);
        const std::uint64_t d = rd_rm(rm, sz), r = rd_reg(rg, sz);
        const std::uint64_t t = alu(0, d, r, sz);
        wr_reg(rg, sz, d);
        wr_rm(rm, sz, t);
        return;
    }
    case 0xBA: {  // bt/bts/btr/btc r/m, imm8
        const Rm rm = decode_rm(rg, 1);
        const unsigned bit = u8() & (opsz * 8 - 1);
        const std::uint64_t v = rd_rm(rm, opsz);
        const int sub = rg & 7;
        if (sub < 4) throw Unsupported{};
        set_flag(FLAG_CF, ((v >> bit) & 1) != 0);
        if (sub == 5) wr_rm(rm, opsz, v | (1ULL << bit));
        else if (sub == 6) wr_rm(rm, opsz, v & ~(1ULL << bit));
        else if (sub == 7) wr_rm(rm, opsz, v ^ (1ULL << bit));
        return;
    }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {
        const Rm rm = decode_rm(rg, 0);
        if (!rm.reg) throw Unsupported{};
        const unsigned bit = static_cast<unsigned>(rd_reg(rg, opsz)) & (opsz * 8 - 1);
        const std::uint64_t v = rd_reg(rm.r, opsz);
        set_flag(FLAG_CF, ((v >> bit) & 1) != 0);
        if (op2 == 0xAB) wr_reg(rm.r, opsz, v | (1ULL << bit));
        else if (op2 == 0xB3) wr_reg(rm.r, opsz, v & ~(1ULL << bit));
        else if (op2 == 0xBB) wr_reg(rm.r, opsz, v ^ (1ULL << bit));
        return;
    }
    case 0xBC: case 0xBD: case 0xB8: {
        const Rm rm = decode_rm(rg, 0);
        const std::uint64_t v = rd_rm(rm, opsz) & mask_of(opsz);
        const int bits = opsz * 8;
        if (op2 == 0xB8) {
            if (!rep) throw Unsupported{};
            int c = 0;
            for (std::uint64_t t = v; t; t &= t - 1) ++c;
            wr_reg(rg, opsz, static_cast<std::uint64_t>(c));
            s.rflags &= ~(FLAG_CF | FLAG_OF | FLAG_SF | FLAG_PF | FLAG_AF);
            set_flag(FLAG_ZF, v == 0);
            return;
        }
        int idx = -1;
        if (op2 == 0xBC) { for (int i = 0; i < bits; ++i) if ((v >> i) & 1) { idx = i; break; } }
        else { for (int i = bits - 1; i >= 0; --i) if ((v >> i) & 1) { idx = i; break; } }
        if (!rep) {  // bsf / bsr
            set_flag(FLAG_ZF, v == 0);
            if (v != 0) wr_reg(rg, opsz, static_cast<std::uint64_t>(idx));
            return;
        }
        std::uint64_t out;  // tzcnt / lzcnt
        if (v == 0) out = static_cast<std::uint64_t>(bits);
        else out = (op2 == 0xBC) ? static_cast<std::uint64_t>(idx) : static_cast<std::uint64_t>(bits - 1 - idx);
        wr_reg(rg, opsz, out);
        set_flag(FLAG_CF, v == 0);
        set_flag(FLAG_ZF, out == 0);
        return;
    }
    default: break;
    }
    if (op2 >= 0xC8 && op2 <= 0xCF) {  // bswap
        const int r = (op2 & 7) | (rb ? 8 : 0);
        std::uint64_t v = rd_reg(r, opsz), o = 0;
        for (int i = 0; i < opsz; ++i) o |= ((v >> (8 * i)) & 0xFF) << (8 * (opsz - 1 - i));
        wr_reg(r, opsz, o);
        return;
    }
    sse(op2, opsz);
}

void Exec::sse(std::uint8_t op2, int opsz) {
    int rg = 0;
    const bool pd = p66;
    switch (op2) {
    case 0x10: case 0x11: {
        const Rm rm = decode_rm(rg, 0);
        if (rep || repne) {  // movss / movsd
            const int w = rep ? 4 : 8;
            if (op2 == 0x10) {
                if (rm.reg) {
                    const std::uint64_t src = s.xmm[rm.r][0];
                    s.xmm[rg][0] = (w == 4) ? ((s.xmm[rg][0] & ~0xFFFFFFFFULL) | (src & 0xFFFFFFFFULL)) : src;
                } else {
                    s.xmm[rg][0] = rd_mem(rm.addr, w);
                    s.xmm[rg][1] = 0;
                }
            } else if (rm.reg) {
                const std::uint64_t src = s.xmm[rg][0];
                s.xmm[rm.r][0] = (w == 4) ? ((s.xmm[rm.r][0] & ~0xFFFFFFFFULL) | (src & 0xFFFFFFFFULL)) : src;
            } else {
                wr_mem(rm.addr, w, s.xmm[rg][0]);
            }
            return;
        }
        if (op2 == 0x10) { std::uint64_t v[2]; rd_xmm_rm(rm, v); s.xmm[rg][0] = v[0]; s.xmm[rg][1] = v[1]; }
        else wr_xmm_rm(rm, s.xmm[rg]);
        return;
    }
    case 0x28: case 0x29: case 0x6F: case 0x7F: {
        if ((op2 == 0x6F || op2 == 0x7F) && !(p66 || rep)) throw Unsupported{};  // MMX
        const Rm rm = decode_rm(rg, 0);
        if (op2 == 0x28 || op2 == 0x6F) { std::uint64_t v[2]; rd_xmm_rm(rm, v); s.xmm[rg][0] = v[0]; s.xmm[rg][1] = v[1]; }
        else wr_xmm_rm(rm, s.xmm[rg]);
        return;
    }
    case 0x6E: {
        if (!pd) throw Unsupported{};
        const Rm rm = decode_rm(rg, 0);
        s.xmm[rg][0] = rd_rm(rm, opsz == 8 ? 8 : 4);
        s.xmm[rg][1] = 0;
        return;
    }
    case 0x7E: {
        const Rm rm = decode_rm(rg, 0);
        if (rep) {  // movq xmm, xmm/m64
            s.xmm[rg][0] = rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8);
            s.xmm[rg][1] = 0;
        } else if (pd) {
            wr_rm(rm, opsz == 8 ? 8 : 4, s.xmm[rg][0]);
        } else throw Unsupported{};
        return;
    }
    case 0xD6: {
        if (!pd) throw Unsupported{};
        const Rm rm = decode_rm(rg, 0);
        if (rm.reg) { s.xmm[rm.r][0] = s.xmm[rg][0]; s.xmm[rm.r][1] = 0; }
        else wr_mem(rm.addr, 8, s.xmm[rg][0]);
        return;
    }
    case 0xEF: case 0x57: case 0x54: case 0x55: case 0x56: case 0xDB: case 0xDF: case 0xEB: {
        const Rm rm = decode_rm(rg, 0);
        std::uint64_t v[2];
        rd_xmm_rm(rm, v);
        for (int i = 0; i < 2; ++i) {
            switch (op2) {
            case 0xEF: case 0x57: s.xmm[rg][i] ^= v[i]; break;
            case 0x54: case 0xDB: s.xmm[rg][i] &= v[i]; break;
            case 0x55: case 0xDF: s.xmm[rg][i] = ~s.xmm[rg][i] & v[i]; break;
            default: s.xmm[rg][i] |= v[i]; break;
            }
        }
        return;
    }
    case 0x58: case 0x59: case 0x5C: case 0x5E: {
        if (!(rep || repne)) throw Unsupported{};
        const Rm rm = decode_rm(rg, 0);
        if (rep) {
            const float a = f32(s.xmm[rg][0]), b = f32(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 4));
            const float r = op2 == 0x58 ? a + b : op2 == 0x59 ? a * b : op2 == 0x5C ? a - b : a / b;
            s.xmm[rg][0] = (s.xmm[rg][0] & ~0xFFFFFFFFULL) | bits32(r);
        } else {
            const double a = f64(s.xmm[rg][0]), b = f64(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8));
            const double r = op2 == 0x58 ? a + b : op2 == 0x59 ? a * b : op2 == 0x5C ? a - b : a / b;
            s.xmm[rg][0] = bits64(r);
        }
        return;
    }
    case 0x2A: {  // cvtsi2ss / cvtsi2sd
        if (!(rep || repne)) throw Unsupported{};
        const Rm rm = decode_rm(rg, 0);
        const std::int64_t v = sext(rd_rm(rm, opsz == 8 ? 8 : 4), opsz == 8 ? 8 : 4);
        if (rep) s.xmm[rg][0] = (s.xmm[rg][0] & ~0xFFFFFFFFULL) | bits32(static_cast<float>(v));
        else s.xmm[rg][0] = bits64(static_cast<double>(v));
        return;
    }
    case 0x2C: case 0x2D: {  // cvtt/cvt ss/sd -> int (truncating variant only for 0x2C)
        if (!(rep || repne)) throw Unsupported{};
        const Rm rm = decode_rm(rg, 0);
        double d = rep ? static_cast<double>(f32(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 4)))
                       : f64(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8));
        if (op2 == 0x2D) throw Unsupported{};  // rounding mode semantics not modeled
        const int sz = opsz == 8 ? 8 : 4;
        std::int64_t out;
        if (d != d || d >= 9223372036854775808.0 || d < -9223372036854775808.0) out = INT64_MIN;
        else out = static_cast<std::int64_t>(d);
        if (sz == 4 && (out > INT32_MAX || out < INT32_MIN)) out = INT32_MIN;
        wr_reg(rg, sz, static_cast<std::uint64_t>(out));
        return;
    }
    case 0x2E: case 0x2F: {
        const Rm rm = decode_rm(rg, 0);
        double a, b;
        if (pd) { a = f64(s.xmm[rg][0]); b = f64(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8)); }
        else { a = f32(s.xmm[rg][0]); b = f32(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 4)); }
        s.rflags &= ~(FLAG_OF | FLAG_SF | FLAG_AF | FLAG_ZF | FLAG_PF | FLAG_CF);
        if (a != a || b != b) s.rflags |= FLAG_ZF | FLAG_PF | FLAG_CF;
        else if (a > b) {}
        else if (a < b) s.rflags |= FLAG_CF;
        else s.rflags |= FLAG_ZF;
        return;
    }
    default: throw Unsupported{};
    }
}

void Exec::step() {
    // prefixes
    for (;;) {
        if (pos >= n) throw Fault{start + pos};
        const std::uint8_t b = buf[pos];
        if (b == 0x66) { p66 = true; ++pos; }
        else if (b == 0xF3) { rep = true; ++pos; }
        else if (b == 0xF2) { repne = true; ++pos; }
        else if (b == 0xF0) { ++pos; }
        else if (b == 0x64) { fs = true; ++pos; }
        else if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x65) { ++pos; }
        else if (b == 0x67) { throw Unsupported{}; }
        else break;
    }
    if (pos < n && buf[pos] >= 0x40 && buf[pos] <= 0x4F) {
        const std::uint8_t r = buf[pos++];
        rex = true; rw = r & 8; rr = r & 4; rx = r & 2; rb = r & 1;
    }
    const int opsz = rw ? 8 : (p66 ? 2 : 4);
    const std::uint8_t op = u8();
    int rg = 0;

    if (op == 0x0F) { two_byte(opsz); if (!branched) s.rip = start + pos; return; }

    if ((op & 0xC0) == 0 && (op & 7) < 6) {  // ALU group
        const int aluop = (op >> 3) & 7, form = op & 7;
        if (form >= 4) {
            const int sz = (form == 4) ? 1 : opsz;
            const std::uint64_t imm = imm_of(sz == 8 ? 4 : sz);
            const std::uint64_t res = alu(aluop, rd_reg(0, sz), imm, sz);
            if (aluop != 7) wr_reg(0, sz, res);
        } else {
            const int sz = (form & 1) ? opsz : 1;
            const Rm rm = decode_rm(rg, 0);
            if (form < 2) {
                const std::uint64_t res = alu(aluop, rd_rm(rm, sz), rd_reg(rg, sz), sz);
                if (aluop != 7) wr_rm(rm, sz, res);
            } else {
                const std::uint64_t res = alu(aluop, rd_reg(rg, sz), rd_rm(rm, sz), sz);
                if (aluop != 7) wr_reg(rg, sz, res);
            }
        }
        s.rip = start + pos;
        return;
    }

    if (op >= 0x50 && op <= 0x57) { push(s.gpr[(op & 7) | (rb ? 8 : 0)]); s.rip = start + pos; return; }
    if (op >= 0x58 && op <= 0x5F) { const std::uint64_t v = pop(); s.gpr[(op & 7) | (rb ? 8 : 0)] = v; s.rip = start + pos; return; }
    if (op >= 0x70 && op <= 0x7F) {
        const std::int8_t rel = static_cast<std::int8_t>(u8());
        if (cond_true(op & 15, s.rflags)) { s.rip = start + pos + static_cast<std::int64_t>(rel); }
        else s.rip = start + pos;
        return;
    }
    if (op >= 0xB0 && op <= 0xB7) { wr_reg((op & 7) | (rb ? 8 : 0), 1, u8()); s.rip = start + pos; return; }
    if (op >= 0xB8 && op <= 0xBF) {
        const int r = (op & 7) | (rb ? 8 : 0);
        if (rw) wr_reg(r, 8, u64());
        else if (p66) wr_reg(r, 2, u16());
        else wr_reg(r, 4, u32());
        s.rip = start + pos;
        return;
    }
    if (op >= 0x91 && op <= 0x97) {
        const int r = (op & 7) | (rb ? 8 : 0);
        const std::uint64_t a = rd_reg(0, opsz), b = rd_reg(r, opsz);
        wr_reg(0, opsz, b); wr_reg(r, opsz, a);
        s.rip = start + pos;
        return;
    }

    switch (op) {
    case 0x63: {
        const Rm rm = decode_rm(rg, 0);
        const std::uint64_t v = rd_rm(rm, 4);
        if (rw) wr_reg(rg, 8, static_cast<std::uint64_t>(sext(v, 4))); else wr_reg(rg, opsz, v);
        break;
    }
    case 0x68: push(static_cast<std::uint64_t>(sext(u32(), 4))); break;
    case 0x6A: push(static_cast<std::uint64_t>(sext(u8(), 1))); break;
    case 0x69: case 0x6B: {
        const int ib = (op == 0x6B) ? 1 : (opsz == 2 ? 2 : 4);
        const Rm rm = decode_rm(rg, ib);
        const std::uint64_t imm = (op == 0x6B) ? static_cast<std::uint64_t>(sext(u8(), 1)) : imm_of(opsz == 2 ? 2 : 4);
        const std::uint64_t a = rd_rm(rm, opsz);
        bool ovf;
        std::uint64_t res;
        if (opsz == 8) {
            std::uint64_t hi, lo;
            const bool neg = (static_cast<std::int64_t>(a) < 0) != (static_cast<std::int64_t>(imm) < 0);
            mul64(static_cast<std::int64_t>(a) < 0 ? ~a + 1 : a, static_cast<std::int64_t>(imm) < 0 ? ~imm + 1 : imm, hi, lo);
            if (neg) neg128(hi, lo);
            res = lo;
            ovf = hi != (static_cast<std::int64_t>(lo) < 0 ? ~0ULL : 0ULL);
        } else {
            const std::int64_t p = sext(a, opsz) * sext(imm, opsz);
            res = static_cast<std::uint64_t>(p) & mask_of(opsz);
            ovf = sext(res, opsz) != p;
        }
        wr_reg(rg, opsz, res);
        set_flag(FLAG_CF, ovf); set_flag(FLAG_OF, ovf); set_szp(res, opsz);
        break;
    }
    case 0x80: case 0x81: case 0x83: {
        const int sz = (op == 0x80) ? 1 : opsz;
        const int ib = (op == 0x81) ? (opsz == 2 ? 2 : 4) : 1;
        const Rm rm = decode_rm(rg, ib);
        const std::uint64_t imm = (op == 0x81) ? imm_of(opsz == 2 ? 2 : 4) : static_cast<std::uint64_t>(sext(u8(), 1));
        const int aluop = rg & 7;
        const std::uint64_t res = alu(aluop, rd_rm(rm, sz), imm, sz);
        if (aluop != 7) wr_rm(rm, sz, res);
        break;
    }
    case 0x84: case 0x85: {
        const int sz = (op == 0x84) ? 1 : opsz;
        const Rm rm = decode_rm(rg, 0);
        alu(4, rd_rm(rm, sz), rd_reg(rg, sz), sz);
        break;
    }
    case 0x86: case 0x87: {
        const int sz = (op == 0x86) ? 1 : opsz;
        const Rm rm = decode_rm(rg, 0);
        const std::uint64_t a = rd_rm(rm, sz), b = rd_reg(rg, sz);
        wr_rm(rm, sz, b); wr_reg(rg, sz, a);
        break;
    }
    case 0x88: case 0x89: {
        const int sz = (op == 0x88) ? 1 : opsz;
        const Rm rm = decode_rm(rg, 0);
        wr_rm(rm, sz, rd_reg(rg, sz));
        break;
    }
    case 0x8A: case 0x8B: {
        const int sz = (op == 0x8A) ? 1 : opsz;
        const Rm rm = decode_rm(rg, 0);
        wr_reg(rg, sz, rd_rm(rm, sz));
        break;
    }
    case 0x8D: {
        const bool saved_fs = fs;
        fs = false;
        const Rm rm = decode_rm(rg, 0);
        fs = saved_fs;
        if (rm.reg) throw UdEx{};
        wr_reg(rg, opsz, rm.addr);
        break;
    }
    case 0x90: break;  // nop / pause
    case 0x98:
        if (rw) s.gpr[0] = static_cast<std::uint64_t>(sext(s.gpr[0], 4));
        else if (p66) wr_reg(0, 2, static_cast<std::uint64_t>(sext(s.gpr[0], 1)));
        else wr_reg(0, 4, static_cast<std::uint64_t>(sext(s.gpr[0], 2)));
        break;
    case 0x99:
        if (rw) s.gpr[2] = (static_cast<std::int64_t>(s.gpr[0]) < 0) ? ~0ULL : 0;
        else if (p66) wr_reg(2, 2, (s.gpr[0] & 0x8000) ? 0xFFFF : 0);
        else wr_reg(2, 4, (s.gpr[0] & 0x80000000ULL) ? 0xFFFFFFFFULL : 0);
        break;
    case 0xA8: alu(4, rd_reg(0, 1), u8(), 1); break;
    case 0xA9: { const std::uint64_t imm = imm_of(opsz == 2 ? 2 : 4); alu(4, rd_reg(0, opsz), imm, opsz); break; }
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: {
        const int sz = (op & 1) ? opsz : 1;
        const bool movs = (op == 0xA4 || op == 0xA5);
        const std::int64_t step = (s.rflags & FLAG_DF) ? -sz : sz;
        std::uint64_t count = (rep || repne) ? s.gpr[1] : 1;
        while (count) {
            if (movs) { wr_mem(s.gpr[7], sz, rd_mem(s.gpr[6], sz)); s.gpr[6] += static_cast<std::uint64_t>(step); }
            else wr_mem(s.gpr[7], sz, rd_reg(0, sz));
            s.gpr[7] += static_cast<std::uint64_t>(step);
            --count;
            if (rep || repne) s.gpr[1] = count;
        }
        break;
    }
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        const int sz = (op & 1) ? opsz : 1;
        const int ib = (op == 0xC0 || op == 0xC1) ? 1 : 0;
        const Rm rm = decode_rm(rg, ib);
        unsigned count = (op <= 0xC1) ? u8() : (op <= 0xD1 ? 1 : static_cast<unsigned>(s.gpr[1] & 0xFF));
        wr_rm(rm, sz, shift(rg & 7, rd_rm(rm, sz), count, sz));
        break;
    }
    case 0xC2: { const std::uint16_t extra = u16(); s.rip = pop(); s.gpr[4] += extra; return; }
    case 0xC3: s.rip = pop(); return;
    case 0xC6: case 0xC7: {
        const int sz = (op == 0xC6) ? 1 : opsz;
        const int ib = (op == 0xC6) ? 1 : (opsz == 2 ? 2 : 4);
        const Rm rm = decode_rm(rg, ib);
        if ((rg & 7) != 0) throw Unsupported{};
        const std::uint64_t imm = (op == 0xC6) ? u8() : imm_of(opsz == 2 ? 2 : 4);
        wr_rm(rm, sz, imm);
        break;
    }
    case 0xC9: { s.gpr[4] = s.gpr[5]; s.gpr[5] = pop(); break; }
    case 0xCC: throw HaltEx{};
    case 0xF4: throw HaltEx{};
    case 0xE8: {
        const std::int32_t rel = static_cast<std::int32_t>(u32());
        const std::uint64_t ret = start + pos;
        push(ret);
        s.rip = ret + static_cast<std::int64_t>(rel);
        return;
    }
    case 0xE9: { const std::int32_t rel = static_cast<std::int32_t>(u32()); s.rip = start + pos + static_cast<std::int64_t>(rel); return; }
    case 0xEB: { const std::int8_t rel = static_cast<std::int8_t>(u8()); s.rip = start + pos + static_cast<std::int64_t>(rel); return; }
    case 0xF6: case 0xF7: {
        const int sz = (op == 0xF6) ? 1 : opsz;
        const int sub = (pos < n) ? ((buf[pos] >> 3) & 7) : 0;
        const int ib = (sub < 2) ? (sz == 1 ? 1 : (sz == 2 ? 2 : 4)) : 0;
        const Rm rm = decode_rm(rg, ib);
        const std::uint64_t v = rd_rm(rm, sz);
        switch (rg & 7) {
        case 0: case 1: alu(4, v, imm_of(sz == 8 ? 4 : sz), sz); break;
        case 2: wr_rm(rm, sz, ~v & mask_of(sz)); break;
        case 3: {
            const std::uint64_t r = alu(5, 0, v, sz);
            wr_rm(rm, sz, r);
            set_flag(FLAG_CF, (v & mask_of(sz)) != 0);
            break;
        }
        case 4: case 5: {  // mul / imul (one operand)
            const bool sg = (rg & 7) == 5;
            std::uint64_t hi = 0, lo = 0;
            bool carry;
            if (sz == 8) {
                const std::uint64_t a = s.gpr[0];
                if (!sg) { mul64(a, v, hi, lo); carry = hi != 0; }
                else {
                    const bool neg = (static_cast<std::int64_t>(a) < 0) != (static_cast<std::int64_t>(v) < 0);
                    mul64(static_cast<std::int64_t>(a) < 0 ? ~a + 1 : a, static_cast<std::int64_t>(v) < 0 ? ~v + 1 : v, hi, lo);
                    if (neg) neg128(hi, lo);
                    carry = hi != (static_cast<std::int64_t>(lo) < 0 ? ~0ULL : 0ULL);
                }
                s.gpr[0] = lo; s.gpr[2] = hi;
            } else {
                const std::uint64_t a = rd_reg(0, sz);
                std::uint64_t full;
                if (!sg) { full = a * v; carry = (full >> (sz * 8)) != 0; }
                else {
                    const std::int64_t p = sext(a, sz) * sext(v, sz);
                    full = static_cast<std::uint64_t>(p);
                    carry = sext(full & mask_of(sz), sz) != p;
                }
                if (sz == 1) wr_reg(0, 2, full & 0xFFFF);
                else { wr_reg(0, sz, full & mask_of(sz)); wr_reg(2, sz, (full >> (sz * 8)) & mask_of(sz)); }
            }
            set_flag(FLAG_CF, carry); set_flag(FLAG_OF, carry);
            break;
        }
        default: {  // div / idiv
            const bool sg = (rg & 7) == 7;
            if ((v & mask_of(sz)) == 0) throw DivErr{};
            if (sz == 8) {
                std::uint64_t hi = s.gpr[2], lo = s.gpr[0], q, r;
                if (!sg) { if (!udiv128(hi, lo, v, q, r)) throw DivErr{}; }
                else {
                    const bool nd = static_cast<std::int64_t>(hi) < 0, nv = static_cast<std::int64_t>(v) < 0;
                    std::uint64_t dh = hi, dl = lo, dv = v;
                    if (nd) neg128(dh, dl);
                    if (nv) dv = ~dv + 1;
                    if (!udiv128(dh, dl, dv, q, r)) throw DivErr{};
                    if (nd != nv) { if (q > 0x8000000000000000ULL) throw DivErr{}; q = ~q + 1; }
                    else if (q > 0x7FFFFFFFFFFFFFFFULL) throw DivErr{};
                    if (nd) r = ~r + 1;
                }
                s.gpr[0] = q; s.gpr[2] = r;
            } else {
                const int dsz = (sz == 1) ? 2 : sz;
                std::uint64_t dividend = (sz == 1) ? (s.gpr[0] & 0xFFFF)
                                       : ((rd_reg(2, sz) << (sz * 8)) | rd_reg(0, sz));
                std::uint64_t q, r;
                if (!sg) {
                    q = dividend / v; r = dividend % v;
                    if (q > mask_of(sz)) throw DivErr{};
                } else {
                    const std::int64_t sd = sext(dividend, dsz), sv = sext(v, sz);
                    const std::int64_t sq = sd / sv, sr = sd % sv;
                    if (sq != sext(static_cast<std::uint64_t>(sq) & mask_of(sz), sz)) throw DivErr{};
                    q = static_cast<std::uint64_t>(sq); r = static_cast<std::uint64_t>(sr);
                }
                if (sz == 1) { wr_reg(0, 1, q); wr_reg(4, 1, r); }  // AL, AH (no REX for AH here)
                else { wr_reg(0, sz, q); wr_reg(2, sz, r); }
            }
        }
        }
        break;
    }
    case 0xFE: case 0xFF: {
        const int sz = (op == 0xFE) ? 1 : opsz;
        const int sub = (pos < n) ? ((buf[pos] >> 3) & 7) : 0;
        if (op == 0xFE && sub > 1) throw Unsupported{};
        if (sub == 2 || sub == 4 || sub == 6) {
            const Rm rm = decode_rm(rg, 0);
            const std::uint64_t v = rd_rm(rm, 8);
            if (sub == 2) { const std::uint64_t ret = start + pos; push(ret); s.rip = v; return; }
            if (sub == 4) { s.rip = v; return; }
            push(v);
            break;
        }
        if (sub > 1) throw Unsupported{};
        const Rm rm = decode_rm(rg, 0);
        wr_rm(rm, sz, incdec(rd_rm(rm, sz), sz, (rg & 7) == 1));
        break;
    }
    default: throw Unsupported{};
    }
    s.rip = start + pos;
}

} // namespace

const char* cpu_stop_name(CpuStop stop) {
    switch (stop) {
    case CpuStop::none: return "none";
    case CpuStop::step_limit: return "step_limit";
    case CpuStop::unsupported_opcode: return "unsupported_opcode";
    case CpuStop::memory_fault: return "memory_fault";
    case CpuStop::trap: return "trap";
    case CpuStop::halt: return "halt";
    case CpuStop::divide_error: return "divide_error";
    case CpuStop::undefined: return "undefined";
    }
    return "unknown";
}

std::string CpuRunResult::describe() const {
    std::ostringstream o;
    o << cpu_stop_name(stop) << " steps=" << steps << " rip=0x" << std::hex << rip;
    if (stop == CpuStop::memory_fault) o << " fault=0x" << fault_address;
    if (byte_count) {
        o << " bytes=";
        static const char* hex = "0123456789abcdef";
        for (std::size_t i = 0; i < byte_count; ++i) o << hex[bytes[i] >> 4] << hex[bytes[i] & 15];
    }
    return o.str();
}

void X86Interpreter::add_trap_range(std::uint64_t base, std::uint64_t size) {
    if (size) traps_.emplace_back(base, size);
}

bool X86Interpreter::in_trap(std::uint64_t rip) const {
    for (const auto& t : traps_)
        if (rip >= t.first && rip - t.first < t.second) return true;
    return false;
}

CpuRunResult X86Interpreter::run(std::uint64_t max_steps) {
    CpuRunResult out;
    while (out.steps < max_steps) {
        out.rip = state.rip;
        if (in_trap(state.rip)) { out.stop = CpuStop::trap; return out; }
        Exec e(state, mem_);
        e.start = state.rip;
        for (std::size_t len = 15; len > 0; --len) {
            if (mem_.load(e.start, e.buf, len) == MemoryError::ok) { e.n = len; break; }
        }
        auto capture = [&]() {
            out.byte_count = e.n < sizeof(out.bytes) ? e.n : sizeof(out.bytes);
            std::memcpy(out.bytes, e.buf, out.byte_count);
        };
        if (e.n == 0) { out.stop = CpuStop::memory_fault; out.fault_address = e.start; return out; }
        try {
            e.step();
            ++out.steps;
        } catch (const Fault& f) { out.stop = CpuStop::memory_fault; out.fault_address = f.addr; capture(); return out; }
        catch (const Unsupported&) { out.stop = CpuStop::unsupported_opcode; capture(); return out; }
        catch (const DivErr&) { out.stop = CpuStop::divide_error; capture(); return out; }
        catch (const HaltEx&) { out.stop = CpuStop::halt; capture(); return out; }
        catch (const UdEx&) { out.stop = CpuStop::undefined; capture(); return out; }
    }
    out.rip = state.rip;
    out.stop = CpuStop::step_limit;
    return out;
}

} // namespace bb
