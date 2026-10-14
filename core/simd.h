#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>

namespace bb {

// 128-bit SIMD Vector representation (aligned to 16 bytes per SSE / NEON standard)
struct alignas(16) Vec128 {
    union {
        float f32[4];
        std::uint32_t u32[4];
        std::uint64_t u64[2];
        std::uint8_t u8[16];
    };

    bool operator==(const Vec128& o) const {
        return u64[0] == o.u64[0] && u64[1] == o.u64[1];
    }
    bool operator!=(const Vec128& o) const { return !(*this == o); }
};

// Vector 4x float addition (equivalent to _mm_add_ps / vaddq_f32)
inline Vec128 vec_add_f32(const Vec128& a, const Vec128& b) {
    Vec128 r{};
    for (int i = 0; i < 4; ++i) r.f32[i] = a.f32[i] + b.f32[i];
    return r;
}

// Vector 4x float multiplication (equivalent to _mm_mul_ps / vmulq_f32)
inline Vec128 vec_mul_f32(const Vec128& a, const Vec128& b) {
    Vec128 r{};
    for (int i = 0; i < 4; ++i) r.f32[i] = a.f32[i] * b.f32[i];
    return r;
}

// Vector dot product of 4 floats (equivalent to _mm_dp_ps)
inline float vec_dot_f32(const Vec128& a, const Vec128& b) {
    float sum = 0.0f;
    for (int i = 0; i < 4; ++i) sum += a.f32[i] * b.f32[i];
    return sum;
}

// Vector 4x int32 addition (equivalent to _mm_add_epi32 / vaddq_u32)
inline Vec128 vec_add_i32(const Vec128& a, const Vec128& b) {
    Vec128 r{};
    for (int i = 0; i < 4; ++i) r.u32[i] = a.u32[i] + b.u32[i];
    return r;
}

// Vector bitwise XOR (equivalent to _mm_xor_ps / _mm_xor_si128 / veorq_u8)
inline Vec128 vec_xor(const Vec128& a, const Vec128& b) {
    Vec128 r{};
    r.u64[0] = a.u64[0] ^ b.u64[0];
    r.u64[1] = a.u64[1] ^ b.u64[1];
    return r;
}

// Vector bitwise AND (equivalent to _mm_and_ps / _mm_and_si128 / vandq_u8)
inline Vec128 vec_and(const Vec128& a, const Vec128& b) {
    Vec128 r{};
    r.u64[0] = a.u64[0] & b.u64[0];
    r.u64[1] = a.u64[1] & b.u64[1];
    return r;
}

// Float classification checks preserving IEEE-754 semantics
inline bool vec_has_nan(const Vec128& v) {
    for (int i = 0; i < 4; ++i) if (std::isnan(v.f32[i])) return true;
    return false;
}

inline bool vec_has_inf(const Vec128& v) {
    for (int i = 0; i < 4; ++i) if (std::isinf(v.f32[i])) return true;
    return false;
}

} // namespace bb
