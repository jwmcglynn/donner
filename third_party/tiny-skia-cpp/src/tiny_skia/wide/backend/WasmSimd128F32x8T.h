#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/ScalarF32x8T.h"

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace tiny_skia::wide::backend::wasm_simd128 {

#if defined(__wasm_simd128__)

struct F32x8Wasm {
  v128_t lo;
  v128_t hi;
};

[[nodiscard]] inline F32x8Wasm loadF32x8(const std::array<float, 8>& lanes) {
  return F32x8Wasm{wasm_v128_load(lanes.data()), wasm_v128_load(lanes.data() + 4)};
}

[[nodiscard]] inline std::array<float, 8> storeF32x8(const F32x8Wasm& value) {
  std::array<float, 8> out{};
  wasm_v128_store(out.data(), value.lo);
  wasm_v128_store(out.data() + 4, value.hi);
  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> storeI32x8(v128_t lo, v128_t hi) {
  std::array<std::int32_t, 8> out{};
  wasm_v128_store(out.data(), lo);
  wasm_v128_store(out.data() + 4, hi);
  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> storeU32x8(v128_t lo, v128_t hi) {
  std::array<std::uint32_t, 8> out{};
  wasm_v128_store(out.data(), lo);
  wasm_v128_store(out.data() + 4, hi);
  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Floor(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeF32x8(F32x8Wasm{wasm_f32x4_floor(v.lo), wasm_f32x4_floor(v.hi)});
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8ToI32Bitcast(
    const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  // v128_t is type-agnostic; reinterpretation is a no-op.
  return storeI32x8(v.lo, v.hi);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> f32x8ToU32Bitcast(
    const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  // v128_t is type-agnostic; reinterpretation is a no-op.
  return storeU32x8(v.lo, v.hi);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpEq(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_f32x4_eq(a.lo, b.lo), wasm_f32x4_eq(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpNe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_f32x4_ne(a.lo, b.lo), wasm_f32x4_ne(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpGe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_f32x4_ge(a.lo, b.lo), wasm_f32x4_ge(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpGt(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_f32x4_gt(a.lo, b.lo), wasm_f32x4_gt(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpLe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_f32x4_le(a.lo, b.lo), wasm_f32x4_le(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpLt(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_f32x4_lt(a.lo, b.lo), wasm_f32x4_lt(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Blend(const std::array<float, 8>& mask,
                                                     const std::array<float, 8>& t,
                                                     const std::array<float, 8>& f) {
  const auto m = loadF32x8(mask);
  const auto onTrue = loadF32x8(t);
  const auto onFalse = loadF32x8(f);
  return storeF32x8(F32x8Wasm{wasm_v128_bitselect(onTrue.lo, onFalse.lo, m.lo),
                               wasm_v128_bitselect(onTrue.hi, onFalse.hi, m.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Abs(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeF32x8(F32x8Wasm{wasm_f32x4_abs(v.lo), wasm_f32x4_abs(v.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Sqrt(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeF32x8(F32x8Wasm{wasm_f32x4_sqrt(v.lo), wasm_f32x4_sqrt(v.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8RecipFast(const std::array<float, 8>& lanes) {
  // WASM SIMD128 has no reciprocal estimate; use exact division.
  const auto v = loadF32x8(lanes);
  const v128_t one = wasm_f32x4_splat(1.0f);
  return storeF32x8(F32x8Wasm{wasm_f32x4_div(one, v.lo), wasm_f32x4_div(one, v.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8RecipSqrt(const std::array<float, 8>& lanes) {
  // WASM SIMD128 has no reciprocal-sqrt estimate; use exact div+sqrt.
  const auto v = loadF32x8(lanes);
  const v128_t one = wasm_f32x4_splat(1.0f);
  return storeF32x8(
      F32x8Wasm{wasm_f32x4_div(one, wasm_f32x4_sqrt(v.lo)),
                wasm_f32x4_div(one, wasm_f32x4_sqrt(v.hi))});
}

[[nodiscard]] inline std::array<float, 8> f32x8Max(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_f32x4_max(a.lo, b.lo), wasm_f32x4_max(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Min(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_f32x4_min(a.lo, b.lo), wasm_f32x4_min(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Round(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeF32x8(F32x8Wasm{wasm_f32x4_nearest(v.lo), wasm_f32x4_nearest(v.hi)});
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8RoundInt(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeI32x8(wasm_i32x4_trunc_sat_f32x4(wasm_f32x4_nearest(v.lo)),
                    wasm_i32x4_trunc_sat_f32x4(wasm_f32x4_nearest(v.hi)));
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8TruncInt(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeI32x8(wasm_i32x4_trunc_sat_f32x4(v.lo), wasm_i32x4_trunc_sat_f32x4(v.hi));
}

[[nodiscard]] inline std::array<float, 8> f32x8Add(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_f32x4_add(a.lo, b.lo), wasm_f32x4_add(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Sub(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_f32x4_sub(a.lo, b.lo), wasm_f32x4_sub(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Mul(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_f32x4_mul(a.lo, b.lo), wasm_f32x4_mul(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Div(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_f32x4_div(a.lo, b.lo), wasm_f32x4_div(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8BitAnd(const std::array<float, 8>& lhs,
                                                      const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_v128_and(a.lo, b.lo), wasm_v128_and(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8BitOr(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_v128_or(a.lo, b.lo), wasm_v128_or(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8BitXor(const std::array<float, 8>& lhs,
                                                      const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Wasm{wasm_v128_xor(a.lo, b.lo), wasm_v128_xor(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8BitNot(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeF32x8(F32x8Wasm{wasm_v128_not(v.lo), wasm_v128_not(v.hi)});
}

#else

[[nodiscard]] inline std::array<float, 8> f32x8Floor(const std::array<float, 8>& lanes) {
  return scalar::f32x8Floor(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8ToI32Bitcast(
    const std::array<float, 8>& lanes) {
  return scalar::f32x8ToI32Bitcast(lanes);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> f32x8ToU32Bitcast(
    const std::array<float, 8>& lanes) {
  return scalar::f32x8ToU32Bitcast(lanes);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpEq(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8CmpEq(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpNe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8CmpNe(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpGe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8CmpGe(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpGt(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8CmpGt(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpLe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8CmpLe(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpLt(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8CmpLt(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8Blend(const std::array<float, 8>& mask,
                                                     const std::array<float, 8>& t,
                                                     const std::array<float, 8>& f) {
  return scalar::f32x8Blend(mask, t, f);
}

[[nodiscard]] inline std::array<float, 8> f32x8Abs(const std::array<float, 8>& lanes) {
  return scalar::f32x8Abs(lanes);
}

[[nodiscard]] inline std::array<float, 8> f32x8Sqrt(const std::array<float, 8>& lanes) {
  return scalar::f32x8Sqrt(lanes);
}

[[nodiscard]] inline std::array<float, 8> f32x8RecipFast(const std::array<float, 8>& lanes) {
  return scalar::f32x8RecipFast(lanes);
}

[[nodiscard]] inline std::array<float, 8> f32x8RecipSqrt(const std::array<float, 8>& lanes) {
  return scalar::f32x8RecipSqrt(lanes);
}

[[nodiscard]] inline std::array<float, 8> f32x8Max(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return scalar::f32x8Max(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8Min(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return scalar::f32x8Min(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8Round(const std::array<float, 8>& lanes) {
  return scalar::f32x8Round(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8RoundInt(const std::array<float, 8>& lanes) {
  return scalar::f32x8RoundInt(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8TruncInt(const std::array<float, 8>& lanes) {
  return scalar::f32x8TruncInt(lanes);
}

[[nodiscard]] inline std::array<float, 8> f32x8Add(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return scalar::f32x8Add(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8Sub(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return scalar::f32x8Sub(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8Mul(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return scalar::f32x8Mul(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8Div(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return scalar::f32x8Div(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8BitAnd(const std::array<float, 8>& lhs,
                                                      const std::array<float, 8>& rhs) {
  return scalar::f32x8BitAnd(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8BitOr(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8BitOr(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8BitXor(const std::array<float, 8>& lhs,
                                                      const std::array<float, 8>& rhs) {
  return scalar::f32x8BitXor(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8BitNot(const std::array<float, 8>& lanes) {
  return scalar::f32x8BitNot(lanes);
}

#endif

}  // namespace tiny_skia::wide::backend::wasm_simd128
