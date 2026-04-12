#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/ScalarF32x4T.h"

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace tiny_skia::wide::backend::wasm_simd128 {

#if defined(__wasm_simd128__)

[[nodiscard]] inline v128_t loadF32x4(const std::array<float, 4>& lanes) {
  return wasm_v128_load(lanes.data());
}

[[nodiscard]] inline std::array<float, 4> storeF32x4(v128_t value) {
  std::array<float, 4> out{};
  wasm_v128_store(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 4> storeI32x4(v128_t value) {
  std::array<std::int32_t, 4> out{};
  wasm_v128_store(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<float, 4> f32x4Abs(const std::array<float, 4>& lanes) {
  return storeF32x4(wasm_f32x4_abs(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Max(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_f32x4_max(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Min(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_f32x4_min(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpEq(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_f32x4_eq(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpNe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_f32x4_ne(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpGe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_f32x4_ge(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpGt(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_f32x4_gt(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpLe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_f32x4_le(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpLt(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_f32x4_lt(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Blend(const std::array<float, 4>& mask,
                                                     const std::array<float, 4>& t,
                                                     const std::array<float, 4>& f) {
  return storeF32x4(wasm_v128_bitselect(loadF32x4(t), loadF32x4(f), loadF32x4(mask)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Floor(const std::array<float, 4>& lanes) {
  return storeF32x4(wasm_f32x4_floor(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Round(const std::array<float, 4>& lanes) {
  return storeF32x4(wasm_f32x4_nearest(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4RoundInt(const std::array<float, 4>& lanes) {
  return storeI32x4(wasm_i32x4_trunc_sat_f32x4(wasm_f32x4_nearest(loadF32x4(lanes))));
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4TruncInt(const std::array<float, 4>& lanes) {
  return storeI32x4(wasm_i32x4_trunc_sat_f32x4(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4ToI32Bitcast(
    const std::array<float, 4>& lanes) {
  // v128_t is type-agnostic; reinterpretation is a no-op.
  return storeI32x4(loadF32x4(lanes));
}

[[nodiscard]] inline std::array<float, 4> f32x4RecipFast(const std::array<float, 4>& lanes) {
  // WASM SIMD128 has no reciprocal estimate; use exact division.
  return storeF32x4(wasm_f32x4_div(wasm_f32x4_splat(1.0f), loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4RecipSqrt(const std::array<float, 4>& lanes) {
  // WASM SIMD128 has no reciprocal-sqrt estimate; use exact div+sqrt.
  return storeF32x4(wasm_f32x4_div(wasm_f32x4_splat(1.0f), wasm_f32x4_sqrt(loadF32x4(lanes))));
}

[[nodiscard]] inline std::array<float, 4> f32x4Sqrt(const std::array<float, 4>& lanes) {
  return storeF32x4(wasm_f32x4_sqrt(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Add(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_f32x4_add(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Sub(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_f32x4_sub(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Mul(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_f32x4_mul(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Div(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_f32x4_div(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4BitAnd(const std::array<float, 4>& lhs,
                                                      const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_v128_and(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4BitOr(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_v128_or(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4BitXor(const std::array<float, 4>& lhs,
                                                      const std::array<float, 4>& rhs) {
  return storeF32x4(wasm_v128_xor(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4BitNot(const std::array<float, 4>& lanes) {
  return storeF32x4(wasm_v128_not(loadF32x4(lanes)));
}

#else

[[nodiscard]] inline std::array<float, 4> f32x4Abs(const std::array<float, 4>& lanes) {
  return scalar::f32x4Abs(lanes);
}

[[nodiscard]] inline std::array<float, 4> f32x4Max(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return scalar::f32x4Max(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4Min(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return scalar::f32x4Min(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpEq(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4CmpEq(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpNe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4CmpNe(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpGe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4CmpGe(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpGt(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4CmpGt(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpLe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4CmpLe(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpLt(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4CmpLt(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4Blend(const std::array<float, 4>& mask,
                                                     const std::array<float, 4>& t,
                                                     const std::array<float, 4>& f) {
  return scalar::f32x4Blend(mask, t, f);
}

[[nodiscard]] inline std::array<float, 4> f32x4Floor(const std::array<float, 4>& lanes) {
  return scalar::f32x4Floor(lanes);
}

[[nodiscard]] inline std::array<float, 4> f32x4Round(const std::array<float, 4>& lanes) {
  return scalar::f32x4Round(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4RoundInt(const std::array<float, 4>& lanes) {
  return scalar::f32x4RoundInt(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4TruncInt(const std::array<float, 4>& lanes) {
  return scalar::f32x4TruncInt(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4ToI32Bitcast(
    const std::array<float, 4>& lanes) {
  return scalar::f32x4ToI32Bitcast(lanes);
}

[[nodiscard]] inline std::array<float, 4> f32x4RecipFast(const std::array<float, 4>& lanes) {
  return scalar::f32x4RecipFast(lanes);
}

[[nodiscard]] inline std::array<float, 4> f32x4RecipSqrt(const std::array<float, 4>& lanes) {
  return scalar::f32x4RecipSqrt(lanes);
}

[[nodiscard]] inline std::array<float, 4> f32x4Sqrt(const std::array<float, 4>& lanes) {
  return scalar::f32x4Sqrt(lanes);
}

[[nodiscard]] inline std::array<float, 4> f32x4Add(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return scalar::f32x4Add(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4Sub(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return scalar::f32x4Sub(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4Mul(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return scalar::f32x4Mul(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4Div(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return scalar::f32x4Div(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4BitAnd(const std::array<float, 4>& lhs,
                                                      const std::array<float, 4>& rhs) {
  return scalar::f32x4BitAnd(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4BitOr(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4BitOr(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4BitXor(const std::array<float, 4>& lhs,
                                                      const std::array<float, 4>& rhs) {
  return scalar::f32x4BitXor(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4BitNot(const std::array<float, 4>& lanes) {
  return scalar::f32x4BitNot(lanes);
}

#endif

}  // namespace tiny_skia::wide::backend::wasm_simd128
