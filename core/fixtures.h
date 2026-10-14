#pragma once
#include "alu.h"

// Legacy fixtures
bb::Result fixture_add(std::uint64_t a, std::uint64_t b);
bb::Result fixture_sub(std::uint64_t a, std::uint64_t b);
bb::Result fixture_xor(std::uint64_t a, std::uint64_t b);

// F1: Arithmetic & flags (expanded)
bb::Result fixture_add8(std::uint64_t a, std::uint64_t b);
bb::Result fixture_add16(std::uint64_t a, std::uint64_t b);
bb::Result fixture_add32(std::uint64_t a, std::uint64_t b);
bb::Result fixture_add64(std::uint64_t a, std::uint64_t b);
bb::Result fixture_adc8(std::uint64_t a, std::uint64_t b);
bb::Result fixture_adc64(std::uint64_t a, std::uint64_t b);
bb::Result fixture_sub8(std::uint64_t a, std::uint64_t b);
bb::Result fixture_sub16(std::uint64_t a, std::uint64_t b);
bb::Result fixture_sub32(std::uint64_t a, std::uint64_t b);
bb::Result fixture_sub64(std::uint64_t a, std::uint64_t b);
bb::Result fixture_sbb64(std::uint64_t a, std::uint64_t b);
bb::Result fixture_and8(std::uint64_t a, std::uint64_t b);
bb::Result fixture_or64(std::uint64_t a, std::uint64_t b);
bb::Result fixture_xor8(std::uint64_t a, std::uint64_t b);
bb::Result fixture_xor64(std::uint64_t a, std::uint64_t b);
bb::Result fixture_shl8(std::uint64_t a, std::uint64_t b);
bb::Result fixture_shr64(std::uint64_t a, std::uint64_t b);
bb::Result fixture_cmp32(std::uint64_t a, std::uint64_t b);
bb::Result fixture_test64(std::uint64_t a, std::uint64_t b);

// F2: Control flow
bb::Result fixture_call_direct(std::uint64_t a, std::uint64_t b);
bb::Result fixture_call_indirect(std::uint64_t a, std::uint64_t b);
bb::Result fixture_recursion(std::uint64_t n, std::uint64_t dummy);
bb::Result fixture_jump_table(std::uint64_t idx, std::uint64_t dummy);
bb::Result fixture_vtable_call(std::uint64_t obj, std::uint64_t dummy);
bb::Result fixture_callback(std::uint64_t fn_ptr, std::uint64_t arg);

// F3: ABI
bb::Result fixture_abi_0args(std::uint64_t a, std::uint64_t b);
bb::Result fixture_abi_1arg(std::uint64_t a, std::uint64_t b);
bb::Result fixture_abi_6args(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t e, std::uint64_t f);
bb::Result fixture_abi_12args(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t e, std::uint64_t f,
                              std::uint64_t g, std::uint64_t h, std::uint64_t i, std::uint64_t j, std::uint64_t k, std::uint64_t l);
bb::Result fixture_abi_struct_byval(std::uint64_t a, std::uint64_t b);
bb::Result fixture_abi_struct_ret(std::uint64_t a, std::uint64_t b);
bb::Result fixture_abi_fp_args(double a, double b);
bb::Result fixture_abi_align16(std::uint64_t a, std::uint64_t b);
bb::Result fixture_abi_callee_saved(std::uint64_t a, std::uint64_t b);

namespace bb {
using ::fixture_add;
using ::fixture_sub;
using ::fixture_xor;

using ::fixture_add8;
using ::fixture_add16;
using ::fixture_add32;
using ::fixture_add64;
using ::fixture_adc8;
using ::fixture_adc64;
using ::fixture_sub8;
using ::fixture_sub16;
using ::fixture_sub32;
using ::fixture_sub64;
using ::fixture_sbb64;
using ::fixture_and8;
using ::fixture_or64;
using ::fixture_xor8;
using ::fixture_xor64;
using ::fixture_shl8;
using ::fixture_shr64;
using ::fixture_cmp32;
using ::fixture_test64;

using ::fixture_call_direct;
using ::fixture_call_indirect;
using ::fixture_recursion;
using ::fixture_jump_table;
using ::fixture_vtable_call;
using ::fixture_callback;

using ::fixture_abi_0args;
using ::fixture_abi_1arg;
using ::fixture_abi_6args;
using ::fixture_abi_12args;
using ::fixture_abi_struct_byval;
using ::fixture_abi_struct_ret;
using ::fixture_abi_fp_args;
using ::fixture_abi_align16;
using ::fixture_abi_callee_saved;
}
