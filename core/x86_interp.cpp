#include "x86_interp.h"
#include <cstring>
#include <cmath>
#include <limits>
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

template <typename T>
inline T round_fp(T val, unsigned imm8, std::uint32_t mxcsr = 0) {
    if (std::isnan(val)) return val;
    int rc = imm8 & 3;
    if (imm8 & 8) {
        rc = (mxcsr >> 13) & 3;
    }
    switch (rc) {
        case 0: return std::nearbyint(val);
        case 1: return std::floor(val);
        case 2: return std::ceil(val);
        case 3: return std::trunc(val);
        default: return std::nearbyint(val);
    }
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

static inline std::uint32_t bswap32(std::uint32_t v) {
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(v);
#else
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
#endif
}

static inline std::uint64_t bswap64(std::uint64_t v) {
#if defined(_MSC_VER)
    return _byteswap_uint64(v);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(v);
#else
    return (static_cast<std::uint64_t>(bswap32(static_cast<std::uint32_t>(v))) << 32) | bswap32(static_cast<std::uint32_t>(v >> 32));
#endif
}

struct Rm { bool reg; int r; std::uint64_t addr; };

struct Exec {
    CpuState& s;
    GuestMemory& m;
    std::uint8_t buf[16] = {};
    std::size_t n = 0, pos = 0;
    std::uint64_t start = 0;
    bool p66 = false, rep = false, repne = false, rex = false, rw = false, rr = false, rx = false,
         rb = false, fs = false, gs = false, addr32 = false;
    bool has_lock = false, lock_consumed = false;
    std::unique_lock<std::mutex> bus_lock_;
    bool branched = false;
    std::uint64_t& tsc;

    Exec(CpuState& st, GuestMemory& mem, std::uint64_t& tsc_ref) : s(st), m(mem), tsc(tsc_ref) {}

    void check_lock(bool allowed, const Rm* rm = nullptr);

    // ---- fetch ----
    std::uint8_t u8() { if (pos >= 15) throw UdEx{}; if (pos >= n) throw Fault{start + pos}; return buf[pos++]; }
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
        if (riprel && !addr32) addr += start + pos + static_cast<std::uint64_t>(imm_after);
        if (addr32) addr = static_cast<std::uint32_t>(addr);
        if (fs) addr += s.fs_base;
        if (gs) addr += s.gs_base;
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
            cf = a < b || (a == b && c != 0);
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
    template <typename T>
    static inline bool eval_cmp_pred(T a, T b, unsigned imm) {
        const bool nan_a = std::isnan(a);
        const bool nan_b = std::isnan(b);
        const bool unord = nan_a || nan_b;
        const unsigned pred = imm & 0x1F;
        switch (pred & 0x0F) {
        case 0: return !unord && (a == b);
        case 1: return !unord && (a < b);
        case 2: return !unord && (a <= b);
        case 3: return unord;
        case 4: return unord || (a != b);
        case 5: return unord || !(a < b);
        case 6: return unord || !(a <= b);
        case 7: return !unord;
        case 8: return unord || (a == b);
        case 9: return unord || !(a >= b);
        case 10: return unord || !(a > b);
        case 11: return false;
        case 12: return !unord && (a != b);
        case 13: return !unord && (a >= b);
        case 14: return !unord && (a > b);
        case 15: return true;
        default: return false;
        }
    }
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
    void avx(bool three);
    void bmi1();
    void two_byte(int opsz);
    void sse(std::uint8_t op2, int opsz);
};

static std::mutex s_bus_lock_mutex;

void Exec::check_lock(bool allowed, const Rm* rm) {
    if (!has_lock) return;
    if (!allowed || !rm || rm->reg) throw UdEx{};
    if (!bus_lock_.owns_lock()) {
        bus_lock_ = std::unique_lock<std::mutex>(s_bus_lock_mutex);
    }
    lock_consumed = true;
}

void Exec::avx(bool three) {
    const std::uint8_t v1 = u8();
    const std::uint8_t v2 = three ? u8() : v1;
    const std::uint8_t op = u8();
    const int map = three ? (v1 & 31) : 1;
    int pp = v2 & 3;
    const int len = (v2 & 4) ? 4 : 2;
    const int src1 = (~(v2 >> 3)) & 15;
    const bool w = three && ((v2 & 0x80) != 0);
    if ((map != 1 && map != 2 && map != 3) || p66 || rep || repne || rex) throw Unsupported{};
    rr = !(v1 & 0x80); rx = three && !(v1 & 0x40); rb = three && !(v1 & 0x20);
    if (map == 3) {
        // 0F 3A map
        int rg = 0;
        const Rm rm = decode_rm(rg, 1);
        const unsigned imm = u8();
        std::uint64_t s1[4] = {}, s2[4] = {}, dst[4] = {};
        for (int i = 0; i < len; ++i) s1[i] = i < 2 ? s.xmm[src1][i] : s.ymm_hi[src1][i - 2];
        const int s2_bytes = (op == 0x18) ? 16 : (op == 0x21 || op == 0x0A) ? 4 : (op == 0x0B) ? 8 : (len * 8);
        if (rm.reg) {
            for (int i = 0; i < len; ++i) s2[i] = i < 2 ? s.xmm[rm.r][i] : s.ymm_hi[rm.r][i - 2];
        } else {
            for (int i = 0; i < (s2_bytes + 7) / 8; ++i) s2[i] = rd_mem(rm.addr + i * 8, (s2_bytes < 8) ? s2_bytes : 8);
        }
        if (op == 0x0A) { // VROUNDSS xmm1, xmm2, xmm3/m32, imm8
            if (pp != 1 || len != 2) throw Unsupported{};
            float f = 0.0f;
            std::memcpy(&f, s2, 4);
            float r = round_fp(f, imm);
            std::uint32_t r_bits = 0;
            std::memcpy(&r_bits, &r, 4);
            dst[0] = (s1[0] & 0xFFFFFFFF00000000ULL) | r_bits;
            dst[1] = s1[1];
        } else if (op == 0x0B) { // VROUNDSD xmm1, xmm2, xmm3/m64, imm8
            if (pp != 1 || len != 2) throw Unsupported{};
            double d = 0.0;
            std::memcpy(&d, s2, 8);
            double r = round_fp(d, imm);
            std::uint64_t r_bits = 0;
            std::memcpy(&r_bits, &r, 8);
            dst[0] = r_bits;
            dst[1] = s1[1];
        } else if (op == 0x08) { // VROUNDPS xmm1/ymm1, xmm2/ymm2/m, imm8
            if (pp != 1) throw Unsupported{};
            const int total_floats = len * 2;
            float src_f[8], dst_f[8];
            std::memcpy(src_f, s2, len * 8);
            for (int i = 0; i < total_floats; ++i) {
                dst_f[i] = round_fp(src_f[i], imm);
            }
            std::memcpy(dst, dst_f, len * 8);
        } else if (op == 0x09) { // VROUNDPD xmm1/ymm1, xmm2/ymm2/m, imm8
            if (pp != 1) throw Unsupported{};
            const int total_doubles = len;
            double src_d[4], dst_d[4];
            std::memcpy(src_d, s2, len * 8);
            for (int i = 0; i < total_doubles; ++i) {
                dst_d[i] = round_fp(src_d[i], imm);
            }
            std::memcpy(dst, dst_d, len * 8);
        } else if (op == 0x40) { // VDPPS xmm1/ymm1, xmm2/ymm2, xmm3/ymm3/m, imm8
            if (pp != 1) throw Unsupported{};
            const int lanes = len / 2; // 1 for 128-bit, 2 for 256-bit
            float src1_f[8], src2_f[8], dst_f[8] = {};
            std::memcpy(src1_f, s1, len * 8);
            std::memcpy(src2_f, s2, len * 8);
            for (int l = 0; l < lanes; ++l) {
                const int base = l * 4;
                float sum = 0.0f;
                if (imm & 0x10) sum += src1_f[base + 0] * src2_f[base + 0];
                if (imm & 0x20) sum += src1_f[base + 1] * src2_f[base + 1];
                if (imm & 0x40) sum += src1_f[base + 2] * src2_f[base + 2];
                if (imm & 0x80) sum += src1_f[base + 3] * src2_f[base + 3];

                dst_f[base + 0] = (imm & 0x01) ? sum : 0.0f;
                dst_f[base + 1] = (imm & 0x02) ? sum : 0.0f;
                dst_f[base + 2] = (imm & 0x04) ? sum : 0.0f;
                dst_f[base + 3] = (imm & 0x08) ? sum : 0.0f;
            }
            std::memcpy(dst, dst_f, len * 8);
        } else if (op == 0x41) { // VDPPD xmm1, xmm2, xmm3/m128, imm8
            if (pp != 1 || len != 2) throw Unsupported{};
            double src1_d[2], src2_d[2], dst_d[2] = {};
            std::memcpy(src1_d, s1, 16);
            std::memcpy(src2_d, s2, 16);
            double sum = 0.0;
            if (imm & 0x10) sum += src1_d[0] * src2_d[0];
            if (imm & 0x20) sum += src1_d[1] * src2_d[1];
            dst_d[0] = (imm & 0x01) ? sum : 0.0;
            dst_d[1] = (imm & 0x02) ? sum : 0.0;
            std::memcpy(dst, dst_d, 16);
        } else if (op == 0x4A || op == 0x4B || op == 0x4C) { // VBLENDVPS / VBLENDVPD / VPBLENDVB
            if (pp != 1) throw Unsupported{};
            const int mask_reg = (imm >> 4) & 15;
            std::uint64_t mask[4] = {};
            for (int i = 0; i < len; ++i) mask[i] = i < 2 ? s.xmm[mask_reg][i] : s.ymm_hi[mask_reg][i - 2];
            const int elem_width = (op == 0x4B) ? 8 : (op == 0x4A) ? 4 : 1;
            const int total_bytes = len * 8;
            std::uint8_t a[32], b[32], mask_bytes[32], out[32];
            std::memcpy(a, s1, total_bytes);
            std::memcpy(b, s2, total_bytes);
            std::memcpy(mask_bytes, mask, total_bytes);
            for (int i = 0; i < total_bytes; i += elem_width) {
                const bool select_b = (mask_bytes[i + elem_width - 1] & 0x80) != 0;
                std::memcpy(out + i, select_b ? (b + i) : (a + i), elem_width);
            }
            std::memcpy(dst, out, total_bytes);
        } else if (op == 0x0C || op == 0x0D || op == 0x0E) { // VBLENDPS / VBLENDPD / VPBLENDW
            if (pp != 1) throw Unsupported{};
            const int elem_width = (op == 0x0D) ? 8 : (op == 0x0C) ? 4 : 2;
            const int total_bytes = len * 8;
            std::uint8_t a[32], b[32], out[32];
            std::memcpy(a, s1, total_bytes);
            std::memcpy(b, s2, total_bytes);
            const int num_elems = total_bytes / elem_width;
            for (int i = 0; i < num_elems; ++i) {
                const bool select_b = ((imm >> i) & 1) != 0;
                std::memcpy(out + i * elem_width, select_b ? (b + i * elem_width) : (a + i * elem_width), elem_width);
            }
            std::memcpy(dst, out, total_bytes);
        } else if (op == 0x0F) { // VPALIGNR
            if (pp != 1) throw Unsupported{};
            std::uint8_t a[32], b[32], out[32];
            std::memcpy(a, s1, len * 8);
            std::memcpy(b, s2, len * 8);
            for (int lane = 0; lane < len / 2; ++lane) {
                const std::uint8_t* lane_a = a + lane * 16;
                const std::uint8_t* lane_b = b + lane * 16;
                std::uint8_t* lane_out = out + lane * 16;
                for (unsigned i = 0; i < 16; ++i) {
                    const unsigned j = imm + i;
                    lane_out[i] = j < 16 ? lane_b[j] : j < 32 ? lane_a[j - 16] : 0;
                }
            }
            std::memcpy(dst, out, len * 8);
        } else if (op == 0x18) { // VINSERTF128 ymm, ymm, xmm/m128, imm8
            if (pp != 1 || len != 4) throw Unsupported{};
            if ((imm & 1) == 0) {
                dst[0] = s2[0]; dst[1] = s2[1];
                dst[2] = s1[2]; dst[3] = s1[3];
            } else {
                dst[0] = s1[0]; dst[1] = s1[1];
                dst[2] = s2[0]; dst[3] = s2[1];
            }
        } else if (op == 0x19) { // VEXTRACTF128 xmm/m128, ymm, imm8
            if (pp != 1 || len != 4) throw Unsupported{};
            const int half = (imm & 1) ? 2 : 0;
            if (rm.reg) {
                s.xmm[rm.r][0] = s1[half + 0];
                s.xmm[rm.r][1] = s1[half + 1];
                s.ymm_hi[rm.r][0] = 0;
                s.ymm_hi[rm.r][1] = 0;
            } else {
                wr_mem(rm.addr + 0, 8, s1[half + 0]);
                wr_mem(rm.addr + 8, 8, s1[half + 1]);
            }
            return;
        } else if (op == 0x21) { // VINSERTPS xmm1, xmm2, xmm3/m32, imm8
            if (pp != 1 || len != 2) throw Unsupported{};
            const int src_idx = (imm >> 6) & 3;
            const int dst_idx = (imm >> 4) & 3;
            const unsigned zmask = imm & 15;
            std::uint32_t val = 0;
            if (rm.reg) {
                std::uint32_t words[4];
                std::memcpy(words, s2, 16);
                val = words[src_idx];
            } else {
                val = static_cast<std::uint32_t>(rd_mem(rm.addr, 4));
            }
            std::uint32_t words[4];
            std::memcpy(words, s1, 16);
            words[dst_idx] = val;
            for (int i = 0; i < 4; ++i) {
                if ((zmask >> i) & 1) words[i] = 0;
            }
            std::memcpy(dst, words, 16);
        } else if (op == 0x14 || op == 0x15 || op == 0x16 || op == 0x17) { // VPEXTRB / VPEXTRW / VPEXTRD / VPEXTRQ / VEXTRACTPS
            if (pp != 1 || len != 2) throw Unsupported{};
            if (op == 0x16 || op == 0x17) {
                if (w && op == 0x16) { // VPEXTRQ
                    const unsigned idx = imm & 1;
                    const std::uint64_t val = s.xmm[rg][idx];
                    if (rm.reg) wr_reg(rm.r, 8, val);
                    else wr_mem(rm.addr, 8, val);
                } else { // VPEXTRD / VEXTRACTPS
                    const unsigned idx = imm & 3;
                    const std::uint32_t val = static_cast<std::uint32_t>((s.xmm[rg][idx / 2] >> ((idx % 2) * 32)) & 0xFFFFFFFFULL);
                    if (rm.reg) wr_reg(rm.r, 4, val);
                    else wr_mem(rm.addr, 4, val);
                }
            } else if (op == 0x15) { // VPEXTRW
                const unsigned idx = imm & 7;
                const std::uint16_t val = static_cast<std::uint16_t>((s.xmm[rg][idx / 4] >> ((idx % 4) * 16)) & 0xFFFF);
                if (rm.reg) wr_reg(rm.r, 4, val);
                else wr_mem(rm.addr, 2, val);
            } else { // VPEXTRB (op == 0x14)
                const unsigned idx = imm & 15;
                const std::uint8_t val = static_cast<std::uint8_t>((s.xmm[rg][idx / 8] >> ((idx % 8) * 8)) & 0xFF);
                if (rm.reg) wr_reg(rm.r, 4, val);
                else wr_mem(rm.addr, 1, val);
            }
            return;
        } else if (op == 0x20 || op == 0x22) { // VPINSRB / VPINSRD / VPINSRQ
            if (pp != 1 || len != 2) throw Unsupported{};
            std::uint64_t val = 0;
            if (op == 0x22) {
                const int sz = w ? 8 : 4;
                val = rm.reg ? rd_reg(rm.r, sz) : rd_mem(rm.addr, sz);
                for (int i = 0; i < 2; ++i) dst[i] = s1[i];
                if (w) {
                    const unsigned idx = imm & 1;
                    dst[idx] = val;
                } else {
                    const unsigned idx = imm & 3;
                    const int word = idx / 2, shift = (idx % 2) * 32;
                    dst[word] = (dst[word] & ~(0xFFFFFFFFULL << shift)) | ((val & 0xFFFFFFFFULL) << shift);
                }
            } else { // VPINSRB (op == 0x20)
                val = rm.reg ? (rd_reg(rm.r, 4) & 0xFF) : rd_mem(rm.addr, 1);
                for (int i = 0; i < 2; ++i) dst[i] = s1[i];
                const unsigned idx = imm & 15;
                const int word = idx / 8, shift = (idx % 8) * 8;
                dst[word] = (dst[word] & ~(0xFFULL << shift)) | ((val & 0xFFULL) << shift);
            }
        } else {
            throw Unsupported{};
        }
        for (int i = 0; i < 2; ++i) s.xmm[rg][i] = dst[i];
        for (int i = 0; i < 2; ++i) s.ymm_hi[rg][i] = (len == 4) ? dst[i + 2] : 0;
        return;
    }
    if (map == 2) {
        // 0F 38 map
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        std::uint64_t s1[4] = {}, s2[4] = {}, dst[4] = {};
        for (int i = 0; i < len; ++i) s1[i] = i < 2 ? s.xmm[src1][i] : s.ymm_hi[src1][i - 2];
        const int s2_bytes = (op == 0x18 || op == 0x58) ? 4 : (op == 0x19 || op == 0x59) ? 8 : (op == 0x1A) ? 16 : (len * 8);
        if (rm.reg) {
            for (int i = 0; i < len; ++i) s2[i] = i < 2 ? s.xmm[rm.r][i] : s.ymm_hi[rm.r][i - 2];
        } else {
            for (int i = 0; i < (s2_bytes + 7) / 8; ++i) {
                const int chunk = (s2_bytes - i * 8 < 8) ? (s2_bytes - i * 8) : 8;
                s2[i] = rd_mem(rm.addr + i * 8, chunk);
            }
        }
        if (op == 0x18 || op == 0x19 || op == 0x1A) { // VBROADCASTSS / VBROADCASTSD / VBROADCASTF128
            if (pp != 1 || src1 != 0) throw Unsupported{};
            if (op == 0x18) { // VBROADCASTSS xmm/ymm, m32
                std::uint64_t val = 0;
                if (rm.reg) val = s.xmm[rm.r][0] & 0xFFFFFFFFULL;
                else val = s2[0] & 0xFFFFFFFFULL;
                const std::uint64_t dword = val | (val << 32);
                for (int i = 0; i < len; ++i) dst[i] = dword;
            } else if (op == 0x19) { // VBROADCASTSD ymm, m64
                if (len != 4) throw Unsupported{};
                std::uint64_t val = rm.reg ? s.xmm[rm.r][0] : s2[0];
                for (int i = 0; i < len; ++i) dst[i] = val;
            } else if (op == 0x1A) { // VBROADCASTF128 ymm, m128
                if (len != 4) throw Unsupported{};
                std::uint64_t val0 = rm.reg ? s.xmm[rm.r][0] : s2[0];
                std::uint64_t val1 = rm.reg ? s.xmm[rm.r][1] : s2[1];
                dst[0] = val0; dst[1] = val1;
                dst[2] = val0; dst[3] = val1;
            }
        } else if (op == 0x00) { // VPSHUFB
            if (pp != 1) throw Unsupported{};
            std::uint8_t a[32], b[32], out[32];
            std::memcpy(a, s1, len * 8);
            std::memcpy(b, s2, len * 8);
            for (int lane = 0; lane < len / 2; ++lane) {
                for (int i = 0; i < 16; ++i) {
                    const std::uint8_t ctrl = b[lane * 16 + i];
                    out[lane * 16 + i] = (ctrl & 0x80) ? 0 : a[lane * 16 + (ctrl & 15)];
                }
            }
            std::memcpy(dst, out, len * 8);
        } else if (op == 0x01 || op == 0x02 || op == 0x03) { // VPHADDW / VPHADDD / VPHADDSW
            if (pp != 1) throw Unsupported{};
            for (int lane = 0; lane < len / 2; ++lane) {
                if (op == 0x02) { // VPHADDD
                    std::uint32_t a[4], b[4], out[4];
                    std::memcpy(a, &s1[lane * 2], 16);
                    std::memcpy(b, &s2[lane * 2], 16);
                    out[0] = a[0] + a[1];
                    out[1] = a[2] + a[3];
                    out[2] = b[0] + b[1];
                    out[3] = b[2] + b[3];
                    std::memcpy(&dst[lane * 2], out, 16);
                } else if (op == 0x01) { // VPHADDW
                    std::uint16_t a[8], b[8], out[8];
                    std::memcpy(a, &s1[lane * 2], 16);
                    std::memcpy(b, &s2[lane * 2], 16);
                    for (int i = 0; i < 4; ++i) out[i] = a[i * 2] + a[i * 2 + 1];
                    for (int i = 0; i < 4; ++i) out[4 + i] = b[i * 2] + b[i * 2 + 1];
                    std::memcpy(&dst[lane * 2], out, 16);
                } else { // VPHADDSW
                    std::int16_t a[8], b[8], out[8];
                    std::memcpy(a, &s1[lane * 2], 16);
                    std::memcpy(b, &s2[lane * 2], 16);
                    auto sat16 = [](std::int32_t v) -> std::int16_t {
                        return static_cast<std::int16_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
                    };
                    for (int i = 0; i < 4; ++i) out[i] = sat16(static_cast<std::int32_t>(a[i * 2]) + a[i * 2 + 1]);
                    for (int i = 0; i < 4; ++i) out[4 + i] = sat16(static_cast<std::int32_t>(b[i * 2]) + b[i * 2 + 1]);
                    std::memcpy(&dst[lane * 2], out, 16);
                }
            }
        } else if (op == 0x40) { // VPMULLD
            if (pp != 1) throw Unsupported{};
            for (int i = 0; i < len * 2; ++i) {
                const std::uint32_t a = static_cast<std::uint32_t>(s1[i / 2] >> ((i % 2) * 32));
                const std::uint32_t b = static_cast<std::uint32_t>(s2[i / 2] >> ((i % 2) * 32));
                const std::uint32_t c = a * b;
                const int word = i / 2, shift = (i % 2) * 32;
                dst[word] = (dst[word] & ~(0xFFFFFFFFULL << shift)) | (static_cast<std::uint64_t>(c) << shift);
            }
        } else if (op == 0x58 || op == 0x59) { // VPBROADCASTD / VPBROADCASTQ
            if (pp != 1 || src1 != 0) throw Unsupported{};
            const int width = (op == 0x59) ? 8 : 4;
            std::uint64_t val = 0;
            if (rm.reg) {
                val = s.xmm[rm.r][0] & mask_of(width);
            } else {
                val = rd_mem(rm.addr, width);
            }
            if (width == 8) {
                for (int i = 0; i < len; ++i) dst[i] = val;
            } else {
                const std::uint64_t dword = val | (val << 32);
                for (int i = 0; i < len; ++i) dst[i] = dword;
            }
        } else if (op >= 0x38 && op <= 0x3F) {
            // VPMINSB(0x38), VPMINSD(0x39), VPMINUW(0x3A), VPMINUD(0x3B),
            // VPMAXSB(0x3C), VPMAXSD(0x3D), VPMAXUW(0x3E), VPMAXUD(0x3F)
            if (pp != 1) throw Unsupported{};
            const bool is_max = (op >= 0x3C);
            const int elem_width = (op == 0x38 || op == 0x3C) ? 1
                                 : (op == 0x3A || op == 0x3E) ? 2 : 4;
            const bool is_signed = (op == 0x38 || op == 0x39 || op == 0x3C || op == 0x3D);
            std::uint8_t a[32], b[32], out[32];
            std::memcpy(a, s1, len * 8);
            std::memcpy(b, s2, len * 8);
            for (int i = 0; i < len * 8; i += elem_width) {
                if (elem_width == 1) {
                    if (is_signed) {
                        std::int8_t x = static_cast<std::int8_t>(a[i]), y = static_cast<std::int8_t>(b[i]);
                        out[i] = static_cast<std::uint8_t>(is_max ? std::max(x, y) : std::min(x, y));
                    } else {
                        std::uint8_t x = a[i], y = b[i];
                        out[i] = is_max ? std::max(x, y) : std::min(x, y);
                    }
                } else if (elem_width == 2) {
                    std::uint16_t x16, y16;
                    std::memcpy(&x16, a + i, 2);
                    std::memcpy(&y16, b + i, 2);
                    if (is_signed) {
                        std::int16_t x = static_cast<std::int16_t>(x16), y = static_cast<std::int16_t>(y16);
                        std::int16_t z = is_max ? std::max(x, y) : std::min(x, y);
                        std::memcpy(out + i, &z, 2);
                    } else {
                        std::uint16_t z = is_max ? std::max(x16, y16) : std::min(x16, y16);
                        std::memcpy(out + i, &z, 2);
                    }
                } else { // elem_width == 4
                    std::uint32_t x32, y32;
                    std::memcpy(&x32, a + i, 4);
                    std::memcpy(&y32, b + i, 4);
                    if (is_signed) {
                        std::int32_t x = static_cast<std::int32_t>(x32), y = static_cast<std::int32_t>(y32);
                        std::int32_t z = is_max ? std::max(x, y) : std::min(x, y);
                        std::memcpy(out + i, &z, 4);
                    } else {
                        std::uint32_t z = is_max ? std::max(x32, y32) : std::min(x32, y32);
                        std::memcpy(out + i, &z, 4);
                    }
                }
            }
            std::memcpy(dst, out, len * 8);
        } else if (op >= 0x1C && op <= 0x1E) { // VPABSB / VPABSW / VPABSD
            if (pp != 1 || src1 != 0) throw Unsupported{};
            const int elem_width = (op == 0x1C) ? 1 : (op == 0x1D) ? 2 : 4;
            std::uint8_t b[32], out[32];
            std::memcpy(b, s2, len * 8);
            for (int i = 0; i < len * 8; i += elem_width) {
                if (elem_width == 1) {
                    std::int8_t v = static_cast<std::int8_t>(b[i]);
                    out[i] = static_cast<std::uint8_t>(v < 0 ? -v : v);
                } else if (elem_width == 2) {
                    std::int16_t v; std::memcpy(&v, b + i, 2);
                    v = static_cast<std::int16_t>(v < 0 ? -v : v);
                    std::memcpy(out + i, &v, 2);
                } else {
                    std::int32_t v; std::memcpy(&v, b + i, 4);
                    v = v < 0 ? -v : v;
                    std::memcpy(out + i, &v, 4);
                }
            }
            std::memcpy(dst, out, len * 8);
        } else if (op == 0x29 || op == 0x37) { // VPCMPEQQ (0x29) / VPCMPGTQ (0x37)
            if (pp != 1) throw Unsupported{};
            for (int i = 0; i < len; ++i) {
                if (op == 0x29) {
                    dst[i] = (s1[i] == s2[i]) ? ~0ULL : 0ULL;
                } else {
                    const std::int64_t a = static_cast<std::int64_t>(s1[i]);
                    const std::int64_t b = static_cast<std::int64_t>(s2[i]);
                    dst[i] = (a > b) ? ~0ULL : 0ULL;
                }
            }
        } else if (op == 0x28) { // VPMULDQ
            if (pp != 1) throw Unsupported{};
            for (int i = 0; i < len; ++i) {
                std::int32_t a = static_cast<std::int32_t>(s1[i] & 0xFFFFFFFFULL);
                std::int32_t b = static_cast<std::int32_t>(s2[i] & 0xFFFFFFFFULL);
                dst[i] = static_cast<std::uint64_t>(static_cast<std::int64_t>(a) * static_cast<std::int64_t>(b));
            }
        } else if (op == 0x2B) { // VPACKUSDW
            if (pp != 1) throw Unsupported{};
            for (int lane = 0; lane < len / 2; ++lane) {
                std::uint16_t out16[8];
                for (int i = 0; i < 4; ++i) {
                    std::int32_t v = static_cast<std::int32_t>(s1[lane * 2 + i / 2] >> ((i % 2) * 32));
                    out16[i] = static_cast<std::uint16_t>(v < 0 ? 0 : v > 65535 ? 65535 : v);
                }
                for (int i = 0; i < 4; ++i) {
                    std::int32_t v = static_cast<std::int32_t>(s2[lane * 2 + i / 2] >> ((i % 2) * 32));
                    out16[4 + i] = static_cast<std::uint16_t>(v < 0 ? 0 : v > 65535 ? 65535 : v);
                }
                std::memcpy(&dst[lane * 2], out16, 16);
            }
        } else if (op == 0x2A) { // VMOVNTDQA
            if (pp != 1 || src1 != 0) throw Unsupported{};
            for (int i = 0; i < len; ++i) dst[i] = s2[i];
        } else if ((op >= 0x20 && op <= 0x25) || (op >= 0x30 && op <= 0x35)) {
            if (pp != 1 || src1 != 0) throw Unsupported{};
            const bool is_signed = (op >= 0x20 && op <= 0x25);
            const int sub = is_signed ? (op - 0x20) : (op - 0x30);
            const int src_elem_bytes = (sub == 0 || sub == 1 || sub == 2) ? 1 : (sub == 3 || sub == 4) ? 2 : 4;
            const int dst_elem_bytes = (sub == 0) ? 2 : (sub == 1 || sub == 3) ? 4 : 8;
            const int total_elems = (len * 8) / dst_elem_bytes;
            const int total_src_bytes = total_elems * src_elem_bytes;
            std::uint8_t src_raw[32] = {};
            if (rm.reg) {
                std::memcpy(src_raw, s.xmm[rm.r], 16);
            } else {
                for (int b = 0; b < total_src_bytes; ++b) {
                    src_raw[b] = static_cast<std::uint8_t>(rd_mem(rm.addr + b, 1));
                }
            }
            std::uint8_t out[32] = {};
            for (int i = 0; i < total_elems; ++i) {
                std::uint64_t v = 0;
                std::memcpy(&v, src_raw + i * src_elem_bytes, src_elem_bytes);
                if (is_signed) {
                    v = sext(v, src_elem_bytes);
                }
                std::memcpy(out + i * dst_elem_bytes, &v, dst_elem_bytes);
            }
            std::memcpy(dst, out, len * 8);
        } else {
            throw Unsupported{};
        }
        for (int i = 0; i < 2; ++i) s.xmm[rg][i] = dst[i];
        for (int i = 0; i < 2; ++i) s.ymm_hi[rg][i] = (len == 4) ? dst[i + 2] : 0;
        return;
    }
    if (op == 0x77 && pp == 0 && len == 2 && src1 == 0 && !w) {
        for (int i = 0; i < 16; ++i) s.ymm_hi[i][0] = s.ymm_hi[i][1] = 0;
        return;
    }
    if ((op == 0x71 || op == 0x72 || op == 0x73) && pp == 1) {
        // VPSLLW / VPSRLW / VPSRAW (0x71)
        // VPSLLD / VPSRLD / VPSRAD (0x72)
        // VPSLLQ / VPSRLQ / VPSLLDQ / VPSRLDQ (0x73)
        int rg = 0;
        const Rm rm = decode_rm(rg, 1);
        const int sub = rg & 7;
        const unsigned count = u8();
        std::uint64_t source[4] = {}, dest[4] = {};
        if (rm.reg) {
            for (int i = 0; i < len; ++i) source[i] = i < 2 ? s.xmm[rm.r][i] : s.ymm_hi[rm.r][i - 2];
        } else {
            for (int i = 0; i < len; ++i) source[i] = rd_mem(rm.addr + i * 8, 8);
        }
        if (op == 0x71) {
            std::uint16_t words[16];
            std::memcpy(words, source, len * 8);
            for (int i = 0; i < len * 4; ++i) {
                if (sub == 2) words[i] = (count >= 16) ? 0 : (words[i] >> count);
                else if (sub == 4) {
                    std::int16_t sw = static_cast<std::int16_t>(words[i]);
                    words[i] = static_cast<std::uint16_t>(count >= 16 ? (sw < 0 ? -1 : 0) : (sw >> count));
                }
                else if (sub == 6) words[i] = (count >= 16) ? 0 : (words[i] << count);
                else throw Unsupported{};
            }
            std::memcpy(dest, words, len * 8);
        } else if (op == 0x72) {
            std::uint32_t dwords[8];
            std::memcpy(dwords, source, len * 8);
            for (int i = 0; i < len * 2; ++i) {
                if (sub == 2) dwords[i] = (count >= 32) ? 0 : (dwords[i] >> count);
                else if (sub == 4) {
                    std::int32_t sd = static_cast<std::int32_t>(dwords[i]);
                    dwords[i] = static_cast<std::uint32_t>(count >= 32 ? (sd < 0 ? -1 : 0) : (sd >> count));
                }
                else if (sub == 6) dwords[i] = (count >= 32) ? 0 : (dwords[i] << count);
                else throw Unsupported{};
            }
            std::memcpy(dest, dwords, len * 8);
        } else if (op == 0x73) {
            if (sub == 2) {
                for (int i = 0; i < len; ++i) dest[i] = (count >= 64) ? 0 : (source[i] >> count);
            } else if (sub == 6) {
                for (int i = 0; i < len; ++i) dest[i] = (count >= 64) ? 0 : (source[i] << count);
            } else if (sub == 3) {
                std::uint8_t b_in[32] = {}, b_out[32] = {};
                std::memcpy(b_in, source, len * 8);
                for (int lane = 0; lane < len / 2; ++lane) {
                    const std::uint8_t* in_l = b_in + lane * 16;
                    std::uint8_t* out_l = b_out + lane * 16;
                    for (unsigned b = 0; b < 16; ++b) out_l[b] = (b + count < 16) ? in_l[b + count] : 0;
                }
                std::memcpy(dest, b_out, len * 8);
            } else if (sub == 7) {
                std::uint8_t b_in[32] = {}, b_out[32] = {};
                std::memcpy(b_in, source, len * 8);
                for (int lane = 0; lane < len / 2; ++lane) {
                    const std::uint8_t* in_l = b_in + lane * 16;
                    std::uint8_t* out_l = b_out + lane * 16;
                    for (unsigned b = 0; b < 16; ++b) out_l[b] = (b >= count) ? in_l[b - count] : 0;
                }
                std::memcpy(dest, b_out, len * 8);
            } else throw Unsupported{};
        }
        for (int i = 0; i < 2; ++i) s.xmm[src1][i] = dest[i];
        for (int i = 0; i < 2; ++i) s.ymm_hi[src1][i] = (len == 4) ? dest[i + 2] : 0;
        return;
    }
    if (((op >= 0xD1 && op <= 0xD3) || (op >= 0xE1 && op <= 0xE2) || (op >= 0xF1 && op <= 0xF3)) && pp == 1) {
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        std::uint64_t count_src[4] = {};
        if (rm.reg) { for (int i = 0; i < len; ++i) count_src[i] = i < 2 ? s.xmm[rm.r][i] : s.ymm_hi[rm.r][i - 2]; }
        else { for (int i = 0; i < len; ++i) count_src[i] = rd_mem(rm.addr + i * 8, 8); }
        const std::uint64_t count = count_src[0];

        std::uint64_t source[4] = {}, dest[4] = {};
        for (int i = 0; i < len; ++i) source[i] = i < 2 ? s.xmm[src1][i] : s.ymm_hi[src1][i - 2];

        if (op == 0xD1 || op == 0xE1 || op == 0xF1) {
            std::uint16_t words[16];
            std::memcpy(words, source, len * 8);
            for (int i = 0; i < len * 4; ++i) {
                if (op == 0xD1) words[i] = (count >= 16) ? 0 : (words[i] >> count);
                else if (op == 0xE1) {
                    std::int16_t sw = static_cast<std::int16_t>(words[i]);
                    words[i] = static_cast<std::uint16_t>(count >= 16 ? (sw < 0 ? -1 : 0) : (sw >> count));
                } else words[i] = (count >= 16) ? 0 : (words[i] << count);
            }
            std::memcpy(dest, words, len * 8);
        } else if (op == 0xD2 || op == 0xE2 || op == 0xF2) {
            std::uint32_t dwords[8];
            std::memcpy(dwords, source, len * 8);
            for (int i = 0; i < len * 2; ++i) {
                if (op == 0xD2) dwords[i] = (count >= 32) ? 0 : (dwords[i] >> count);
                else if (op == 0xE2) {
                    std::int32_t sd = static_cast<std::int32_t>(dwords[i]);
                    dwords[i] = static_cast<std::uint32_t>(count >= 32 ? (sd < 0 ? -1 : 0) : (sd >> count));
                } else dwords[i] = (count >= 32) ? 0 : (dwords[i] << count);
            }
            std::memcpy(dest, dwords, len * 8);
        } else if (op == 0xD3 || op == 0xF3) {
            for (int i = 0; i < len; ++i) {
                if (op == 0xD3) dest[i] = (count >= 64) ? 0 : (source[i] >> count);
                else dest[i] = (count >= 64) ? 0 : (source[i] << count);
            }
        }
        for (int i = 0; i < 2; ++i) s.xmm[rg][i] = dest[i];
        for (int i = 0; i < 2; ++i) s.ymm_hi[rg][i] = (len == 4) ? dest[i + 2] : 0;
        return;
    }
    if (op == 0x50 && (pp == 0 || pp == 1)) { // vmovmskps / vmovmskpd
        if (src1 != 0) throw Unsupported{};
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        if (!rm.reg) throw UdEx{};
        unsigned mask = 0;
        if (pp == 0) { // vmovmskps
            const int total_floats = len * 2;
            for (int i = 0; i < total_floats; ++i) {
                const std::uint64_t qw = (i < 4) ? s.xmm[rm.r][i / 2] : s.ymm_hi[rm.r][(i - 4) / 2];
                const unsigned sign_bit = static_cast<unsigned>((qw >> ((i % 2) * 32 + 31)) & 1);
                mask |= (sign_bit << i);
            }
        } else { // vmovmskpd
            const int total_doubles = len;
            for (int i = 0; i < total_doubles; ++i) {
                const std::uint64_t qw = (i < 2) ? s.xmm[rm.r][i] : s.ymm_hi[rm.r][i - 2];
                const unsigned sign_bit = static_cast<unsigned>((qw >> 63) & 1);
                mask |= (sign_bit << i);
            }
        }
        wr_reg(rg, 4, mask);
        return;
    }
    if (op == 0xD7 && pp == 1) { // vpmovmskb
        if (src1 != 0) throw Unsupported{};
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        if (!rm.reg) throw UdEx{};
        unsigned mask = 0;
        const int total_bytes = len * 8;
        for (int i = 0; i < total_bytes; ++i) {
            const std::uint64_t qw = (i < 16) ? s.xmm[rm.r][i / 8] : s.ymm_hi[rm.r][(i - 16) / 8];
            const unsigned sign_bit = static_cast<unsigned>((qw >> ((i % 8) * 8 + 7)) & 1);
            mask |= (sign_bit << i);
        }
        wr_reg(rg, 4, mask);
        return;
    }
    if (op == 0xAE && pp == 0 && len == 2 && src1 == 0) { // vstmxcsr / vldmxcsr
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        if (rm.reg) throw UdEx{};
        const int sub = rg & 7;
        if (sub == 2) {
            // VLDMXCSR m32
            s.mxcsr = static_cast<std::uint32_t>(rd_mem(rm.addr, 4));
            return;
        } else if (sub == 3) {
            // VSTMXCSR m32
            wr_mem(rm.addr, 4, s.mxcsr);
            return;
        }
        throw Unsupported{};
    }
    if (op == 0x6E) { // vmovd / vmovq xmm, r/m
        if (pp != 1 || len != 2 || src1 != 0) throw Unsupported{};
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        const int sz = w ? 8 : 4;
        const std::uint64_t val = rd_rm(rm, sz);
        s.xmm[rg][0] = val;
        s.xmm[rg][1] = 0;
        s.ymm_hi[rg][0] = 0;
        s.ymm_hi[rg][1] = 0;
        return;
    }
    if (op == 0x7E && (pp == 1 || pp == 2)) { // vmovd / vmovq r/m, xmm OR vmovq xmm, xmm/m64
        if (len != 2 || src1 != 0) throw Unsupported{};
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        if (pp == 1) {
            const int sz = w ? 8 : 4;
            wr_rm(rm, sz, s.xmm[rg][0]);
            return;
        }
        if (pp == 2) {
            const std::uint64_t val = rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8);
            s.xmm[rg][0] = val;
            s.xmm[rg][1] = 0;
            s.ymm_hi[rg][0] = 0;
            s.ymm_hi[rg][1] = 0;
            return;
        }
        throw Unsupported{};
    }
    if (op == 0xD6 && pp == 1) { // vmovq xmm2/m64, xmm1
        if (len != 2 || src1 != 0) throw Unsupported{};
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        if (rm.reg) {
            s.xmm[rm.r][0] = s.xmm[rg][0];
            s.xmm[rm.r][1] = 0;
            s.ymm_hi[rm.r][0] = 0;
            s.ymm_hi[rm.r][1] = 0;
        } else {
            wr_mem(rm.addr, 8, s.xmm[rg][0]);
        }
        return;
    }
    if (op == 0x2A) { // vcvtsi2ss / vcvtsi2sd
        if ((pp != 2 && pp != 3) || len != 2) throw Unsupported{};
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        const int sz = w ? 8 : 4;
        const std::uint64_t raw = rd_rm(rm, sz);
        const std::int64_t v = (sz == 8) ? static_cast<std::int64_t>(raw)
                                         : static_cast<std::int64_t>(static_cast<std::int32_t>(raw));
        const std::uint64_t dest0 = (pp == 2)
            ? ((s.xmm[src1][0] & ~0xFFFFFFFFULL) | bits32(static_cast<float>(v)))
            : bits64(static_cast<double>(v));
        const std::uint64_t dest1 = s.xmm[src1][1];
        s.xmm[rg][0] = dest0;
        s.xmm[rg][1] = dest1;
        s.ymm_hi[rg][0] = 0;
        s.ymm_hi[rg][1] = 0;
        return;
    }
    if (op == 0x2C) { // vcvttss2si / vcvttsd2si
        if ((pp != 2 && pp != 3) || len != 2 || src1 != 0) throw Unsupported{};
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        const double d = (pp == 2)
            ? static_cast<double>(f32(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 4)))
            : f64(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8));
        const int sz = w ? 8 : 4;
        std::int64_t out;
        if (d != d || d >= 9223372036854775808.0 || d < -9223372036854775808.0) out = INT64_MIN;
        else out = static_cast<std::int64_t>(d);
        if (sz == 4 && (out > INT32_MAX || out < INT32_MIN)) out = INT32_MIN;
        wr_reg(rg, sz, static_cast<std::uint64_t>(out));
        return;
    }
    if (op == 0x5A) { // vcvtss2sd / vcvtsd2ss / vcvtps2pd / vcvtpd2ps
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        if (pp == 2) { // vcvtss2sd xmm1, xmm2, xmm3/m32
            if (len != 2) throw Unsupported{};
            const float src = f32(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 4));
            const double d = static_cast<double>(src);
            s.xmm[rg][0] = bits64(d);
            s.xmm[rg][1] = s.xmm[src1][1];
            s.ymm_hi[rg][0] = 0;
            s.ymm_hi[rg][1] = 0;
            return;
        }
        if (pp == 3) { // vcvtsd2ss xmm1, xmm2, xmm3/m64
            if (len != 2) throw Unsupported{};
            const double src = f64(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8));
            const float f = static_cast<float>(src);
            s.xmm[rg][0] = (s.xmm[src1][0] & ~0xFFFFFFFFULL) | bits32(f);
            s.xmm[rg][1] = s.xmm[src1][1];
            s.ymm_hi[rg][0] = 0;
            s.ymm_hi[rg][1] = 0;
            return;
        }
        if (pp == 0) { // vcvtps2pd (128-bit: xmm, xmm/m64 -> 2 doubles; 256-bit: ymm, xmm/m128 -> 4 doubles)
            if (src1 != 0) throw Unsupported{};
            if (len == 2) {
                const std::uint64_t raw = rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8);
                const float f0 = f32(raw & 0xFFFFFFFFULL);
                const float f1 = f32(raw >> 32);
                s.xmm[rg][0] = bits64(static_cast<double>(f0));
                s.xmm[rg][1] = bits64(static_cast<double>(f1));
                s.ymm_hi[rg][0] = 0;
                s.ymm_hi[rg][1] = 0;
            } else {
                std::uint64_t raw[2] = {};
                if (rm.reg) { raw[0] = s.xmm[rm.r][0]; raw[1] = s.xmm[rm.r][1]; }
                else { raw[0] = rd_mem(rm.addr, 8); raw[1] = rd_mem(rm.addr + 8, 8); }
                const float f0 = f32(raw[0] & 0xFFFFFFFFULL);
                const float f1 = f32(raw[0] >> 32);
                const float f2 = f32(raw[1] & 0xFFFFFFFFULL);
                const float f3 = f32(raw[1] >> 32);
                s.xmm[rg][0] = bits64(static_cast<double>(f0));
                s.xmm[rg][1] = bits64(static_cast<double>(f1));
                s.ymm_hi[rg][0] = bits64(static_cast<double>(f2));
                s.ymm_hi[rg][1] = bits64(static_cast<double>(f3));
            }
            return;
        }
        if (pp == 1) { // vcvtpd2ps (128-bit: xmm, xmm/m128 -> 2 floats; 256-bit: xmm, ymm/m256 -> 4 floats)
            if (src1 != 0) throw Unsupported{};
            if (len == 2) {
                std::uint64_t raw[2] = {};
                if (rm.reg) { raw[0] = s.xmm[rm.r][0]; raw[1] = s.xmm[rm.r][1]; }
                else { raw[0] = rd_mem(rm.addr, 8); raw[1] = rd_mem(rm.addr + 8, 8); }
                const float f0 = static_cast<float>(f64(raw[0]));
                const float f1 = static_cast<float>(f64(raw[1]));
                s.xmm[rg][0] = bits32(f0) | (static_cast<std::uint64_t>(bits32(f1)) << 32);
                s.xmm[rg][1] = 0;
                s.ymm_hi[rg][0] = 0;
                s.ymm_hi[rg][1] = 0;
            } else {
                std::uint64_t raw[4] = {};
                for (int i = 0; i < 4; ++i) {
                    raw[i] = rm.reg ? (i < 2 ? s.xmm[rm.r][i] : s.ymm_hi[rm.r][i - 2]) : rd_mem(rm.addr + i * 8, 8);
                }
                const float f0 = static_cast<float>(f64(raw[0]));
                const float f1 = static_cast<float>(f64(raw[1]));
                const float f2 = static_cast<float>(f64(raw[2]));
                const float f3 = static_cast<float>(f64(raw[3]));
                s.xmm[rg][0] = bits32(f0) | (static_cast<std::uint64_t>(bits32(f1)) << 32);
                s.xmm[rg][1] = bits32(f2) | (static_cast<std::uint64_t>(bits32(f3)) << 32);
                s.ymm_hi[rg][0] = 0;
                s.ymm_hi[rg][1] = 0;
            }
            return;
        }
    }
    if (op == 0x5B) { // vcvtdq2ps / vcvtps2dq / vcvttps2dq
        if (src1 != 0) throw Unsupported{};
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        const int num_lanes = len * 2;
        std::uint32_t src[8] = {}, out[8] = {};
        if (rm.reg) {
            for (int i = 0; i < len; ++i) {
                const std::uint64_t val = (i < 2) ? s.xmm[rm.r][i] : s.ymm_hi[rm.r][i - 2];
                src[i * 2 + 0] = static_cast<std::uint32_t>(val);
                src[i * 2 + 1] = static_cast<std::uint32_t>(val >> 32);
            }
        } else {
            for (int i = 0; i < num_lanes; ++i) src[i] = static_cast<std::uint32_t>(rd_mem(rm.addr + i * 4, 4));
        }
        if (pp == 0) { // vcvtdq2ps: 32-bit int -> float
            for (int i = 0; i < num_lanes; ++i) {
                const std::int32_t si = static_cast<std::int32_t>(src[i]);
                const float f = static_cast<float>(si);
                out[i] = static_cast<std::uint32_t>(bits32(f));
            }
        } else if (pp == 1 || pp == 2) { // vcvtps2dq / vcvttps2dq: float -> 32-bit int
            for (int i = 0; i < num_lanes; ++i) {
                const float f = f32(src[i]);
                std::int32_t val = 0;
                if (std::isnan(f) || f >= 2147483648.0f || f < -2147483648.0f) val = INT32_MIN;
                else if (pp == 2) val = static_cast<std::int32_t>(f);
                else val = static_cast<std::int32_t>(std::nearbyint(f));
                out[i] = static_cast<std::uint32_t>(val);
            }
        } else {
            throw Unsupported{};
        }
        for (int i = 0; i < 2; ++i) s.xmm[rg][i] = static_cast<std::uint64_t>(out[i * 2]) | (static_cast<std::uint64_t>(out[i * 2 + 1]) << 32);
        for (int i = 0; i < 2; ++i) s.ymm_hi[rg][i] = (len == 4) ? (static_cast<std::uint64_t>(out[4 + i * 2]) | (static_cast<std::uint64_t>(out[4 + i * 2 + 1]) << 32)) : 0;
        return;
    }
    if ((op == 0x10 || op == 0x11) && pp >= 2) { // vmovss / vmovsd
        if (len != 2) throw Unsupported{};
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        const int width = (pp == 2) ? 4 : 8;
        if (op == 0x10) {
            if (rm.reg) {
                const std::uint64_t dest0 = (width == 4)
                    ? ((s.xmm[src1][0] & ~0xFFFFFFFFULL) | (s.xmm[rm.r][0] & 0xFFFFFFFFULL))
                    : s.xmm[rm.r][0];
                const std::uint64_t dest1 = s.xmm[src1][1];
                s.xmm[rg][0] = dest0;
                s.xmm[rg][1] = dest1;
            } else {
                if (src1 != 0) throw Unsupported{};
                s.xmm[rg][0] = rd_mem(rm.addr, width);
                s.xmm[rg][1] = 0;
            }
            s.ymm_hi[rg][0] = 0;
            s.ymm_hi[rg][1] = 0;
            return;
        } else { // op == 0x11
            if (rm.reg) {
                const std::uint64_t dest0 = (width == 4)
                    ? ((s.xmm[src1][0] & ~0xFFFFFFFFULL) | (s.xmm[rg][0] & 0xFFFFFFFFULL))
                    : s.xmm[rg][0];
                const std::uint64_t dest1 = s.xmm[src1][1];
                s.xmm[rm.r][0] = dest0;
                s.xmm[rm.r][1] = dest1;
                s.ymm_hi[rm.r][0] = 0;
                s.ymm_hi[rm.r][1] = 0;
            } else {
                if (src1 != 0) throw Unsupported{};
                wr_mem(rm.addr, width, s.xmm[rg][0]);
            }
            return;
        }
    }
    if (op == 0x12) { // vmovhlps, vmovlps, vmovlpd, vmovsldup, vmovddup
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        if (pp == 0) { // vmovhlps (reg) or vmovlps (mem)
            if (len != 2) throw Unsupported{};
            if (rm.reg) { // vmovhlps xmm1, xmm2, xmm3
                s.xmm[rg][0] = s.xmm[rm.r][1];
                s.xmm[rg][1] = s.xmm[src1][1];
            } else { // vmovlps xmm1, xmm2, m64
                s.xmm[rg][0] = rd_mem(rm.addr, 8);
                s.xmm[rg][1] = s.xmm[src1][1];
            }
            s.ymm_hi[rg][0] = s.ymm_hi[rg][1] = 0;
            return;
        }
        if (pp == 1) { // vmovlpd xmm1, xmm2, m64
            if (len != 2 || rm.reg) throw Unsupported{};
            s.xmm[rg][0] = rd_mem(rm.addr, 8);
            s.xmm[rg][1] = s.xmm[src1][1];
            s.ymm_hi[rg][0] = s.ymm_hi[rg][1] = 0;
            return;
        }
        if (pp == 2) { // vmovsldup xmm1, xmm2/m128 (or ymm)
            if (src1 != 0) throw Unsupported{};
            std::uint64_t src[4] = {};
            if (rm.reg) for (int i = 0; i < len; ++i) src[i] = i < 2 ? s.xmm[rm.r][i] : s.ymm_hi[rm.r][i - 2];
            else for (int i = 0; i < len; ++i) src[i] = rd_mem(rm.addr + i * 8, 8);
            for (int lane = 0; lane < len / 2; ++lane) {
                const std::uint64_t w0 = src[lane * 2], w1 = src[lane * 2 + 1];
                const std::uint64_t d0 = (w0 & 0xFFFFFFFFULL) | ((w0 & 0xFFFFFFFFULL) << 32);
                const std::uint64_t d1 = (w1 & 0xFFFFFFFFULL) | ((w1 & 0xFFFFFFFFULL) << 32);
                if (lane == 0) { s.xmm[rg][0] = d0; s.xmm[rg][1] = d1; }
                else { s.ymm_hi[rg][0] = d0; s.ymm_hi[rg][1] = d1; }
            }
            if (len == 2) s.ymm_hi[rg][0] = s.ymm_hi[rg][1] = 0;
            return;
        }
        if (pp == 3) { // vmovddup xmm1, xmm2/m64 (or ymm, m256)
            if (src1 != 0) throw Unsupported{};
            if (len == 2) {
                const std::uint64_t val = rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8);
                s.xmm[rg][0] = val; s.xmm[rg][1] = val;
                s.ymm_hi[rg][0] = s.ymm_hi[rg][1] = 0;
            } else {
                if (rm.reg) {
                    const std::uint64_t lo = s.xmm[rm.r][0], hi = s.ymm_hi[rm.r][0];
                    s.xmm[rg][0] = lo; s.xmm[rg][1] = lo;
                    s.ymm_hi[rg][0] = hi; s.ymm_hi[rg][1] = hi;
                } else {
                    const std::uint64_t mem_v0 = rd_mem(rm.addr, 8), mem_v1 = rd_mem(rm.addr + 16, 8);
                    s.xmm[rg][0] = mem_v0; s.xmm[rg][1] = mem_v0;
                    s.ymm_hi[rg][0] = mem_v1; s.ymm_hi[rg][1] = mem_v1;
                }
            }
            return;
        }
        throw Unsupported{};
    }
    if (op == 0x13) { // vmovlps, vmovlpd m64, xmm1
        if (len != 2 || (pp != 0 && pp != 1) || src1 != 0) throw Unsupported{};
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        if (rm.reg) throw Unsupported{};
        wr_mem(rm.addr, 8, s.xmm[rg][0]);
        return;
    }
    if (op == 0x16) { // vmovlhps, vmovhps, vmovhpd, vmovshdup
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        if (pp == 0) { // vmovlhps (reg) or vmovhps (mem)
            if (len != 2) throw Unsupported{};
            if (rm.reg) { // vmovlhps xmm1, xmm2, xmm3
                const std::uint64_t low = s.xmm[src1][0], high = s.xmm[rm.r][0];
                s.xmm[rg][0] = low;
                s.xmm[rg][1] = high;
            } else { // vmovhps xmm1, xmm2, m64
                s.xmm[rg][0] = s.xmm[src1][0];
                s.xmm[rg][1] = rd_mem(rm.addr, 8);
            }
            s.ymm_hi[rg][0] = s.ymm_hi[rg][1] = 0;
            return;
        }
        if (pp == 1) { // vmovhpd xmm1, xmm2, m64
            if (len != 2 || rm.reg) throw Unsupported{};
            s.xmm[rg][0] = s.xmm[src1][0];
            s.xmm[rg][1] = rd_mem(rm.addr, 8);
            s.ymm_hi[rg][0] = s.ymm_hi[rg][1] = 0;
            return;
        }
        if (pp == 2) { // vmovshdup xmm1, xmm2/m128
            if (src1 != 0) throw Unsupported{};
            std::uint64_t src[4] = {};
            if (rm.reg) for (int i = 0; i < len; ++i) src[i] = i < 2 ? s.xmm[rm.r][i] : s.ymm_hi[rm.r][i - 2];
            else for (int i = 0; i < len; ++i) src[i] = rd_mem(rm.addr + i * 8, 8);
            for (int lane = 0; lane < len / 2; ++lane) {
                const std::uint64_t w0 = src[lane * 2], w1 = src[lane * 2 + 1];
                const std::uint64_t d0 = (w0 >> 32) | ((w0 >> 32) << 32);
                const std::uint64_t d1 = (w1 >> 32) | ((w1 >> 32) << 32);
                if (lane == 0) { s.xmm[rg][0] = d0; s.xmm[rg][1] = d1; }
                else { s.ymm_hi[rg][0] = d0; s.ymm_hi[rg][1] = d1; }
            }
            if (len == 2) s.ymm_hi[rg][0] = s.ymm_hi[rg][1] = 0;
            return;
        }
        throw Unsupported{};
    }
    if (op == 0x17) { // vmovhps, vmovhpd m64, xmm1
        if (len != 2 || (pp != 0 && pp != 1) || src1 != 0) throw Unsupported{};
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        if (rm.reg) throw Unsupported{};
        wr_mem(rm.addr, 8, s.xmm[rg][1]);
        return;
    }
    if (op == 0x2E || op == 0x2F) { // vucomiss / vucomisd / vcomiss / vcomisd
        if (pp > 1 || len != 2 || src1 != 0) throw Unsupported{};
        int rg = 0;
        const Rm rm = decode_rm(rg, 0);
        double a, b;
        if (pp == 1) { a = f64(s.xmm[rg][0]); b = f64(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8)); }
        else { a = f32(s.xmm[rg][0]); b = f32(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 4)); }
        s.rflags &= ~(FLAG_OF | FLAG_SF | FLAG_AF | FLAG_ZF | FLAG_PF | FLAG_CF);
        if (a != a || b != b) s.rflags |= FLAG_ZF | FLAG_PF | FLAG_CF;
        else if (a > b) {}
        else if (a < b) s.rflags |= FLAG_CF;
        else s.rflags |= FLAG_ZF;
        return;
    }
    const bool mov = ((op == 0x10 || op == 0x11 || op == 0x28 || op == 0x29) && pp <= 1) ||
                     ((op == 0x6F || op == 0x7F) && (pp == 1 || pp == 2)) ||
                     ((op == 0x2B && pp <= 1) || (op == 0xE7 && pp == 1));
    const bool logic = (op == 0x57) || (pp == 0 && (op == 0x54 || op == 0x55 || op == 0x56)) ||
                       (pp == 1 && (op == 0xEF || op == 0xDB || op == 0xEB || op == 0xDF ||
                                    op == 0x54 || op == 0x55 || op == 0x56 || op == 0x57));
    const bool arith = op == 0x58 || op == 0x59 || op == 0x5C || op == 0x5E ||
                       op == 0x5F || op == 0x5D || op == 0x51 || op == 0x52 || op == 0x53;
    // 0F 7C/7D: F2 = horizontal PS, 66 = horizontal PD.
    const bool hadd_sub = (op == 0x7C || op == 0x7D) && (pp == 1 || pp == 3);
    const bool unpack = (pp == 1 &&
        ((op >= 0x60 && op <= 0x62) || op == 0x6C ||
         (op >= 0x68 && op <= 0x6A) || op == 0x6D)) ||
        (pp <= 1 && (op == 0x14 || op == 0x15));
    const bool pack = pp == 1 && (op == 0x63 || op == 0x67 || op == 0x6B);
    const bool padd_sub = pp == 1 &&
        (op == 0xFC || op == 0xFD || op == 0xFE || op == 0xD4 ||
         op == 0xF8 || op == 0xF9 || op == 0xFA || op == 0xFB);
    const bool compare = op == 0xC2;
    const bool pcmpeq = pp == 1 && (op >= 0x74 && op <= 0x76);
    const bool pcmpgt = pp == 1 && (op >= 0x64 && op <= 0x66);
    const bool pcmp = pcmpeq || pcmpgt;
    const bool shuffle = (op == 0x70 && pp == 1) || (op == 0xC6 && pp <= 1);
    if ((!mov && !logic && !arith && !unpack && !pack && !padd_sub && !shuffle && !hadd_sub && !compare && !pcmp) ||
        (pp > 1 && !arith && !mov && !hadd_sub && !compare) ||
        (pp > 1 && len != 2 && !mov && !hadd_sub) ||
        (logic && op != 0x57 && pp != 1 && pp != 0))
        throw Unsupported{};
    if (mov && src1 != 0) throw Unsupported{}; // reserved vvvv=1111
    const bool unary = op == 0x51 || op == 0x52 || op == 0x53;
    if (unary && pp <= 1 && src1 != 0) throw Unsupported{};
    if (unary && (pp == 1 || pp == 3) && (op == 0x52 || op == 0x53)) throw Unsupported{};
    if ((op == 0x28 || op == 0x29) && pp != 0 && pp != 1) throw Unsupported{};
    if (op == 0x57 && pp != 0 && pp != 1) throw Unsupported{};
    if (hadd_sub) {
        if (len != 2 && len != 4) throw Unsupported{}; // 128 or 256 bit only
        // vvvv (src1) is the first source register for hadd, not reserved
    }
    if (shuffle) {
        if (len != 2 && len != 4) throw Unsupported{}; // 128 or 256 bit only
        if (op == 0x70 && src1 != 0) throw Unsupported{}; // vpshufd requires vvvv = 1111b
    }
    if (compare) {
        if (len != 2 && len != 4) throw Unsupported{};
        if (pp > 1 && len != 2) throw Unsupported{};
    }
    int rg = 0;
    const Rm rm = decode_rm(rg, (shuffle || compare) ? 1 : 0);
    std::uint64_t source[4] = {}, dest[4] = {};
    const bool scalar = (arith || compare) && pp >= 2;
    const int width = (pp == 1 || pp == 3) ? 8 : 4;
    const int bytes = scalar ? width : len * 8;
    if (mov && (op == 0x11 || op == 0x29 || op == 0x7F || op == 0x2B || op == 0xE7)) {
        for (int i = 0; i < len; ++i) source[i] = i < 2 ? s.xmm[rg][i] : s.ymm_hi[rg][i - 2];
        if (rm.reg) {
            for (int i = 0; i < len; ++i) (i < 2 ? s.xmm[rm.r][i] : s.ymm_hi[rm.r][i - 2]) = source[i];
            if (len == 2) s.ymm_hi[rm.r][0] = s.ymm_hi[rm.r][1] = 0;
        } else for (int i = 0; i < len; ++i) wr_mem(rm.addr + i * 8, 8, source[i]);
        return;
    }
    if (rm.reg) for (int i = 0; i < len; ++i) source[i] = i < 2 ? s.xmm[rm.r][i] : s.ymm_hi[rm.r][i - 2];
    else for (int i = 0; i < (bytes + 7) / 8; ++i) source[i] = rd_mem(rm.addr + i * 8, scalar ? width : 8);
    if (mov) {
        for (int i = 0; i < len; ++i) dest[i] = source[i];
    } else if (unpack) {
        for (int i = 0; i < len; ++i) dest[i] = i < 2 ? s.xmm[src1][i] : s.ymm_hi[src1][i - 2];
        const bool is_high = (op >= 0x68 && op <= 0x6A) || op == 0x6D || op == 0x15;
        const int elem_width = (op == 0x6C || op == 0x6D || (op >= 0x14 && op <= 0x15 && pp == 1)) ? 8
                             : (op == 0x62 || op == 0x6A || (op >= 0x14 && op <= 0x15 && pp == 0)) ? 4
                             : (op == 0x61 || op == 0x69) ? 2 : 1;
        std::uint8_t a[32], b[32], out[32];
        std::memcpy(a, dest, len * 8);
        std::memcpy(b, source, len * 8);
        const int num_lanes = len / 2;
        const int offset = is_high ? 8 : 0;
        const int elems_per_lane = 8 / elem_width;
        for (int lane = 0; lane < num_lanes; ++lane) {
            const std::uint8_t* lane_a = a + lane * 16;
            const std::uint8_t* lane_b = b + lane * 16;
            std::uint8_t* lane_out = out + lane * 16;
            for (int i = 0; i < elems_per_lane; ++i) {
                std::memcpy(lane_out + 2 * i * elem_width, lane_a + offset + i * elem_width, elem_width);
                std::memcpy(lane_out + (2 * i + 1) * elem_width, lane_b + offset + i * elem_width, elem_width);
            }
        }
        std::memcpy(dest, out, len * 8);
    } else if (pack) {
        // VPACKSSWB (0x63), VPACKUSWB (0x67), VPACKSSDW (0x6B)
        std::uint64_t s1[4] = {};
        for (int i = 0; i < len; ++i) s1[i] = i < 2 ? s.xmm[src1][i] : s.ymm_hi[src1][i - 2];
        const int num_lanes = len / 2;
        for (int lane = 0; lane < num_lanes; ++lane) {
            const std::uint8_t* lane_s1 = reinterpret_cast<const std::uint8_t*>(&s1[lane * 2]);
            const std::uint8_t* lane_s2 = reinterpret_cast<const std::uint8_t*>(&source[lane * 2]);
            std::uint8_t lane_dst[16] = {};
            if (op == 0x63) { // VPACKSSWB: s16 -> s8
                for (int i = 0; i < 8; ++i) {
                    std::int16_t v = 0;
                    std::memcpy(&v, lane_s1 + i * 2, 2);
                    lane_dst[i] = static_cast<std::uint8_t>(v > 127 ? 127 : v < -128 ? -128 : v);
                }
                for (int i = 0; i < 8; ++i) {
                    std::int16_t v = 0;
                    std::memcpy(&v, lane_s2 + i * 2, 2);
                    lane_dst[8 + i] = static_cast<std::uint8_t>(v > 127 ? 127 : v < -128 ? -128 : v);
                }
            } else if (op == 0x67) { // VPACKUSWB: s16 -> u8
                for (int i = 0; i < 8; ++i) {
                    std::int16_t v = 0;
                    std::memcpy(&v, lane_s1 + i * 2, 2);
                    lane_dst[i] = static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
                }
                for (int i = 0; i < 8; ++i) {
                    std::int16_t v = 0;
                    std::memcpy(&v, lane_s2 + i * 2, 2);
                    lane_dst[8 + i] = static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
                }
            } else if (op == 0x6B) { // VPACKSSDW: s32 -> s16
                std::int16_t* dst16 = reinterpret_cast<std::int16_t*>(lane_dst);
                for (int i = 0; i < 4; ++i) {
                    std::int32_t v = 0;
                    std::memcpy(&v, lane_s1 + i * 4, 4);
                    dst16[i] = static_cast<std::int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
                for (int i = 0; i < 4; ++i) {
                    std::int32_t v = 0;
                    std::memcpy(&v, lane_s2 + i * 4, 4);
                    dst16[4 + i] = static_cast<std::int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
            }
            std::memcpy(&dest[lane * 2], lane_dst, 16);
        }
    } else if (padd_sub) {
        for (int i = 0; i < len; ++i) dest[i] = i < 2 ? s.xmm[src1][i] : s.ymm_hi[src1][i - 2];
        const bool is_add = (op == 0xFC || op == 0xFD || op == 0xFE || op == 0xD4);
        const int elem_width = (op == 0xD4 || op == 0xFB) ? 8
                             : (op == 0xFE || op == 0xFA) ? 4
                             : (op == 0xFD || op == 0xF9) ? 2 : 1;
        std::uint8_t a[32], b[32], out[32];
        std::memcpy(a, dest, len * 8);
        std::memcpy(b, source, len * 8);
        const int total_bytes = len * 8;
        for (int i = 0; i < total_bytes; i += elem_width) {
            std::uint64_t x = 0, y = 0, z = 0;
            std::memcpy(&x, a + i, elem_width);
            std::memcpy(&y, b + i, elem_width);
            if (is_add) z = x + y;
            else z = x - y;
            std::memcpy(out + i, &z, elem_width);
        }
        std::memcpy(dest, out, len * 8);
    } else if (pcmp) {
        for (int i = 0; i < len; ++i) dest[i] = i < 2 ? s.xmm[src1][i] : s.ymm_hi[src1][i - 2];
        const int elem_width = (op == 0x76 || op == 0x66) ? 4
                             : (op == 0x75 || op == 0x65) ? 2 : 1;
        std::uint8_t a[32], b[32], out[32];
        std::memcpy(a, dest, len * 8);
        std::memcpy(b, source, len * 8);
        const int total_bytes = len * 8;
        for (int i = 0; i < total_bytes; i += elem_width) {
            std::uint64_t x = 0, y = 0, z = 0;
            std::memcpy(&x, a + i, elem_width);
            std::memcpy(&y, b + i, elem_width);
            if (pcmpeq) {
                z = (x == y) ? mask_of(elem_width) : 0;
            } else {
                const std::int64_t sx = sext(x, elem_width), sy = sext(y, elem_width);
                z = (sx > sy) ? mask_of(elem_width) : 0;
            }
            std::memcpy(out + i, &z, elem_width);
        }
        std::memcpy(dest, out, len * 8);
    } else if (hadd_sub) {
        // VHADDPS/VHSUBPS (F2), VHADDPD/VHSUBPD (66).
        // Horizontal add/sub: adjacent pairs within each source operand.
        // For PS (4 floats per 128-bit lane): dst[0]=src1[0]±src1[1], dst[1]=src1[2]±src1[3], dst[2]=src2[0]±src2[1], dst[3]=src2[2]±src2[3]
        // For PD (2 doubles per 128-bit lane): dst[0]=src1[0]±src1[1], dst[1]=src2[0]±src2[1]
        // 256-bit: same operation independently in each 128-bit lane.
        for (int i = 0; i < len; ++i) dest[i] = i < 2 ? s.xmm[src1][i] : s.ymm_hi[src1][i - 2];
        const bool is_pd = pp == 1;
        const bool is_sub = op == 0x7D;
        const int elem_bits = is_pd ? 64 : 32;
        const int elem_bytes = elem_bits / 8;
        const int elems_per_128 = 128 / elem_bits;
        const int num_lanes = len / 2; // each 128-bit lane
        std::uint8_t a[32], b[32], out[32];
        // src1 is in dest (from src1 register), src2 is in source (from modrm)
        std::memcpy(a, dest, len * 8);
        std::memcpy(b, source, len * 8);
        for (int lane = 0; lane < num_lanes; ++lane) {
            const std::uint8_t* lane_a = a + lane * 16;
            const std::uint8_t* lane_b = b + lane * 16;
            std::uint8_t* lane_out = out + lane * 16;
            for (int i = 0; i < elems_per_128 / 2; ++i) {
                // Add/sub adjacent pairs from src1 (lane_a)
                std::uint64_t x1 = 0, x2 = 0;
                std::memcpy(&x1, lane_a + 2 * i * elem_bytes, elem_bytes);
                std::memcpy(&x2, lane_a + (2 * i + 1) * elem_bytes, elem_bytes);
                std::uint64_t r1 = 0;
                if (is_pd) {
                    const double d1 = f64(x1), d2 = f64(x2);
                    r1 = bits64(is_sub ? d1 - d2 : d1 + d2);
                } else {
                    const float f1 = f32(x1), f2 = f32(x2);
                    r1 = bits32(is_sub ? f1 - f2 : f1 + f2);
                }
                std::memcpy(lane_out + i * elem_bytes, &r1, elem_bytes);
                // Add/sub adjacent pairs from src2 (lane_b)
                std::uint64_t y1 = 0, y2 = 0;
                std::memcpy(&y1, lane_b + 2 * i * elem_bytes, elem_bytes);
                std::memcpy(&y2, lane_b + (2 * i + 1) * elem_bytes, elem_bytes);
                std::uint64_t r2 = 0;
                if (is_pd) {
                    const double d1 = f64(y1), d2 = f64(y2);
                    r2 = bits64(is_sub ? d1 - d2 : d1 + d2);
                } else {
                    const float f1 = f32(y1), f2 = f32(y2);
                    r2 = bits32(is_sub ? f1 - f2 : f1 + f2);
                }
                std::memcpy(lane_out + (i + elems_per_128 / 2) * elem_bytes, &r2, elem_bytes);
            }
        }
        std::memcpy(dest, out, len * 8);
    } else if (shuffle) {
        // vpshufd (0x70, pp=1) or vshufps/vshufpd (0xC6, pp=0/1)
        const unsigned imm = u8();
        if (op == 0x70) { // vpshufd: shuffle 32-bit ints within each 128-bit lane from rm
            for (int lane = 0; lane < len / 2; ++lane) {
                const std::uint64_t lane_src[2] = {source[lane * 2], source[lane * 2 + 1]};
                std::uint64_t lane_dst[2] = {0, 0};
                for (int i = 0; i < 4; ++i) {
                    const int idx = (imm >> (i * 2)) & 3;
                    const std::uint64_t v = (lane_src[idx / 2] >> ((idx % 2) * 32)) & 0xFFFFFFFFULL;
                    const int j = i / 2, sh = (i % 2) * 32;
                    lane_dst[j] = (lane_dst[j] & ~(0xFFFFFFFFULL << sh)) | (v << sh);
                }
                dest[lane * 2] = lane_dst[0];
                dest[lane * 2 + 1] = lane_dst[1];
            }
        } else {
            // op == 0xC6: vshufps (pp=0) or vshufpd (pp=1)
            // First source operand is src1 (vvvv), second source operand is rm (source).
            std::uint64_t s1[4] = {};
            for (int i = 0; i < len; ++i) s1[i] = i < 2 ? s.xmm[src1][i] : s.ymm_hi[src1][i - 2];
            if (pp == 0) { // vshufps: 32-bit float shuffle
                for (int lane = 0; lane < len / 2; ++lane) {
                    const std::uint64_t s1_lane[2] = {s1[lane * 2], s1[lane * 2 + 1]};
                    const std::uint64_t s2_lane[2] = {source[lane * 2], source[lane * 2 + 1]};
                    std::uint64_t lane_dst[2] = {0, 0};
                    for (int i = 0; i < 4; ++i) {
                        const int idx = (imm >> (i * 2)) & 3;
                        const std::uint64_t* from = (i < 2) ? s1_lane : s2_lane;
                        const std::uint64_t v = (from[idx / 2] >> ((idx % 2) * 32)) & 0xFFFFFFFFULL;
                        const int j = i / 2, sh = (i % 2) * 32;
                        lane_dst[j] = (lane_dst[j] & ~(0xFFFFFFFFULL << sh)) | (v << sh);
                    }
                    dest[lane * 2] = lane_dst[0];
                    dest[lane * 2 + 1] = lane_dst[1];
                }
            } else { // vshufpd: 64-bit float shuffle
                for (int lane = 0; lane < len / 2; ++lane) {
                    const int bit0 = lane * 2;
                    const int bit1 = lane * 2 + 1;
                    dest[lane * 2 + 0] = ((imm >> bit0) & 1) ? s1[lane * 2 + 1] : s1[lane * 2 + 0];
                    dest[lane * 2 + 1] = ((imm >> bit1) & 1) ? source[lane * 2 + 1] : source[lane * 2 + 0];
                }
            }
        }
    } else if (compare) {
        const unsigned imm = u8();
        for (int i = 0; i < len; ++i) dest[i] = i < 2 ? s.xmm[src1][i] : s.ymm_hi[src1][i - 2];
        const int lanes = scalar ? 1 : bytes / width;
        for (int i = 0; i < lanes; ++i) {
            const int word = i * width / 8, shift = (i * width % 8) * 8;
            const std::uint64_t mask = mask_of(width);
            const std::uint64_t a = (dest[word] >> shift) & mask, b = (source[word] >> shift) & mask;
            const std::uint64_t val = (width == 8)
                ? (eval_cmp_pred(f64(a), f64(b), imm) ? mask : 0ULL)
                : (eval_cmp_pred(f32(a), f32(b), imm) ? mask : 0ULL);
            dest[word] = (dest[word] & ~(mask << shift)) | ((val & mask) << shift);
        }
    } else {
        for (int i = 0; i < len; ++i) dest[i] = i < 2 ? s.xmm[src1][i] : s.ymm_hi[src1][i - 2];
        for (int i = 0; i < bytes / width; ++i) {
            const int word = i * width / 8, shift = (i * width % 8) * 8;
            const std::uint64_t mask = mask_of(width);
            const std::uint64_t a = (dest[word] >> shift) & mask, b = (source[word] >> shift) & mask;
            std::uint64_t value = 0;
            if (logic) {
                if (op == 0xDB || op == 0x54) value = a & b;
                else if (op == 0xEB || op == 0x56) value = a | b;
                else if (op == 0xDF || op == 0x55) value = (~a) & b;
                else value = a ^ b; // 0x57, 0xEF
            } else if (width == 8) {
                const double x = f64(a), y = f64(b);
                value = bits64(op == 0x58 ? x + y : op == 0x59 ? x * y : op == 0x5C ? x - y
                    : op == 0x5E ? x / y : op == 0x51 ? std::sqrt(y)
                    : op == 0x5F ? (std::isnan(x) || std::isnan(y) || x == y ? y : (x > y ? x : y))
                    : (std::isnan(x) || std::isnan(y) || x == y ? y : (x < y ? x : y)));
            } else {
                const float x = f32(a), y = f32(b);
                value = bits32(op == 0x58 ? x + y : op == 0x59 ? x * y : op == 0x5C ? x - y
                    : op == 0x5E ? x / y : op == 0x51 ? std::sqrt(y)
                    : op == 0x52 ? 1.0f / std::sqrt(y) : op == 0x53 ? 1.0f / y
                    : op == 0x5F ? (std::isnan(x) || std::isnan(y) || x == y ? y : (x > y ? x : y))
                    : (std::isnan(x) || std::isnan(y) || x == y ? y : (x < y ? x : y)));
            }
            dest[word] = (dest[word] & ~(mask << shift)) | ((value & mask) << shift);
        }
    }
    for (int i = 0; i < 2; ++i) s.xmm[rg][i] = dest[i];
    for (int i = 0; i < 2; ++i) s.ymm_hi[rg][i] = len == 4 ? dest[i + 2] : 0;
}

void Exec::two_byte(int opsz) {
    const std::uint8_t op2 = u8();
    int rg = 0;
    if (op2 == 0x0B) throw UdEx{};
    if (op2 == 0x05) throw HaltEx{}; // syscall: trap, nunca chamar o kernel do host
    if (op2 == 0x38 || op2 == 0x3A) {
        const unsigned op3 = u8();
        if (op2 == 0x38 && (op3 == 0xF0 || op3 == 0xF1)) {
            const Rm rm = decode_rm(rg, 0);
            if (rm.reg) throw UdEx{};
            const int sz = (opsz == 2) ? 2 : (opsz == 8) ? 8 : 4;
            if (op3 == 0xF0) { // MOVBE r, m (load)
                std::uint64_t val = rd_mem(rm.addr, sz);
                std::uint64_t swapped = 0;
                if (sz == 2) swapped = ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
                else if (sz == 4) swapped = bswap32(static_cast<std::uint32_t>(val));
                else swapped = bswap64(val);
                wr_reg(rg, sz, swapped);
            } else { // MOVBE m, r (store)
                std::uint64_t val = rd_reg(rg, sz);
                std::uint64_t swapped = 0;
                if (sz == 2) swapped = ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
                else if (sz == 4) swapped = bswap32(static_cast<std::uint32_t>(val));
                else swapped = bswap64(val);
                wr_mem(rm.addr, sz, swapped);
            }
            return;
        }
        if (!p66 || (op2 == 0x38 && op3 != 0x00 && op3 != 0x40 &&
                     op3 != 0x01 && op3 != 0x02 && op3 != 0x03 &&
                     !(op3 >= 0x38 && op3 <= 0x3F) && !(op3 >= 0x1C && op3 <= 0x1E) &&
                     op3 != 0x28 && op3 != 0x29 && op3 != 0x2A && op3 != 0x2B &&
                     !((op3 >= 0x20 && op3 <= 0x25) || (op3 >= 0x30 && op3 <= 0x35))) ||
            (op2 == 0x3A && op3 != 0x08 && op3 != 0x09 && op3 != 0x0A && op3 != 0x0B &&
             op3 != 0x0F && !(op3 >= 0x14 && op3 <= 0x17) && op3 != 0x20 && op3 != 0x22)) throw Unsupported{};
        const int imm = op2 == 0x3A ? 1 : 0;
        const Rm rm = decode_rm(rg, imm);
        std::uint64_t src[2] = {};
        if ((op3 >= 0x20 && op3 <= 0x25) || (op3 >= 0x30 && op3 <= 0x35)) {
            const bool is_signed = (op3 >= 0x20 && op3 <= 0x25);
            const int sub = is_signed ? (op3 - 0x20) : (op3 - 0x30);
            const int src_elem_bytes = (sub == 0 || sub == 1 || sub == 2) ? 1 : (sub == 3 || sub == 4) ? 2 : 4;
            const int dst_elem_bytes = (sub == 0) ? 2 : (sub == 1 || sub == 3) ? 4 : 8;
            const int total_elems = 16 / dst_elem_bytes;
            const int total_src_bytes = total_elems * src_elem_bytes;
            std::uint8_t src_raw[16] = {};
            if (rm.reg) std::memcpy(src_raw, s.xmm[rm.r], 16);
            else { for (int b = 0; b < total_src_bytes; ++b) src_raw[b] = static_cast<std::uint8_t>(rd_mem(rm.addr + b, 1)); }
            std::uint8_t out[16] = {};
            for (int i = 0; i < total_elems; ++i) {
                std::uint64_t v = 0;
                std::memcpy(&v, src_raw + i * src_elem_bytes, src_elem_bytes);
                if (is_signed) v = sext(v, src_elem_bytes);
                std::memcpy(out + i * dst_elem_bytes, &v, dst_elem_bytes);
            }
            std::memcpy(s.xmm[rg], out, 16);
            return;
        }
        rd_xmm_rm(rm, src);
        std::uint8_t a[16], b[16], out[16] = {};
        std::memcpy(a, s.xmm[rg], 16); std::memcpy(b, src, 16);
        if (op2 == 0x3A) {
            const unsigned imm8 = u8();
            if (op3 == 0x0A) { // ROUNDSS xmm1, xmm2/m32, imm8
                float f = 0.0f;
                std::memcpy(&f, b, 4);
                float r = round_fp(f, imm8);
                std::memcpy(out, a, 16);
                std::memcpy(out, &r, 4);
            } else if (op3 == 0x0B) { // ROUNDSD xmm1, xmm2/m64, imm8
                double d = 0.0;
                std::memcpy(&d, b, 8);
                double r = round_fp(d, imm8);
                std::memcpy(out, a, 16);
                std::memcpy(out, &r, 8);
            } else if (op3 == 0x08) { // ROUNDPS xmm1, xmm2/m128, imm8
                float src_f[4], dst_f[4];
                std::memcpy(src_f, b, 16);
                for (int i = 0; i < 4; ++i) dst_f[i] = round_fp(src_f[i], imm8);
                std::memcpy(out, dst_f, 16);
            } else if (op3 == 0x09) { // ROUNDPD xmm1, xmm2/m128, imm8
                double src_d[2], dst_d[2];
                std::memcpy(src_d, b, 16);
                for (int i = 0; i < 2; ++i) dst_d[i] = round_fp(src_d[i], imm8);
                std::memcpy(out, dst_d, 16);
            } else if (op3 == 0x0F) { // palignr
                for (unsigned i = 0; i < 16; ++i) {
                    const unsigned j = imm8 + i;
                    out[i] = j < 16 ? b[j] : j < 32 ? a[j - 16] : 0;
                }
            } else if (op3 == 0x14 || op3 == 0x15 || op3 == 0x16 || op3 == 0x17) {
                if (op3 == 0x16 || op3 == 0x17) {
                    if (rw && op3 == 0x16) { // PEXTRQ
                        const unsigned idx = imm8 & 1;
                        std::uint64_t val = 0;
                        std::memcpy(&val, a + idx * 8, 8);
                        if (rm.reg) wr_reg(rm.r, 8, val);
                        else wr_mem(rm.addr, 8, val);
                    } else { // PEXTRD / EXTRACTPS
                        const unsigned idx = imm8 & 3;
                        std::uint32_t val = 0;
                        std::memcpy(&val, a + idx * 4, 4);
                        if (rm.reg) wr_reg(rm.r, 4, val);
                        else wr_mem(rm.addr, 4, val);
                    }
                } else if (op3 == 0x15) {
                    const unsigned idx = imm8 & 7;
                    std::uint16_t val = 0;
                    std::memcpy(&val, a + idx * 2, 2);
                    if (rm.reg) wr_reg(rm.r, 4, val);
                    else wr_mem(rm.addr, 2, val);
                } else {
                    const unsigned idx = imm8 & 15;
                    std::uint8_t val = a[idx];
                    if (rm.reg) wr_reg(rm.r, 4, val);
                    else wr_mem(rm.addr, 1, val);
                }
                return;
            } else if (op3 == 0x20 || op3 == 0x22) {
                std::memcpy(out, a, 16);
                if (op3 == 0x22) {
                    const int sz = rw ? 8 : 4;
                    std::uint64_t val = rm.reg ? rd_reg(rm.r, sz) : rd_mem(rm.addr, sz);
                    if (rw) {
                        const unsigned idx = imm8 & 1;
                        std::memcpy(out + idx * 8, &val, 8);
                    } else {
                        const unsigned idx = imm8 & 3;
                        std::memcpy(out + idx * 4, &val, 4);
                    }
                } else {
                    std::uint8_t val = static_cast<std::uint8_t>(rm.reg ? rd_reg(rm.r, 4) : rd_mem(rm.addr, 1));
                    const unsigned idx = imm8 & 15;
                    out[idx] = val;
                }
            } else if (op3 == 0x40) { // DPPS xmm1, xmm2/m128, imm8
                float a_f[4], b_f[4], dst_f[4] = {};
                std::memcpy(a_f, a, 16);
                std::memcpy(b_f, b, 16);
                float sum = 0.0f;
                if (imm8 & 0x10) sum += a_f[0] * b_f[0];
                if (imm8 & 0x20) sum += a_f[1] * b_f[1];
                if (imm8 & 0x40) sum += a_f[2] * b_f[2];
                if (imm8 & 0x80) sum += a_f[3] * b_f[3];

                dst_f[0] = (imm8 & 0x01) ? sum : 0.0f;
                dst_f[1] = (imm8 & 0x02) ? sum : 0.0f;
                dst_f[2] = (imm8 & 0x04) ? sum : 0.0f;
                dst_f[3] = (imm8 & 0x08) ? sum : 0.0f;
                std::memcpy(out, dst_f, 16);
            } else if (op3 == 0x41) { // DPPD xmm1, xmm2/m128, imm8
                double a_d[2], b_d[2], dst_d[2] = {};
                std::memcpy(a_d, a, 16);
                std::memcpy(b_d, b, 16);
                double sum = 0.0;
                if (imm8 & 0x10) sum += a_d[0] * b_d[0];
                if (imm8 & 0x20) sum += a_d[1] * b_d[1];
                dst_d[0] = (imm8 & 0x01) ? sum : 0.0;
                dst_d[1] = (imm8 & 0x02) ? sum : 0.0;
                std::memcpy(out, dst_d, 16);
            }
        } else if (op3 == 0x00) { // pshufb: controle de alta bit zera o byte
            for (int i = 0; i < 16; ++i) out[i] = b[i] & 0x80 ? 0 : a[b[i] & 15];
        } else if (op3 == 0x01 || op3 == 0x02 || op3 == 0x03) {
            // PHADDW (0x01), PHADDD (0x02), PHADDSW (0x03)
            if (op3 == 0x02) {
                std::uint32_t a32[4], b32[4], out32[4];
                std::memcpy(a32, a, 16);
                std::memcpy(b32, b, 16);
                out32[0] = a32[0] + a32[1];
                out32[1] = a32[2] + a32[3];
                out32[2] = b32[0] + b32[1];
                out32[3] = b32[2] + b32[3];
                std::memcpy(out, out32, 16);
            } else if (op3 == 0x01) {
                std::uint16_t a16[8], b16[8], out16[8];
                std::memcpy(a16, a, 16);
                std::memcpy(b16, b, 16);
                for (int i = 0; i < 4; ++i) out16[i] = a16[i * 2] + a16[i * 2 + 1];
                for (int i = 0; i < 4; ++i) out16[4 + i] = b16[i * 2] + b16[i * 2 + 1];
                std::memcpy(out, out16, 16);
            } else {
                std::int16_t a16[8], b16[8], out16[8];
                std::memcpy(a16, a, 16);
                std::memcpy(b16, b, 16);
                auto sat16 = [](std::int32_t v) -> std::int16_t {
                    return static_cast<std::int16_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
                };
                for (int i = 0; i < 4; ++i) out16[i] = sat16(static_cast<std::int32_t>(a16[i * 2]) + a16[i * 2 + 1]);
                for (int i = 0; i < 4; ++i) out16[4 + i] = sat16(static_cast<std::int32_t>(b16[i * 2]) + b16[i * 2 + 1]);
                std::memcpy(out, out16, 16);
            }
        } else if (op3 == 0x40) { // pmulld (produto modular de 32 bits)
            for (int i = 0; i < 4; ++i) {
                std::uint32_t x, y;
                std::memcpy(&x, a + i * 4, 4); std::memcpy(&y, b + i * 4, 4);
                const std::uint32_t z = x * y;
                std::memcpy(out + i * 4, &z, 4);
            }
        } else if (op3 >= 0x38 && op3 <= 0x3F) {
            const bool is_max = (op3 >= 0x3C);
            const int elem_width = (op3 == 0x38 || op3 == 0x3C) ? 1
                                 : (op3 == 0x3A || op3 == 0x3E) ? 2 : 4;
            const bool is_signed = (op3 == 0x38 || op3 == 0x39 || op3 == 0x3C || op3 == 0x3D);
            for (int i = 0; i < 16; i += elem_width) {
                if (elem_width == 1) {
                    if (is_signed) {
                        std::int8_t x = static_cast<std::int8_t>(a[i]), y = static_cast<std::int8_t>(b[i]);
                        out[i] = static_cast<std::uint8_t>(is_max ? std::max(x, y) : std::min(x, y));
                    } else {
                        std::uint8_t x = a[i], y = b[i];
                        out[i] = is_max ? std::max(x, y) : std::min(x, y);
                    }
                } else if (elem_width == 2) {
                    std::uint16_t x16, y16;
                    std::memcpy(&x16, a + i, 2); std::memcpy(&y16, b + i, 2);
                    if (is_signed) {
                        std::int16_t x = static_cast<std::int16_t>(x16), y = static_cast<std::int16_t>(y16);
                        std::int16_t z = is_max ? std::max(x, y) : std::min(x, y);
                        std::memcpy(out + i, &z, 2);
                    } else {
                        std::uint16_t z = is_max ? std::max(x16, y16) : std::min(x16, y16);
                        std::memcpy(out + i, &z, 2);
                    }
                } else {
                    std::uint32_t x32, y32;
                    std::memcpy(&x32, a + i, 4); std::memcpy(&y32, b + i, 4);
                    if (is_signed) {
                        std::int32_t x = static_cast<std::int32_t>(x32), y = static_cast<std::int32_t>(y32);
                        std::int32_t z = is_max ? std::max(x, y) : std::min(x, y);
                        std::memcpy(out + i, &z, 4);
                    } else {
                        std::uint32_t z = is_max ? std::max(x32, y32) : std::min(x32, y32);
                        std::memcpy(out + i, &z, 4);
                    }
                }
            }
        } else if (op3 >= 0x1C && op3 <= 0x1E) {
            const int elem_width = (op3 == 0x1C) ? 1 : (op3 == 0x1D) ? 2 : 4;
            for (int i = 0; i < 16; i += elem_width) {
                if (elem_width == 1) {
                    std::int8_t v = static_cast<std::int8_t>(b[i]);
                    out[i] = static_cast<std::uint8_t>(v < 0 ? -v : v);
                } else if (elem_width == 2) {
                    std::int16_t v; std::memcpy(&v, b + i, 2);
                    v = static_cast<std::int16_t>(v < 0 ? -v : v);
                    std::memcpy(out + i, &v, 2);
                } else {
                    std::int32_t v; std::memcpy(&v, b + i, 4);
                    v = v < 0 ? -v : v;
                    std::memcpy(out + i, &v, 4);
                }
            }
        } else if (op3 == 0x29 || op3 == 0x37) {
            for (int i = 0; i < 2; ++i) {
                std::uint64_t x64, y64;
                std::memcpy(&x64, a + i * 8, 8); std::memcpy(&y64, b + i * 8, 8);
                std::uint64_t z = 0;
                if (op3 == 0x29) {
                    z = (x64 == y64) ? ~0ULL : 0ULL;
                } else {
                    z = (static_cast<std::int64_t>(x64) > static_cast<std::int64_t>(y64)) ? ~0ULL : 0ULL;
                }
                std::memcpy(out + i * 8, &z, 8);
            }
        } else if (op3 == 0x28) {
            for (int i = 0; i < 2; ++i) {
                std::int32_t x32, y32;
                std::memcpy(&x32, a + i * 8, 4); std::memcpy(&y32, b + i * 8, 4);
                std::uint64_t z = static_cast<std::uint64_t>(static_cast<std::int64_t>(x32) * static_cast<std::int64_t>(y32));
                std::memcpy(out + i * 8, &z, 8);
            }
        } else if (op3 == 0x2B) {
            std::uint16_t out16[8];
            for (int i = 0; i < 4; ++i) {
                std::int32_t v; std::memcpy(&v, a + i * 4, 4);
                out16[i] = static_cast<std::uint16_t>(v < 0 ? 0 : v > 65535 ? 65535 : v);
            }
            for (int i = 0; i < 4; ++i) {
                std::int32_t v; std::memcpy(&v, b + i * 4, 4);
                out16[4 + i] = static_cast<std::uint16_t>(v < 0 ? 0 : v > 65535 ? 65535 : v);
            }
            std::memcpy(out, out16, 16);
        } else if (op3 == 0x2A) {
            std::memcpy(out, b, 16);
        }
        std::memcpy(s.xmm[rg], out, 16);
        return;
    }
    if (op2 == 0x31) { // RDTSC: contador monotônico determinístico
        s.gpr[0] = tsc & 0xFFFFFFFFULL;
        s.gpr[2] = (tsc >> 32) & 0xFFFFFFFFULL;
        return;
    }
    if (op2 == 0x01) { // RDTSCP / XGETBV
        const std::uint8_t mrm = u8();
        if (mrm == 0xF9) { // RDTSCP
            s.gpr[0] = tsc & 0xFFFFFFFFULL;
            s.gpr[2] = (tsc >> 32) & 0xFFFFFFFFULL;
            s.gpr[1] = 0; // processor ID
            return;
        }
        if (mrm == 0xD0) { // XGETBV
            s.gpr[0] = 7; s.gpr[2] = 0;
            return;
        }
        throw Unsupported{};
    }
    if (op2 == 0xA2) { // CPUID: perfil conservador Jaguar, sem anunciar AVX sem XSAVE
        const std::uint32_t leaf = static_cast<std::uint32_t>(s.gpr[0]);
        const std::uint32_t sub = static_cast<std::uint32_t>(s.gpr[1]);
        std::uint32_t a = 0, b = 0, c = 0, d = 0;
        if (leaf == 0) { a = 7; b = 0x68747541; d = 0x69746e65; c = 0x444d4163; }
        else if (leaf == 1) {
            a = 0x00700f01; c = (1u << 0) | (1u << 9) | (1u << 19) | (1u << 20) | (1u << 23);
            d = (1u << 4) | (1u << 15) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26);
        } else if (leaf == 7 && sub == 0) b = (1u << 3); // BMI1
        else if (leaf == 0x80000000u) a = 0x80000001u;
        else if (leaf == 0x80000001u) { c = (1u << 5); d = (1u << 29); }
        s.gpr[0] = a; s.gpr[3] = b; s.gpr[1] = c; s.gpr[2] = d;
        return;
    }
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
        check_lock(true, &rm);
        const std::uint64_t dest = rd_rm(rm, sz), acc = rd_reg(0, sz);
        alu(7, acc, dest, sz);
        if (s.rflags & FLAG_ZF) wr_rm(rm, sz, rd_reg(rg, sz));
        else wr_reg(0, sz, dest);
        return;
    }
    case 0xC0: case 0xC1: {  // xadd
        const int sz = (op2 & 1) ? opsz : 1;
        const Rm rm = decode_rm(rg, 0);
        check_lock(true, &rm);
        const std::uint64_t d = rd_rm(rm, sz), r = rd_reg(rg, sz);
        const std::uint64_t t = alu(0, d, r, sz);
        wr_reg(rg, sz, d);
        wr_rm(rm, sz, t);
        return;
    }
    case 0xC7: {  // CMPXCHG8B / CMPXCHG16B
        const Rm rm = decode_rm(rg, 0);
        const int sub = rg & 7;
        if (sub == 1) { // cmpxchg8b / cmpxchg16b
            if (rm.reg) throw UdEx{};
            check_lock(true, &rm);
            if (opsz == 8) { // cmpxchg16b m128
                const std::uint64_t mem_lo = rd_mem(rm.addr, 8);
                const std::uint64_t mem_hi = rd_mem(rm.addr + 8, 8);
                const std::uint64_t rax = rd_reg(0, 8);
                const std::uint64_t rdx = rd_reg(2, 8);
                if (mem_lo == rax && mem_hi == rdx) {
                    wr_mem(rm.addr, 8, rd_reg(3, 8)); // RBX
                    wr_mem(rm.addr + 8, 8, rd_reg(1, 8)); // RCX
                    set_flag(FLAG_ZF, true);
                } else {
                    wr_reg(0, 8, mem_lo);
                    wr_reg(2, 8, mem_hi);
                    set_flag(FLAG_ZF, false);
                }
                return;
            } else { // cmpxchg8b m64
                const std::uint64_t mem = rd_mem(rm.addr, 8);
                const std::uint32_t eax = static_cast<std::uint32_t>(rd_reg(0, 4));
                const std::uint32_t edx = static_cast<std::uint32_t>(rd_reg(2, 4));
                const std::uint64_t edx_eax = (static_cast<std::uint64_t>(edx) << 32) | eax;
                if (mem == edx_eax) {
                    const std::uint32_t ebx = static_cast<std::uint32_t>(rd_reg(3, 4));
                    const std::uint32_t ecx = static_cast<std::uint32_t>(rd_reg(1, 4));
                    const std::uint64_t ecx_ebx = (static_cast<std::uint64_t>(ecx) << 32) | ebx;
                    wr_mem(rm.addr, 8, ecx_ebx);
                    set_flag(FLAG_ZF, true);
                } else {
                    wr_reg(0, 4, static_cast<std::uint32_t>(mem & 0xFFFFFFFF));
                    wr_reg(2, 4, static_cast<std::uint32_t>(mem >> 32));
                    set_flag(FLAG_ZF, false);
                }
                return;
            }
        }
        throw Unsupported{};
    }
    case 0xBA: {  // bt/bts/btr/btc r/m, imm8
        const Rm rm = decode_rm(rg, 1);
        const unsigned bit = u8() & (opsz * 8 - 1);
        const std::uint64_t v = rd_rm(rm, opsz);
        const int sub = rg & 7;
        if (sub < 4) throw Unsupported{};
        if (sub == 4) check_lock(false);
        else check_lock(true, &rm);
        set_flag(FLAG_CF, ((v >> bit) & 1) != 0);
        if (sub == 5) wr_rm(rm, opsz, v | (1ULL << bit));
        else if (sub == 6) wr_rm(rm, opsz, v & ~(1ULL << bit));
        else if (sub == 7) wr_rm(rm, opsz, v ^ (1ULL << bit));
        return;
    }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {
        const Rm rm = decode_rm(rg, 0);
        if (op2 == 0xA3) check_lock(false);
        else check_lock(true, &rm);
        if (rm.reg) {
            const unsigned bit = static_cast<unsigned>(rd_reg(rg, opsz)) & (opsz * 8 - 1);
            const std::uint64_t v = rd_reg(rm.r, opsz);
            set_flag(FLAG_CF, ((v >> bit) & 1) != 0);
            if (op2 == 0xAB) wr_reg(rm.r, opsz, v | (1ULL << bit));
            else if (op2 == 0xB3) wr_reg(rm.r, opsz, v & ~(1ULL << bit));
            else if (op2 == 0xBB) wr_reg(rm.r, opsz, v ^ (1ULL << bit));
        } else {
            std::int64_t bit_offset = 0;
            if (opsz == 2) bit_offset = static_cast<std::int16_t>(rd_reg(rg, 2));
            else if (opsz == 4) bit_offset = static_cast<std::int32_t>(rd_reg(rg, 4));
            else bit_offset = static_cast<std::int64_t>(rd_reg(rg, 8));

            const std::uint64_t target_addr = rm.addr + static_cast<std::uint64_t>(bit_offset >> 3);
            const unsigned bit = static_cast<unsigned>(bit_offset & 7);
            std::uint8_t byte_val = 0;
            if (m.load(target_addr, &byte_val, 1) != MemoryError::ok) throw Fault{target_addr};
            set_flag(FLAG_CF, ((byte_val >> bit) & 1) != 0);
            if (op2 == 0xAB) {
                byte_val |= static_cast<std::uint8_t>(1u << bit);
                if (m.store(target_addr, &byte_val, 1) != MemoryError::ok) throw Fault{target_addr};
            } else if (op2 == 0xB3) {
                byte_val &= static_cast<std::uint8_t>(~(1u << bit));
                if (m.store(target_addr, &byte_val, 1) != MemoryError::ok) throw Fault{target_addr};
            } else if (op2 == 0xBB) {
                byte_val ^= static_cast<std::uint8_t>(1u << bit);
                if (m.store(target_addr, &byte_val, 1) != MemoryError::ok) throw Fault{target_addr};
            }
        }
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
    case 0xAE: {
        const Rm rm = decode_rm(rg, 0);
        const int sub = rg & 7;
        if (sub == 2) {
            // LDMXCSR m32
            s.mxcsr = static_cast<std::uint32_t>(rd_rm(rm, 4));
        } else if (sub == 3) {
            // STMXCSR m32: grava MXCSR
            wr_rm(rm, 4, s.mxcsr);
        } else if (sub == 0) {
            // FXSAVE m512byte: salva estado FPU/MMX/SSE
            if (!rm.reg) {
                std::uint8_t fx[512] = {};
                fx[0] = 0x7F; fx[1] = 0x03; // FCW = 0x037F
                fx[24] = 0x80; fx[25] = 0x1F; // MXCSR = 0x1F80
                fx[28] = 0xFF; fx[29] = 0xFF; // MXCSR_MASK = 0xFFFF
                for (int i = 0; i < 16; ++i) {
                    std::memcpy(&fx[160 + i * 16], s.xmm[i], 16);
                }
                m.store(rm.addr, fx, sizeof(fx));
            }
        } else if (sub == 1) {
            // FXRSTOR m512byte: restaura estado
            if (!rm.reg) {
                std::uint8_t fx[512] = {};
                if (m.load(rm.addr, fx, sizeof(fx)) == MemoryError::ok) {
                    for (int i = 0; i < 16; ++i) {
                        std::memcpy(s.xmm[i], &fx[160 + i * 16], 16);
                    }
                }
            }
        }
        // LFENCE, MFENCE, SFENCE, CLFLUSH
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
    // Mandatory prefixes select different instructions, not optional decorations.
    if ((rep || repne) && pd) throw Unsupported{};
    if ((rep || repne) && op2 != 0x10 && op2 != 0x11 && op2 != 0x6F &&
        op2 != 0x7F && op2 != 0x7E && op2 != 0x2A && op2 != 0x2C &&
        op2 != 0x2D && op2 != 0x12 && op2 != 0x16 &&
        !(op2 >= 0x51 && op2 <= 0x5F)) throw Unsupported{};
    if (repne && (op2 == 0x6F || op2 == 0x7F || op2 == 0x7E)) throw Unsupported{};
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
    case 0x12: {
        const Rm rm = decode_rm(rg, 0);
        if (rep) { // movsldup xmm1, xmm2/m128
            std::uint64_t src[2]; rd_xmm_rm(rm, src);
            s.xmm[rg][0] = (src[0] & 0xFFFFFFFFULL) | ((src[0] & 0xFFFFFFFFULL) << 32);
            s.xmm[rg][1] = (src[1] & 0xFFFFFFFFULL) | ((src[1] & 0xFFFFFFFFULL) << 32);
        } else if (repne) { // movddup xmm1, xmm2/m64
            const std::uint64_t val = rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8);
            s.xmm[rg][0] = val; s.xmm[rg][1] = val;
        } else if (rm.reg) { // movhlps xmm1, xmm2
            if (pd) throw Unsupported{};
            s.xmm[rg][0] = s.xmm[rm.r][1];
        } else { // movlps / movlpd xmm1, m64
            s.xmm[rg][0] = rd_mem(rm.addr, 8);
        }
        return;
    }
    case 0x13: { // movlps / movlpd m64, xmm1
        const Rm rm = decode_rm(rg, 0);
        if (rm.reg || rep || repne) throw Unsupported{};
        wr_mem(rm.addr, 8, s.xmm[rg][0]);
        return;
    }
    case 0x14: case 0x15: { // unpcklps/pd, unpckhps/pd
        const Rm rm = decode_rm(rg, 0);
        std::uint64_t src[2]; rd_xmm_rm(rm, src);
        const bool is_hi = (op2 == 0x15);
        if (pd) { // pd: 64-bit doubles
            if (is_hi) { s.xmm[rg][0] = s.xmm[rg][1]; s.xmm[rg][1] = src[1]; }
            else { s.xmm[rg][1] = src[0]; }
        } else { // ps: 32-bit floats
            const std::uint32_t a0 = static_cast<std::uint32_t>(s.xmm[rg][0]), a1 = static_cast<std::uint32_t>(s.xmm[rg][0] >> 32);
            const std::uint32_t a2 = static_cast<std::uint32_t>(s.xmm[rg][1]), a3 = static_cast<std::uint32_t>(s.xmm[rg][1] >> 32);
            const std::uint32_t b0 = static_cast<std::uint32_t>(src[0]), b1 = static_cast<std::uint32_t>(src[0] >> 32);
            const std::uint32_t b2 = static_cast<std::uint32_t>(src[1]), b3 = static_cast<std::uint32_t>(src[1] >> 32);
            if (is_hi) {
                s.xmm[rg][0] = static_cast<std::uint64_t>(a2) | (static_cast<std::uint64_t>(b2) << 32);
                s.xmm[rg][1] = static_cast<std::uint64_t>(a3) | (static_cast<std::uint64_t>(b3) << 32);
            } else {
                s.xmm[rg][0] = static_cast<std::uint64_t>(a0) | (static_cast<std::uint64_t>(b0) << 32);
                s.xmm[rg][1] = static_cast<std::uint64_t>(a1) | (static_cast<std::uint64_t>(b1) << 32);
            }
        }
        return;
    }
    case 0x16: {
        const Rm rm = decode_rm(rg, 0);
        if (rep) { // movshdup xmm1, xmm2/m128
            std::uint64_t src[2]; rd_xmm_rm(rm, src);
            s.xmm[rg][0] = (src[0] >> 32) | ((src[0] >> 32) << 32);
            s.xmm[rg][1] = (src[1] >> 32) | ((src[1] >> 32) << 32);
        } else if (rm.reg) { // movlhps xmm1, xmm2
            if (pd || repne) throw Unsupported{};
            s.xmm[rg][1] = s.xmm[rm.r][0];
        } else { // movhps / movhpd xmm1, m64
            if (repne) throw Unsupported{};
            s.xmm[rg][1] = rd_mem(rm.addr, 8);
        }
        return;
    }
    case 0x17: { // movhps / movhpd m64, xmm1
        const Rm rm = decode_rm(rg, 0);
        if (rm.reg || rep || repne) throw Unsupported{};
        wr_mem(rm.addr, 8, s.xmm[rg][1]);
        return;
    }
    case 0x28: case 0x29: case 0x6F: case 0x7F: case 0x2B: case 0xE7: {
        if ((op2 == 0x6F || op2 == 0x7F) && !(p66 || rep)) throw Unsupported{};  // MMX
        if (op2 == 0xE7 && !p66) throw Unsupported{};
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
    case 0x50: { // movmskps / movmskpd
        const Rm rm = decode_rm(rg, 0);
        if (!rm.reg) throw UdEx{};
        unsigned mask = 0;
        if (!pd) { // movmskps
            for (int i = 0; i < 4; ++i) {
                const std::uint64_t qw = s.xmm[rm.r][i / 2];
                const unsigned sign_bit = static_cast<unsigned>((qw >> ((i % 2) * 32 + 31)) & 1);
                mask |= (sign_bit << i);
            }
        } else { // movmskpd
            for (int i = 0; i < 2; ++i) {
                const std::uint64_t qw = s.xmm[rm.r][i];
                const unsigned sign_bit = static_cast<unsigned>((qw >> 63) & 1);
                mask |= (sign_bit << i);
            }
        }
        wr_reg(rg, 4, mask);
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
    case 0x58: case 0x59: case 0x5C: case 0x5E: case 0x51: case 0x5D: case 0x5F: case 0x53: case 0x52: {
        const Rm rm = decode_rm(rg, 0);
        const bool scalar = rep || repne;
        const bool dbl = repne || (!scalar && pd);
        const int width = dbl ? 8 : 4;
        const int lanes = scalar ? 1 : 16 / width;
        std::uint64_t src[2] = {};
        if (rm.reg) { src[0] = s.xmm[rm.r][0]; src[1] = s.xmm[rm.r][1]; }
        else if (scalar) src[0] = rd_mem(rm.addr, width);
        else rd_xmm_rm(rm, src);
        if (dbl && (op2 == 0x53 || op2 == 0x52)) throw Unsupported{};
        for (int i = 0; i < lanes; ++i) {
            const int word = (i * width) / 8, shift = (i * width % 8) * 8;
            const std::uint64_t mask = mask_of(width);
            const std::uint64_t a = (s.xmm[rg][word] >> shift) & mask, b = (src[word] >> shift) & mask;
            std::uint64_t result = 0;
            if (dbl) {
                const double x = f64(a), y = f64(b);
                const double z = op2 == 0x58 ? x + y : op2 == 0x59 ? x * y : op2 == 0x5C ? x - y
                    : op2 == 0x5E ? x / y : op2 == 0x51 ? std::sqrt(y)
                    : op2 == 0x5D ? (std::isnan(x) || std::isnan(y) || x == y ? y : (x < y ? x : y))
                    : (std::isnan(x) || std::isnan(y) || x == y ? y : (x > y ? x : y));
                result = bits64(z);
            } else {
                const float x = f32(a), y = f32(b);
                const float z = op2 == 0x58 ? x + y : op2 == 0x59 ? x * y : op2 == 0x5C ? x - y
                    : op2 == 0x5E ? x / y : op2 == 0x51 ? std::sqrt(y) : op2 == 0x53 ? 1.0f / y
                    : op2 == 0x52 ? 1.0f / std::sqrt(y)
                    : op2 == 0x5D ? (std::isnan(x) || std::isnan(y) || x == y ? y : (x < y ? x : y))
                    : (std::isnan(x) || std::isnan(y) || x == y ? y : (x > y ? x : y));
                result = bits32(z);
            }
            s.xmm[rg][word] = (s.xmm[rg][word] & ~(mask << shift)) | ((result & mask) << shift);
        }
        return;
    }
    case 0x70: case 0xC6: {  // pshufd / shufps / shufpd
        const Rm rm = decode_rm(rg, 1);
        const unsigned imm = u8();
        std::uint64_t src[2]; rd_xmm_rm(rm, src);
        const std::uint64_t dst[2] = {s.xmm[rg][0], s.xmm[rg][1]};
        if (op2 == 0x70 && pd) {
            for (int i = 0; i < 4; ++i) {
                const int idx = (imm >> (i * 2)) & 3;
                const std::uint64_t v = (src[idx / 2] >> ((idx % 2) * 32)) & 0xFFFFFFFFULL;
                const int j = i / 2, sh = (i % 2) * 32;
                s.xmm[rg][j] = (s.xmm[rg][j] & ~(0xFFFFFFFFULL << sh)) | (v << sh);
            }
        } else if (op2 == 0xC6) {
            if (pd) {
                s.xmm[rg][0] = (imm & 1) ? dst[1] : dst[0];
                s.xmm[rg][1] = (imm & 2) ? src[1] : src[0];
            } else {
                for (int i = 0; i < 4; ++i) {
                    const int idx = (imm >> (i * 2)) & 3;
                    const std::uint64_t* from = i < 2 ? dst : src;
                    const std::uint64_t v = (from[idx / 2] >> ((idx % 2) * 32)) & 0xFFFFFFFFULL;
                    const int j = i / 2, sh = (i % 2) * 32;
                    s.xmm[rg][j] = (s.xmm[rg][j] & ~(0xFFFFFFFFULL << sh)) | (v << sh);
                }
            }
        } else throw Unsupported{};
        return;
    }
    case 0xC2: {  // cmpps / cmppd / cmpss / cmpsd
        const Rm rm = decode_rm(rg, 1);
        const unsigned imm = u8();
        const bool scalar = rep || repne;
        const bool dbl = repne || (!scalar && pd);
        const int width = dbl ? 8 : 4;
        const int lanes = scalar ? 1 : 16 / width;
        std::uint64_t src[2] = {};
        if (rm.reg) { src[0] = s.xmm[rm.r][0]; src[1] = s.xmm[rm.r][1]; }
        else if (scalar) src[0] = rd_mem(rm.addr, width);
        else rd_xmm_rm(rm, src);
        for (int i = 0; i < lanes; ++i) {
            const int word = (i * width) / 8, shift = (i * width % 8) * 8;
            const std::uint64_t mask = mask_of(width);
            const std::uint64_t a = (s.xmm[rg][word] >> shift) & mask, b = (src[word] >> shift) & mask;
            const std::uint64_t res = dbl
                ? (eval_cmp_pred(f64(a), f64(b), imm) ? mask : 0ULL)
                : (eval_cmp_pred(f32(a), f32(b), imm) ? mask : 0ULL);
            s.xmm[rg][word] = (s.xmm[rg][word] & ~(mask << shift)) | ((res & mask) << shift);
        }
        return;
    }
    case 0x5A: { // cvtss2sd / cvtsd2ss / cvtps2pd / cvtpd2ps
        const Rm rm = decode_rm(rg, 0);
        if (rep) { // cvtss2sd xmm1, xmm2/m32
            const float src = f32(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 4));
            s.xmm[rg][0] = bits64(static_cast<double>(src));
        } else if (repne) { // cvtsd2ss xmm1, xmm2/m64
            const double src = f64(rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8));
            s.xmm[rg][0] = (s.xmm[rg][0] & ~0xFFFFFFFFULL) | bits32(static_cast<float>(src));
        } else if (pd) { // cvtpd2ps xmm1, xmm2/m128
            std::uint64_t raw[2] = {};
            rd_xmm_rm(rm, raw);
            const float f0 = static_cast<float>(f64(raw[0]));
            const float f1 = static_cast<float>(f64(raw[1]));
            s.xmm[rg][0] = bits32(f0) | (static_cast<std::uint64_t>(bits32(f1)) << 32);
            s.xmm[rg][1] = 0;
        } else { // cvtps2pd xmm1, xmm2/m64
            const std::uint64_t raw = rm.reg ? s.xmm[rm.r][0] : rd_mem(rm.addr, 8);
            s.xmm[rg][0] = bits64(static_cast<double>(f32(raw & 0xFFFFFFFFULL)));
            s.xmm[rg][1] = bits64(static_cast<double>(f32(raw >> 32)));
        }
        return;
    }
    case 0x5B: { // cvtdq2ps / cvtps2dq / cvttps2dq
        const Rm rm = decode_rm(rg, 0);
        std::uint64_t raw[2] = {};
        rd_xmm_rm(rm, raw);
        std::uint32_t src[4] = {
            static_cast<std::uint32_t>(raw[0]), static_cast<std::uint32_t>(raw[0] >> 32),
            static_cast<std::uint32_t>(raw[1]), static_cast<std::uint32_t>(raw[1] >> 32)
        };
        std::uint32_t out[4] = {};
        if (!rep && !repne && !pd) { // cvtdq2ps
            for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint32_t>(bits32(static_cast<float>(static_cast<std::int32_t>(src[i]))));
        } else if (rep || pd) { // cvttps2dq (rep) or cvtps2dq (pd)
            for (int i = 0; i < 4; ++i) {
                const float f = f32(src[i]);
                std::int32_t val = 0;
                if (std::isnan(f) || f >= 2147483648.0f || f < -2147483648.0f) val = INT32_MIN;
                else if (rep) val = static_cast<std::int32_t>(f);
                else val = static_cast<std::int32_t>(std::nearbyint(f));
                out[i] = static_cast<std::uint32_t>(val);
            }
        } else throw Unsupported{};
        s.xmm[rg][0] = static_cast<std::uint64_t>(out[0]) | (static_cast<std::uint64_t>(out[1]) << 32);
        s.xmm[rg][1] = static_cast<std::uint64_t>(out[2]) | (static_cast<std::uint64_t>(out[3]) << 32);
        return;
    }
    case 0xD7: {  // pmovmskb
        if (!pd) throw Unsupported{};
        const Rm rm = decode_rm(rg, 0);
        if (!rm.reg) throw UdEx{};
        unsigned bits = 0;
        for (int i = 0; i < 16; ++i)
            bits |= static_cast<unsigned>((s.xmm[rm.r][i / 8] >> ((i % 8) * 8 + 7)) & 1) << i;
        wr_reg(rg, 4, bits);
        return;
    }
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D:
    case 0x64: case 0x65: case 0x66:
    case 0xFC: case 0xFD: case 0xFE: case 0xD4:
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0x74: case 0x75: case 0x76: case 0xD5: {
        if (!pd) throw Unsupported{};
        const Rm rm = decode_rm(rg, 0);
        std::uint64_t src[2]; rd_xmm_rm(rm, src);
        std::uint8_t a[16], b[16], out[16] = {};
        std::memcpy(a, s.xmm[rg], 16); std::memcpy(b, src, 16);
        const bool pack = op2 == 0x63 || op2 == 0x67 || op2 == 0x6B;
        const bool unpack = (op2 >= 0x60 && op2 <= 0x62) || op2 == 0x6C || (op2 >= 0x68 && op2 <= 0x6A) || op2 == 0x6D;
        const int width = (op2 == 0xD4 || op2 == 0xFB || op2 == 0x6C || op2 == 0x6D) ? 8
                        : (op2 == 0xFE || op2 == 0xFA || op2 == 0x76 || op2 == 0x66) ? 4
                        : (op2 == 0xFD || op2 == 0xF9 || op2 == 0x75 || op2 == 0xD5 || op2 == 0x61 || op2 == 0x69 || op2 == 0x65) ? 2 : 1;
        if (pack) {
            if (op2 == 0x63) { // PACKSSWB
                for (int i = 0; i < 8; ++i) {
                    std::int16_t v = 0;
                    std::memcpy(&v, a + i * 2, 2);
                    out[i] = static_cast<std::uint8_t>(v > 127 ? 127 : v < -128 ? -128 : v);
                }
                for (int i = 0; i < 8; ++i) {
                    std::int16_t v = 0;
                    std::memcpy(&v, b + i * 2, 2);
                    out[8 + i] = static_cast<std::uint8_t>(v > 127 ? 127 : v < -128 ? -128 : v);
                }
            } else if (op2 == 0x67) { // PACKUSWB
                for (int i = 0; i < 8; ++i) {
                    std::int16_t v = 0;
                    std::memcpy(&v, a + i * 2, 2);
                    out[i] = static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
                }
                for (int i = 0; i < 8; ++i) {
                    std::int16_t v = 0;
                    std::memcpy(&v, b + i * 2, 2);
                    out[8 + i] = static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
                }
            } else if (op2 == 0x6B) { // PACKSSDW
                std::int16_t* dst16 = reinterpret_cast<std::int16_t*>(out);
                for (int i = 0; i < 4; ++i) {
                    std::int32_t v = 0;
                    std::memcpy(&v, a + i * 4, 4);
                    dst16[i] = static_cast<std::int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
                for (int i = 0; i < 4; ++i) {
                    std::int32_t v = 0;
                    std::memcpy(&v, b + i * 4, 4);
                    dst16[4 + i] = static_cast<std::int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
            }
        } else if (unpack) {
            const int first = ((op2 >= 0x68 && op2 <= 0x6A) || op2 == 0x6D) ? 8 : 0;
            for (int i = 0; i < 8 / width; ++i) {
                std::memcpy(out + 2 * i * width, a + first + i * width, width);
                std::memcpy(out + (2 * i + 1) * width, b + first + i * width, width);
            }
        } else {
            for (int i = 0; i < 16; i += width) {
                std::uint64_t x = 0, y = 0, z = 0;
                std::memcpy(&x, a + i, width); std::memcpy(&y, b + i, width);
                if (op2 >= 0x74 && op2 <= 0x76) z = x == y ? mask_of(width) : 0;
                else if (op2 >= 0x64 && op2 <= 0x66) z = (sext(x, width) > sext(y, width)) ? mask_of(width) : 0;
                else if (op2 == 0xD5) z = x * y;
                else if (op2 == 0xFC || op2 == 0xFD || op2 == 0xFE || op2 == 0xD4) z = x + y;
                else z = x - y;
                std::memcpy(out + i, &z, width);
            }
        }
        std::memcpy(s.xmm[rg], out, 16);
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
    case 0x71: case 0x72: case 0x73: {
        if (!pd) throw Unsupported{};
        const Rm rm = decode_rm(rg, 1);
        const int sub = rg & 7;
        const unsigned count = u8();
        std::uint64_t source[2] = {}, dest[2] = {};
        if (rm.reg) { source[0] = s.xmm[rm.r][0]; source[1] = s.xmm[rm.r][1]; }
        else { rd_xmm_rm(rm, source); }

        if (op2 == 0x71) {
            std::uint16_t words[8];
            std::memcpy(words, source, 16);
            for (int i = 0; i < 8; ++i) {
                if (sub == 2) words[i] = (count >= 16) ? 0 : (words[i] >> count);
                else if (sub == 4) {
                    std::int16_t sw = static_cast<std::int16_t>(words[i]);
                    words[i] = static_cast<std::uint16_t>(count >= 16 ? (sw < 0 ? -1 : 0) : (sw >> count));
                }
                else if (sub == 6) words[i] = (count >= 16) ? 0 : (words[i] << count);
                else throw Unsupported{};
            }
            std::memcpy(dest, words, 16);
        } else if (op2 == 0x72) {
            std::uint32_t dwords[4];
            std::memcpy(dwords, source, 16);
            for (int i = 0; i < 4; ++i) {
                if (sub == 2) dwords[i] = (count >= 32) ? 0 : (dwords[i] >> count);
                else if (sub == 4) {
                    std::int32_t sd = static_cast<std::int32_t>(dwords[i]);
                    dwords[i] = static_cast<std::uint32_t>(count >= 32 ? (sd < 0 ? -1 : 0) : (sd >> count));
                }
                else if (sub == 6) dwords[i] = (count >= 32) ? 0 : (dwords[i] << count);
                else throw Unsupported{};
            }
            std::memcpy(dest, dwords, 16);
        } else if (op2 == 0x73) {
            if (sub == 2) {
                dest[0] = (count >= 64) ? 0 : (source[0] >> count);
                dest[1] = (count >= 64) ? 0 : (source[1] >> count);
            } else if (sub == 6) {
                dest[0] = (count >= 64) ? 0 : (source[0] << count);
                dest[1] = (count >= 64) ? 0 : (source[1] << count);
            } else if (sub == 3) { // PSRLDQ
                std::uint8_t b_in[16] = {}, b_out[16] = {};
                std::memcpy(b_in, source, 16);
                for (unsigned b = 0; b < 16; ++b) b_out[b] = (b + count < 16) ? b_in[b + count] : 0;
                std::memcpy(dest, b_out, 16);
            } else if (sub == 7) { // PSLLDQ
                std::uint8_t b_in[16] = {}, b_out[16] = {};
                std::memcpy(b_in, source, 16);
                for (unsigned b = 0; b < 16; ++b) b_out[b] = (b >= count) ? b_in[b - count] : 0;
                std::memcpy(dest, b_out, 16);
            } else throw Unsupported{};
        }
        if (rm.reg) { s.xmm[rm.r][0] = dest[0]; s.xmm[rm.r][1] = dest[1]; }
        else { wr_xmm_rm(rm, dest); }
        return;
    }
    case 0xD1: case 0xD2: case 0xD3:
    case 0xE1: case 0xE2:
    case 0xF1: case 0xF2: case 0xF3: {
        if (!pd) throw Unsupported{};
        const Rm rm = decode_rm(rg, 0);
        std::uint64_t count_src[2] = {};
        if (rm.reg) { count_src[0] = s.xmm[rm.r][0]; count_src[1] = s.xmm[rm.r][1]; }
        else { rd_xmm_rm(rm, count_src); }
        const std::uint64_t count = count_src[0];
        std::uint64_t source[2] = {s.xmm[rg][0], s.xmm[rg][1]}, dest[2] = {};

        if (op2 == 0xD1 || op2 == 0xE1 || op2 == 0xF1) {
            std::uint16_t words[8];
            std::memcpy(words, source, 16);
            for (int i = 0; i < 8; ++i) {
                if (op2 == 0xD1) words[i] = (count >= 16) ? 0 : (words[i] >> count);
                else if (op2 == 0xE1) {
                    std::int16_t sw = static_cast<std::int16_t>(words[i]);
                    words[i] = static_cast<std::uint16_t>(count >= 16 ? (sw < 0 ? -1 : 0) : (sw >> count));
                } else words[i] = (count >= 16) ? 0 : (words[i] << count);
            }
            std::memcpy(dest, words, 16);
        } else if (op2 == 0xD2 || op2 == 0xE2 || op2 == 0xF2) {
            std::uint32_t dwords[4];
            std::memcpy(dwords, source, 16);
            for (int i = 0; i < 4; ++i) {
                if (op2 == 0xD2) dwords[i] = (count >= 32) ? 0 : (dwords[i] >> count);
                else if (op2 == 0xE2) {
                    std::int32_t sd = static_cast<std::int32_t>(dwords[i]);
                    dwords[i] = static_cast<std::uint32_t>(count >= 32 ? (sd < 0 ? -1 : 0) : (sd >> count));
                } else dwords[i] = (count >= 32) ? 0 : (dwords[i] << count);
            }
            std::memcpy(dest, dwords, 16);
        } else if (op2 == 0xD3 || op2 == 0xF3) {
            if (op2 == 0xD3) {
                dest[0] = (count >= 64) ? 0 : (source[0] >> count);
                dest[1] = (count >= 64) ? 0 : (source[1] >> count);
            } else {
                dest[0] = (count >= 64) ? 0 : (source[0] << count);
                dest[1] = (count >= 64) ? 0 : (source[1] << count);
            }
        }
        s.xmm[rg][0] = dest[0]; s.xmm[rg][1] = dest[1];
        return;
    }
    default: throw Unsupported{};
    }
}

void Exec::bmi1() {
    if (p66 || rep || repne || rex) throw Unsupported{};
    const std::uint8_t v1 = u8(), v2 = u8(), opcode = u8();
    if ((v1 & 31) != 2 || (v2 & 3) != 0 || (v2 & 4) != 0) throw Unsupported{};
    rr = !(v1 & 0x80); rx = !(v1 & 0x40); rb = !(v1 & 0x20);
    const int sz = (v2 & 0x80) ? 8 : 4;
    const int vvvv = (~(v2 >> 3)) & 15;
    int rg = 0;
    if (opcode == 0xF2) { // ANDN r32/r64, r32/r64, r/m32/m64
        const Rm rm = decode_rm(rg, 0);
        const std::uint64_t src1 = rd_reg(vvvv, sz);
        const std::uint64_t src2 = rd_rm(rm, sz);
        const std::uint64_t res = (~src1 & src2) & mask_of(sz);
        s.rflags &= ~(FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF);
        if (res == 0) s.rflags |= FLAG_ZF;
        if (msb_of(res, sz)) s.rflags |= FLAG_SF;
        wr_reg(rg, sz, res);
        return;
    }
    if (opcode == 0xF3) { // BLSR / BLSMSK / BLSI
        const unsigned sub = (pos < n) ? ((buf[pos] >> 3) & 7) : 0xff;
        if (sub < 1 || sub > 3) throw Unsupported{};
        const Rm rm = decode_rm(rg, 0);
        const std::uint64_t src = rd_rm(rm, sz);
        const std::uint64_t reduced = (src - 1) & mask_of(sz);
        const std::uint64_t result = sub == 1 ? (src & reduced) : sub == 2 ? (src ^ reduced) : (src & (~src + 1));
        s.rflags &= ~(FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF);
        if ((sub == 3) ? (src != 0) : (src == 0)) s.rflags |= FLAG_CF;
        if (sub != 2 && result == 0) s.rflags |= FLAG_ZF;
        if (msb_of(result, sz)) s.rflags |= FLAG_SF;
        wr_reg(vvvv, sz, result);
        return;
    }
    if (opcode == 0xF7) { // BEXTR r32/r64, r/m32/m64, r32/r64
        const Rm rm = decode_rm(rg, 0);
        const std::uint64_t src = rd_rm(rm, sz);
        const std::uint64_t ctrl = rd_reg(vvvv, sz);
        const unsigned start_bit = ctrl & 0xFF;
        const unsigned len_bit = (ctrl >> 8) & 0xFF;
        const unsigned max_bits = sz * 8;
        std::uint64_t res = 0;
        if (start_bit < max_bits && len_bit > 0) {
            const std::uint64_t shifted = src >> start_bit;
            const std::uint64_t mask = (len_bit >= max_bits) ? mask_of(sz) : ((1ULL << len_bit) - 1ULL);
            res = (shifted & mask) & mask_of(sz);
        }
        s.rflags &= ~(FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF);
        if (res == 0) s.rflags |= FLAG_ZF;
        wr_reg(rg, sz, res);
        return;
    }
    throw Unsupported{};
}

void Exec::step() {
    tsc += 2;
    // prefixes
    for (;;) {
        if (pos >= 15) throw UdEx{};
        if (pos >= n) throw Fault{start + pos};
        const std::uint8_t b = buf[pos];
        // A legacy prefix after REX cancels that REX, even if another REX follows.
        if (b == 0x66 || b == 0xF3 || b == 0xF2 || b == 0xF0 || b == 0x64 || b == 0x65 ||
            b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x67)
            rex = rw = rr = rx = rb = false;
        if (b == 0x66) { p66 = true; ++pos; }
        else if (b == 0xF3) { rep = true; repne = false; ++pos; }
        else if (b == 0xF2) { repne = true; rep = false; ++pos; }
        else if (b == 0xF0) { has_lock = true; ++pos; }
        else if (b == 0x64) { fs = true; gs = false; ++pos; }
        else if (b == 0x65) { gs = true; fs = false; ++pos; }
        else if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36) { ++pos; }
        else if (b == 0x67) { addr32 = true; ++pos; }
        else if (b >= 0x40 && b <= 0x4F) {
            rex = true; rw = b & 8; rr = b & 4; rx = b & 2; rb = b & 1; ++pos;
        } else break;
    }
    const int opsz = rw ? 8 : (p66 ? 2 : 4);
    const std::uint8_t op = u8();
    int rg = 0;

    if (op == 0xC5) { if (has_lock) throw UdEx{}; avx(false); s.rip = start + pos; return; }
    if (op == 0xC4 && pos + 1 < n && (buf[pos] & 31) == 2 && (buf[pos + 1] & 3) == 0) {
        if (has_lock) throw UdEx{};
        bmi1();
        s.rip = start + pos;
        return;
    }
    if (op == 0xC4) { if (has_lock) throw UdEx{}; avx(true); s.rip = start + pos; return; }
    if (op == 0x0F) {
        two_byte(opsz);
        if (has_lock && !lock_consumed) throw UdEx{};
        if (!branched) s.rip = start + pos;
        return;
    }

    if ((op & 0xC0) == 0 && (op & 7) < 6) {  // ALU group
        const int aluop = (op >> 3) & 7, form = op & 7;
        if (form >= 4) {
            check_lock(false);
            const int sz = (form == 4) ? 1 : opsz;
            const std::uint64_t imm = imm_of(sz == 8 ? 4 : sz);
            const std::uint64_t res = alu(aluop, rd_reg(0, sz), imm, sz);
            if (aluop != 7) wr_reg(0, sz, res);
        } else {
            const int sz = (form & 1) ? opsz : 1;
            const Rm rm = decode_rm(rg, 0);
            if (form < 2) {
                if (aluop == 7) check_lock(false);
                else check_lock(true, &rm);
                const std::uint64_t res = alu(aluop, rd_rm(rm, sz), rd_reg(rg, sz), sz);
                if (aluop != 7) wr_rm(rm, sz, res);
            } else {
                check_lock(false);
                const std::uint64_t res = alu(aluop, rd_reg(rg, sz), rd_rm(rm, sz), sz);
                if (aluop != 7) wr_reg(rg, sz, res);
            }
        }
        s.rip = start + pos;
        return;
    }

    if (op >= 0x50 && op <= 0x57) { if (has_lock) throw UdEx{}; push(s.gpr[(op & 7) | (rb ? 8 : 0)]); s.rip = start + pos; return; }
    if (op >= 0x58 && op <= 0x5F) { if (has_lock) throw UdEx{}; const std::uint64_t v = pop(); s.gpr[(op & 7) | (rb ? 8 : 0)] = v; s.rip = start + pos; return; }
    if (op >= 0x70 && op <= 0x7F) {
        if (has_lock) throw UdEx{};
        const std::int8_t rel = static_cast<std::int8_t>(u8());
        if (cond_true(op & 15, s.rflags)) { s.rip = start + pos + static_cast<std::int64_t>(rel); }
        else s.rip = start + pos;
        return;
    }
    if (op >= 0xB0 && op <= 0xB7) { if (has_lock) throw UdEx{}; wr_reg((op & 7) | (rb ? 8 : 0), 1, u8()); s.rip = start + pos; return; }
    if (op >= 0xB8 && op <= 0xBF) {
        if (has_lock) throw UdEx{};
        const int r = (op & 7) | (rb ? 8 : 0);
        if (rw) wr_reg(r, 8, u64());
        else if (p66) wr_reg(r, 2, u16());
        else wr_reg(r, 4, u32());
        s.rip = start + pos;
        return;
    }
    if (op >= 0x91 && op <= 0x97) {
        if (has_lock) throw UdEx{};
        const int r = (op & 7) | (rb ? 8 : 0);
        const std::uint64_t a = rd_reg(0, opsz), b = rd_reg(r, opsz);
        wr_reg(0, opsz, b); wr_reg(r, opsz, a);
        s.rip = start + pos;
        return;
    }

    if (has_lock) {
        switch (op) {
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x86: case 0x87:
        case 0xF6: case 0xF7:
        case 0xFE: case 0xFF:
            break;
        default:
            throw UdEx{};
        }
    }

    switch (op) {
    case 0x63: {
        const Rm rm = decode_rm(rg, 0);
        const std::uint64_t v = rd_rm(rm, 4);
        if (rw) wr_reg(rg, 8, static_cast<std::uint64_t>(sext(v, 4))); else wr_reg(rg, opsz, v);
        break;
    }
    case 0x68:
        if (p66) {
            const std::uint16_t v = u16();
            wr_mem(s.gpr[4] - 2, 2, v);
            s.gpr[4] -= 2;
        } else push(static_cast<std::uint64_t>(sext(u32(), 4)));
        break;
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
        if (aluop == 7) check_lock(false);
        else check_lock(true, &rm);
        const std::uint64_t res = alu(aluop, rd_rm(rm, sz), imm, sz);
        if (aluop != 7) wr_rm(rm, sz, res);
        break;
    }
    case 0x84: case 0x85: {
        check_lock(false);
        const int sz = (op == 0x84) ? 1 : opsz;
        const Rm rm = decode_rm(rg, 0);
        alu(4, rd_rm(rm, sz), rd_reg(rg, sz), sz);
        break;
    }
    case 0x86: case 0x87: {
        const int sz = (op == 0x86) ? 1 : opsz;
        const Rm rm = decode_rm(rg, 0);
        if (has_lock) {
            check_lock(true, &rm);
        } else if (!rm.reg) {
            bus_lock_ = std::unique_lock<std::mutex>(s_bus_lock_mutex);
        }
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
        const bool saved_fs = fs, saved_gs = gs;
        fs = gs = false;
        const Rm rm = decode_rm(rg, 0);
        fs = saved_fs; gs = saved_gs;
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
    case 0x9C: push(s.rflags | 2); break;
    case 0x9D: s.rflags = (pop() & 0x00000000003F7FD7ULL) | 2; break;
    case 0x9E: s.rflags = (s.rflags & ~0xD5ULL) | ((s.gpr[0] >> 8) & 0xD5); break;
    case 0x9F: s.gpr[0] = (s.gpr[0] & ~0xFF00ULL) | (((s.rflags & 0xD5ULL) | 2) << 8); break;
    case 0xF8: s.rflags &= ~FLAG_CF; break;
    case 0xF9: s.rflags |= FLAG_CF; break;
    case 0xFC: s.rflags &= ~FLAG_DF; break;
    case 0xFD: s.rflags |= FLAG_DF; break;
    case 0xA8: alu(4, rd_reg(0, 1), u8(), 1); break;
    case 0xA9: { const std::uint64_t imm = imm_of(opsz == 2 ? 2 : 4); alu(4, rd_reg(0, opsz), imm, opsz); break; }
    case 0xA4: case 0xA5: case 0xAA: case 0xAB:
    case 0xA6: case 0xA7: case 0xAE: case 0xAF: case 0xAC: case 0xAD: {
        const int sz = (op & 1) ? opsz : 1;
        const bool movs = (op == 0xA4 || op == 0xA5);
        const std::int64_t step = (s.rflags & FLAG_DF) ? -sz : sz;
        const bool cmps = op == 0xA6 || op == 0xA7, scas = op == 0xAE || op == 0xAF;
        const bool lods = op == 0xAC || op == 0xAD;
        std::uint64_t count = (rep || repne) ? (addr32 ? static_cast<std::uint32_t>(s.gpr[1]) : s.gpr[1]) : 1;
        while (count) {
            const std::uint64_t si = addr32 ? static_cast<std::uint32_t>(s.gpr[6]) : s.gpr[6];
            const std::uint64_t di = addr32 ? static_cast<std::uint32_t>(s.gpr[7]) : s.gpr[7];
            if (movs) wr_mem(di, sz, rd_mem(si + (fs ? s.fs_base : gs ? s.gs_base : 0), sz));
            else if (cmps) alu(7, rd_mem(si + (fs ? s.fs_base : gs ? s.gs_base : 0), sz), rd_mem(di, sz), sz);
            else if (scas) alu(7, rd_reg(0, sz), rd_mem(di, sz), sz);
            else if (lods) wr_reg(0, sz, rd_mem(si + (fs ? s.fs_base : gs ? s.gs_base : 0), sz));
            else wr_mem(di, sz, rd_reg(0, sz));
            if (movs || cmps || lods) s.gpr[6] = addr32 ? static_cast<std::uint32_t>(s.gpr[6] + step) : s.gpr[6] + step;
            if (!lods) s.gpr[7] = addr32 ? static_cast<std::uint32_t>(s.gpr[7] + step) : s.gpr[7] + step;
            --count;
            if (rep || repne) s.gpr[1] = addr32 ? static_cast<std::uint32_t>(count) : count;
            if ((cmps || scas) && (rep || repne) && count && ((s.rflags & FLAG_ZF) != 0) != rep) break;
        }
        break;
    }
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        const int sz = (op & 1) ? opsz : 1;
        const int ib = (op == 0xC0 || op == 0xC1) ? 1 : 0;
        const Rm rm = decode_rm(rg, ib);
        unsigned count = (op <= 0xC1) ? u8() : (op <= 0xD1 ? 1 : static_cast<unsigned>(s.gpr[1] & 0xFF));
        const std::uint64_t value = rd_rm(rm, sz);
        if ((count & (sz == 8 ? 0x3F : 0x1F)) != 0)
            wr_rm(rm, sz, shift(rg & 7, value, count, sz));
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
    case 0xC8: { const std::uint16_t frame = u16(); const unsigned nesting = u8() & 31; if (nesting) throw Unsupported{}; push(s.gpr[5]); s.gpr[5] = s.gpr[4]; s.gpr[4] -= frame; break; }
    case 0xC9: { s.gpr[4] = s.gpr[5]; s.gpr[5] = pop(); break; }
    case 0xCC: throw HaltEx{};
    case 0xF4: throw HaltEx{};
    case 0xE8: {
        if (has_lock) throw UdEx{};
        const std::int32_t rel = static_cast<std::int32_t>(u32());
        const std::uint64_t ret = start + pos;
        push(ret);
        s.rip = ret + static_cast<std::int64_t>(rel);
        return;
    }
    case 0xE9: { if (has_lock) throw UdEx{}; const std::int32_t rel = static_cast<std::int32_t>(u32()); s.rip = start + pos + static_cast<std::int64_t>(rel); return; }
    case 0xEB: { if (has_lock) throw UdEx{}; const std::int8_t rel = static_cast<std::int8_t>(u8()); s.rip = start + pos + static_cast<std::int64_t>(rel); return; }
    case 0xF6: case 0xF7: {
        const int sz = (op == 0xF6) ? 1 : opsz;
        const int sub = (pos < n) ? ((buf[pos] >> 3) & 7) : 0;
        const int ib = (sub < 2) ? (sz == 1 ? 1 : (sz == 2 ? 2 : 4)) : 0;
        const Rm rm = decode_rm(rg, ib);
        if ((rg & 7) == 2 || (rg & 7) == 3) check_lock(true, &rm);
        else check_lock(false);
        // TEST has an immediate after ModRM; all other extensions do not.
        // Resolve RIP-relative addressing before accessing the operand.
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
                const int dsz = sz == 1 ? 2 : sz * 2;
                std::uint64_t dividend = (sz == 1) ? (s.gpr[0] & 0xFFFF)
                                       : ((rd_reg(2, sz) << (sz * 8)) | rd_reg(0, sz));
                std::uint64_t q, r;
                if (!sg) {
                    q = dividend / v; r = dividend % v;
                    if (q > mask_of(sz)) throw DivErr{};
                } else {
                    const std::int64_t sd = sext(dividend, dsz), sv = sext(v, sz);
                    if (sd == INT64_MIN && sv == -1) throw DivErr{};
                    const std::int64_t sq = sd / sv, sr = sd % sv;
                    if (sq != sext(static_cast<std::uint64_t>(sq) & mask_of(sz), sz)) throw DivErr{};
                    q = static_cast<std::uint64_t>(sq); r = static_cast<std::uint64_t>(sr);
                }
                if (sz == 1) s.gpr[0] = (s.gpr[0] & ~0xFFFFULL) | (q & 0xFF) | ((r & 0xFF) << 8);
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
            check_lock(false);
            const Rm rm = decode_rm(rg, 0);
            const std::uint64_t v = rd_rm(rm, 8);
            if (sub == 2) { const std::uint64_t ret = start + pos; push(ret); s.rip = v; return; }
            if (sub == 4) { s.rip = v; return; }
            push(v);
            break;
        }
        if (sub > 1) throw Unsupported{};
        const Rm rm = decode_rm(rg, 0);
        check_lock(true, &rm);
        wr_rm(rm, sz, incdec(rd_rm(rm, sz), sz, (rg & 7) == 1));
        break;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        const Rm rm = decode_rm(rg, 0);
        const int sub = rg & 7;
        if (op == 0xD9) {
            if (sub == 7 && !rm.reg) {
                // FNSTCW m2byte (Store FPU Control Word)
                wr_rm(rm, 2, 0x037F);
            } else if (sub == 5 && !rm.reg) {
                // FLDCW m2byte (Load FPU Control Word)
                (void)rd_rm(rm, 2);
            }
        } else if (op == 0xDD) {
            if (sub == 7 && !rm.reg) {
                // FNSTSW m2byte (Store FPU Status Word)
                wr_rm(rm, 2, 0x0000);
            }
        }
        break;
    }
    default: throw Unsupported{};
    }
    if (has_lock && !lock_consumed) throw UdEx{};
    s.rip = start + pos;
}

} // namespace

X86Decoded decode_x86(const std::uint8_t* b, std::size_t n) {
    X86Decoded d;
    if (!b || !n) return d;
    n = n < 15 ? n : 15;
    std::size_t p = 0;
    bool p66 = false, f2 = false, f3 = false, w = false, vex = false, vex3 = false;
    bool legacy_prefix = false, rex_prefix = false;
    while (p < n) {
        const unsigned x = b[p];
        if (x >= 0x40 && x <= 0x4F) {
            w = (x & 8) != 0; rex_prefix = true; ++p; continue;
        }
        if (x == 0x66) p66 = true;
        else if (x == 0xF2) f2 = true;
        else if (x == 0xF3) f3 = true;
        else if (x == 0x67 || x == 0xF0 || x == 0x64 || x == 0x65 ||
                 x == 0x2E || x == 0x3E || x == 0x26 || x == 0x36) {}
        else break;
        legacy_prefix = true;
        w = false; rex_prefix = false;
        ++p;
    }
    if (p >= n) return d;
    unsigned map = 0, op = b[p++], pp = p66 ? 1 : f3 ? 2 : f2 ? 3 : 0;
    if (op == 0xC5 || op == 0xC4) {
        vex = true;
        if (p + (op == 0xC4 ? 2u : 1u) >= n) return d;
        if (op == 0xC4) { vex3 = true; map = b[p++] & 31; const unsigned v = b[p++]; pp = v & 3; w = (v & 128) != 0; }
        else { map = 1; pp = b[p++] & 3; }
        op = b[p++];
    } else if (op == 0x0F) {
        if (p >= n) return d;
        map = 1; op = b[p++];
        if (op == 0x38 || op == 0x3A) { map = op == 0x38 ? 2 : 3; if (p >= n) return d; op = b[p++]; }
    }
    d.opcode = (vex ? 0x100000u : 0) | (map << 16) | (pp << 8) | op;
    bool modrm = false;
    unsigned imm = 0;
    const bool opsz16 = p66 && !w;
    if (map == 0) {
        modrm = (op < 0x40 && (op & 7) < 4) || op == 0x63 || op == 0x69 || op == 0x6B ||
            (op >= 0x80 && op <= 0x8F) || op == 0x8D || (op >= 0xC0 && op <= 0xC1) ||
            (op >= 0xC6 && op <= 0xC7) || (op >= 0xD0 && op <= 0xD3) ||
            (op >= 0xF6 && op <= 0xF7) || (op >= 0xFE) || (op >= 0xD8 && op <= 0xDF);
        if (op >= 0xB0 && op <= 0xBF) imm = op < 0xB8 ? 1 : w ? 8 : opsz16 ? 2 : 4;
        if (op < 0x40 && (op & 7) >= 4 && (op & 7) <= 5) imm = (op & 7) == 4 ? 1 : opsz16 ? 2 : 4;
        if (op == 0x68 || op == 0xA9) imm = opsz16 ? 2 : 4;
        if (op == 0xE8 || op == 0xE9) imm = 4;
        if (op == 0x6A || op == 0xEB || op == 0xA8 || op == 0xCD || op == 0xC0 || op == 0xC1 || op == 0x6B || op == 0x80 || op == 0x83 || op == 0xC6) imm = 1;
        if (op == 0x69 || op == 0x81 || op == 0xC7) imm = opsz16 ? 2 : 4;
        if (op == 0xC2 || op == 0xC8) imm = 2 + (op == 0xC8);
        if (op >= 0x70 && op <= 0x7F) imm = 1;
        if (op >= 0xA0 && op <= 0xA3) imm = 8;
        if (op >= 0xE0 && op <= 0xE3) imm = 1;
        d.supported = (op < 0x40 && (op & 7) < 6) || (op >= 0x50 && op <= 0x5F) ||
            (op >= 0x70 && op <= 0x8D) || (op >= 0x90 && op <= 0x99) ||
            (op >= 0xA4 && op <= 0xAB) || (op >= 0xB0 && op <= 0xBF) ||
            (op >= 0xC0 && op <= 0xC3) || op == 0xC6 || op == 0xC7 || op == 0xC9 || op == 0xCC ||
            op == 0xE8 || op == 0xE9 || op == 0xEB || op == 0xF4 || op == 0xF6 || op == 0xF7 || op == 0xFE || op == 0xFF || op == 0x63 || op == 0x68 || op == 0x69 || op == 0x6A ||
            (op >= 0xD8 && op <= 0xDF);
    } else {
        modrm = !(vex && map == 1 && op == 0x77) &&
                !(map == 1 && (op == 0x05 || op == 0x0B || op == 0x31 || op == 0xA2 || (op >= 0x80 && op <= 0x8F) || (op >= 0xC8 && op <= 0xCF)));
        if (map == 1 && op >= 0x80 && op <= 0x8F) imm = 4;
        if (map == 3 || (map == 1 && (op == 0x70 || op == 0x71 || op == 0x72 || op == 0x73 || op == 0xBA || op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6))) imm = 1;
        d.supported = !vex && map == 1 && ((op >= 0x80 && op <= 0x9F) ||
            (op >= 0x40 && op <= 0x4F) || (op >= 0xC8 && op <= 0xCF) ||
            op == 0x0B || op == 0x1F || op == 0x1E || op == 0x18 || op == 0x0D ||
            op == 0x31 || op == 0xA2 || op == 0x05 || op == 0x01 ||
            op == 0xAF || op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF ||
            op == 0xB0 || op == 0xB1 || op == 0xC0 || op == 0xC1 || op == 0xBA ||
            op == 0xA3 || op == 0xAB || op == 0xB3 || op == 0xBB || op == 0xBC || op == 0xBD || op == 0xB8 ||
            op == 0x10 || op == 0x11 || op == 0x12 || op == 0x13 || op == 0x14 || op == 0x15 ||
            op == 0x16 || op == 0x17 || op == 0x28 || op == 0x29 || op == 0x2B || op == 0x6F || op == 0x7F || op == 0xE7 ||
            op == 0x50 || op == 0x6E || op == 0x7E || op == 0xD6 || op == 0xEF || op == 0x57 ||
            (op >= 0x54 && op <= 0x5F) || (op >= 0x58 && op <= 0x62) ||
            (op >= 0x60 && op <= 0x6D) || op == 0xDB || op == 0xDF || op == 0xEB ||
            (op >= 0xF8 && op <= 0xFE) || (op >= 0x74 && op <= 0x76) || (op >= 0x64 && op <= 0x66) || op == 0xD4 || op == 0xD5 || op == 0xD7 ||
            op == 0x70 || op == 0xC6 || op == 0xC7 || op == 0xC2 || (op >= 0x2A && op <= 0x2F) || op == 0xAE ||
            (((op >= 0x71 && op <= 0x73) || (op >= 0xD1 && op <= 0xD3) || (op >= 0xE1 && op <= 0xE2) || (op >= 0xF1 && op <= 0xF3)) && pp == 1));
        if (vex3 && map == 2 && pp == 0 && (op == 0xF2 || op == 0xF3 || op == 0xF7) &&
            !(b[p - 2] & 4) && !legacy_prefix && !rex_prefix) d.supported = true;
        if (vex && map == 1 && !legacy_prefix && !rex_prefix &&
            ((op == 0x77 && pp == 0 && !(b[p - 2] & 4)) ||
             (op == 0x50 && pp <= 1) ||
             (op == 0xAE && pp == 0 && !(b[p - 2] & 4)) ||
             (op == 0xD7 && pp == 1) ||
             (op == 0x6E && pp == 1 && !(b[p - 2] & 4)) ||
             (op == 0x7E && (pp == 1 || pp == 2) && !(b[p - 2] & 4)) ||
             (op == 0xD6 && pp == 1 && !(b[p - 2] & 4)) ||
             (op == 0x2A && (pp == 2 || pp == 3) && !(b[p - 2] & 4)) ||
             (op == 0x2C && (pp == 2 || pp == 3) && !(b[p - 2] & 4)) ||
             ((op == 0x10 || op == 0x11) && (pp <= 1 || !(b[p - 2] & 4))) ||
             ((op == 0x12 || op == 0x13 || op == 0x16 || op == 0x17) && (pp >= 2 || !(b[p - 2] & 4))) ||
             ((op == 0x14 || op == 0x15) && pp <= 1) ||
             ((op == 0x2E || op == 0x2F) && pp <= 1 && !(b[p - 2] & 4)) ||
             ((op == 0x28 || op == 0x29 || op == 0x57 || op == 0xEF || op == 0xDB || op == 0xEB || op == 0x2B || op == 0xE7) && pp <= 1) ||
             (((op >= 0x51 && op <= 0x53) || op == 0x58 || op == 0x59 || op == 0x5A || op == 0x5B || op == 0x5C || op == 0x5E || op == 0xC2) && (pp <= 1 || !(b[p - 2] & 4))) ||
             ((op == 0x6F || op == 0x7F) && (pp == 1 || pp == 2)) ||
             ((op == 0x7C || op == 0x7D) && (pp == 1 || pp == 3)) ||
             ((op >= 0x60 && op <= 0x6D) && pp == 1) ||
             ((op == 0xFC || op == 0xFD || op == 0xFE || op == 0xD4 ||
               op == 0xF8 || op == 0xF9 || op == 0xFA || op == 0xFB) && pp == 1) ||
             ((op == 0x54 || op == 0x55 || op == 0x56) && pp <= 1) ||
             (op == 0xDF && pp == 1) ||
             ((op == 0x5F || op == 0x5D) && pp <= 1) ||
             (((op >= 0x51 && op <= 0x53) || op == 0x58 || op == 0x59 || op == 0x5A || op == 0x5B || op == 0x5C || op == 0x5E || op == 0x5F || op == 0x5D || op == 0xC2) && pp >= 2) ||
             (op == 0x70 && pp == 1) ||
             (op == 0xC6 && pp <= 1) ||
             (((op >= 0x71 && op <= 0x73) || (op >= 0xD1 && op <= 0xD3) ||
               (op >= 0xE1 && op <= 0xE2) || (op >= 0xF1 && op <= 0xF3) ||
               (op >= 0x74 && op <= 0x76) || (op >= 0x64 && op <= 0x66)) && pp == 1))) d.supported = true;
        if (!vex && map == 2 && ((pp == 1 &&
            (op == 0x00 || op == 0x01 || op == 0x02 || op == 0x03 || op == 0x40 || (op >= 0x38 && op <= 0x3F) || (op >= 0x1C && op <= 0x1E) ||
             op == 0x28 || op == 0x29 || op == 0x2A || op == 0x2B || (op >= 0x20 && op <= 0x25) || (op >= 0x30 && op <= 0x37))) ||
            ((pp == 0 || pp == 1) && (op == 0xF0 || op == 0xF1)))) d.supported = true;
        if (!vex && map == 3 && pp == 1 &&
            (op == 0x08 || op == 0x09 || op == 0x0A || op == 0x0B ||
             op == 0x40 || op == 0x41 ||
             op == 0x0F || op == 0x14 || op == 0x15 || op == 0x16 || op == 0x17 || op == 0x20 || op == 0x22)) d.supported = true;
        if (vex && map == 2 && pp == 1 && !legacy_prefix && !rex_prefix &&
            (op == 0x00 || op == 0x01 || op == 0x02 || op == 0x03 || op == 0x18 || op == 0x19 || op == 0x1A || op == 0x40 || op == 0x58 || op == 0x59 || (op >= 0x38 && op <= 0x3F) || (op >= 0x1C && op <= 0x1E) ||
             op == 0x28 || op == 0x29 || op == 0x2A || op == 0x2B || (op >= 0x20 && op <= 0x25) || (op >= 0x30 && op <= 0x37))) d.supported = true;
        if (vex && map == 3 && pp == 1 && !legacy_prefix && !rex_prefix &&
            (op == 0x08 || op == 0x09 || op == 0x0A || op == 0x0B ||
             op == 0x40 || op == 0x41 ||
             op == 0x4A || op == 0x4B || op == 0x4C || op == 0x0C || op == 0x0D || op == 0x0E ||
             op == 0x0F || op == 0x18 || op == 0x19 || op == 0x21 ||
             op == 0x14 || op == 0x15 || op == 0x16 || op == 0x17 || op == 0x20 || op == 0x22)) d.supported = true;
    }
    if (modrm) {
        if (p >= n) return X86Decoded{};
        const unsigned m = b[p++], mode = m >> 6, rm = m & 7;
        if (vex3 && map == 2 && op == 0xF3 && ((m >> 3) & 7) != 1 &&
            ((m >> 3) & 7) != 2 && ((m >> 3) & 7) != 3) d.supported = false;
        if (map == 0 && (op == 0xF6 || op == 0xF7) && ((m >> 3) & 7) < 2) imm = op == 0xF6 ? 1 : opsz16 ? 2 : 4;
        if (mode != 3) {
            if (rm == 4) { if (p >= n) return X86Decoded{}; const unsigned sib = b[p++]; if (mode == 0 && (sib & 7) == 5) p += 4; }
            else if (mode == 0 && rm == 5) p += 4;
            if (mode == 1) ++p;
            if (mode == 2) p += 4;
        }
    }
    p += imm;
    if (p <= n && p <= 15) d.length = p;
    return d;
}

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

void X86Interpreter::register_aot_block(std::uint64_t rip, AotBlockHandler handler) {
    if (handler) {
        aot_blocks_[rip] = std::move(handler);
    }
}

void X86Interpreter::unregister_aot_block(std::uint64_t rip) {
    aot_blocks_.erase(rip);
}

bool X86Interpreter::has_aot_block(std::uint64_t rip) const {
    return aot_blocks_.find(rip) != aot_blocks_.end();
}

void X86Interpreter::clear_aot_blocks() {
    aot_blocks_.clear();
}

std::size_t X86Interpreter::aot_block_count() const {
    return aot_blocks_.size();
}

bool X86Interpreter::in_trap(std::uint64_t rip) const {
    for (const auto& t : traps_)
        if (rip >= t.first && rip - t.first < t.second) return true;
    return false;
}

CpuRunResult X86Interpreter::run(std::uint64_t max_steps) {
    CpuRunResult out;
    Exec e(state, mem_, tsc_);
    if (recent_traces_.empty()) recent_traces_.resize(1024);
    while (out.steps < max_steps) {
        if ((out.steps & 0xFF) == 0 && pause_checker_ && pause_checker_()) {
            out.stop = CpuStop::step_limit;
            return out;
        }
        out.rip = state.rip;
        recent_traces_[recent_rips_count_++ % 1024] = { state.rip, state.gpr[5], state.gpr[4] };
        if (in_trap(state.rip)) { out.stop = CpuStop::trap; return out; }

        // Despacho de blocos de codigo compilados AOT com fallback seguro (T051)
        auto aot_it = aot_blocks_.find(state.rip);
        if (aot_it != aot_blocks_.end() && aot_it->second) {
            auto aot_res = aot_it->second(state, mem_);
            out.steps += aot_res.steps;
            tsc_ += aot_res.steps;
            if (aot_res.status == AotExecutionStatus::trap_encountered || in_trap(state.rip)) {
                out.stop = CpuStop::trap;
                out.rip = state.rip;
                return out;
            }
            if (aot_res.status == AotExecutionStatus::fault) {
                out.stop = CpuStop::memory_fault;
                out.rip = state.rip;
                return out;
            }
            if (aot_res.status == AotExecutionStatus::fallback_untranslated) {
                if (aot_res.fallback_rip != 0) {
                    state.rip = aot_res.fallback_rip;
                }
                out.rip = state.rip;
                if (out.steps >= max_steps) {
                    out.stop = CpuStop::step_limit;
                    return out;
                }
                // Fallback seguro: continua o loop onde state.rip aponta para a instrucao nao traduzida,
                // que sera decodificada e executada com precisao pelo interpretador
                continue;
            }
            if (out.steps >= max_steps) {
                out.rip = state.rip;
                out.stop = CpuStop::step_limit;
                return out;
            }
            continue;
        }

        e.start = state.rip;
        e.pos = e.n = 0;
        e.p66 = e.rep = e.repne = e.rex = e.rw = e.rr = e.rx = e.rb = false;
        e.fs = e.gs = e.addr32 = e.branched = false;
        e.has_lock = e.lock_consumed = false;
        if (mem_.load(e.start, e.buf, 15) == MemoryError::ok) e.n = 15;
        else {
            // Busca binária pelo prefixo acessível, preservando fault no byte exato.
            std::size_t lo = 0, hi = 15;
            while (lo + 1 < hi) {
                const std::size_t mid = (lo + hi) / 2;
                if (mem_.load(e.start, e.buf, mid) == MemoryError::ok) lo = mid;
                else hi = mid;
            }
            e.n = lo;
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
