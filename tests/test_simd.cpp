#include "check.h"
#include "simd.h"

int main() { return run([] {
    using namespace bb;
    Vec128 a{{{1.0f, 2.0f, 3.0f, 4.0f}}};
    Vec128 b{{{10.0f, 20.0f, 30.0f, 40.0f}}};

    auto sum = vec_add_f32(a, b);
    CHECK(sum.f32[0] == 11.0f);
    CHECK(sum.f32[1] == 22.0f);
    CHECK(sum.f32[2] == 33.0f);
    CHECK(sum.f32[3] == 44.0f);

    auto prod = vec_mul_f32(a, b);
    CHECK(prod.f32[0] == 10.0f);
    CHECK(prod.f32[1] == 40.0f);
    CHECK(prod.f32[2] == 90.0f);
    CHECK(prod.f32[3] == 160.0f);

    float dot = vec_dot_f32(a, b);
    CHECK(dot == 300.0f); // 10 + 40 + 90 + 160 = 300

    auto xor_zero = vec_xor(a, a);
    CHECK(xor_zero.u64[0] == 0 && xor_zero.u64[1] == 0);

    auto and_self = vec_and(a, a);
    CHECK(and_self == a);

    Vec128 v_int1{{{0, 0, 0, 0}}};
    v_int1.u32[0] = 50; v_int1.u32[1] = 60;
    Vec128 v_int2{{{0, 0, 0, 0}}};
    v_int2.u32[0] = 50; v_int2.u32[1] = 40;
    auto i_sum = vec_add_i32(v_int1, v_int2);
    CHECK(i_sum.u32[0] == 100);
    CHECK(i_sum.u32[1] == 100);

    CHECK(!vec_has_nan(a));
    CHECK(!vec_has_inf(a));

    Vec128 nan_v{{{NAN, 1.0f, 2.0f, 3.0f}}};
    CHECK(vec_has_nan(nan_v));

    Vec128 inf_v{{{1.0f, -INFINITY, 2.0f, 3.0f}}};
    CHECK(vec_has_inf(inf_v));

    std::cout << "simd: 128-bit float, int, bitwise and IEEE-754 edge cases passed\n";
}); }
