#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/ScalarI32x4T.h"

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace tiny_skia::wide::backend::wasm_simd128 {

#if defined(__wasm_simd128__)

[[nodiscard]] inline v128_t loadI32x4(const std::array<std::int32_t, 4>& lanes) {
  return wasm_v128_load(lanes.data());
}

[[nodiscard]] inline std::array<std::int32_t, 4> storeI32x4(v128_t value) {
  std::array<std::int32_t, 4> out{};
  wasm_v128_store(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<float, 4> storeF32x4(v128_t value) {
  std::array<float, 4> out{};
  wasm_v128_store(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Blend(const std::array<std::int32_t, 4>& mask,
                                                            const std::array<std::int32_t, 4>& t,
                                                            const std::array<std::int32_t, 4>& f) {
  return storeI32x4(wasm_v128_bitselect(loadI32x4(t), loadI32x4(f), loadI32x4(mask)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpEq(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(wasm_i32x4_eq(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpGt(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(wasm_i32x4_gt(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpLt(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(wasm_i32x4_lt(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> i32x4ToF32(const std::array<std::int32_t, 4>& lanes) {
  return storeF32x4(wasm_f32x4_convert_i32x4(loadI32x4(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Add(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(wasm_i32x4_add(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Mul(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(wasm_i32x4_mul(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4And(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(wasm_v128_and(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Or(const std::array<std::int32_t, 4>& lhs,
                                                         const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(wasm_v128_or(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Xor(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(wasm_v128_xor(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> i32x4ToF32Bitcast(
    const std::array<std::int32_t, 4>& lanes) {
  // v128_t is type-agnostic; reinterpretation is a no-op.
  return storeF32x4(loadI32x4(lanes));
}

#else

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Blend(const std::array<std::int32_t, 4>& mask,
                                                            const std::array<std::int32_t, 4>& t,
                                                            const std::array<std::int32_t, 4>& f) {
  return scalar::i32x4Blend(mask, t, f);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpEq(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4CmpEq(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpGt(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4CmpGt(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpLt(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4CmpLt(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> i32x4ToF32(const std::array<std::int32_t, 4>& lanes) {
  return scalar::i32x4ToF32(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Add(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4Add(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Mul(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4Mul(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4And(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4And(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Or(const std::array<std::int32_t, 4>& lhs,
                                                         const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4Or(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Xor(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4Xor(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> i32x4ToF32Bitcast(
    const std::array<std::int32_t, 4>& lanes) {
  return scalar::i32x4ToF32Bitcast(lanes);
}

#endif

}  // namespace tiny_skia::wide::backend::wasm_simd128
