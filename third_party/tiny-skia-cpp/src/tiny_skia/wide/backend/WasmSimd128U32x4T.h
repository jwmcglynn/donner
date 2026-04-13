#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/ScalarU32x4T.h"

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace tiny_skia::wide::backend::wasm_simd128 {

#if defined(__wasm_simd128__)

[[nodiscard]] inline v128_t loadU32x4(const std::array<std::uint32_t, 4>& lanes) {
  return wasm_v128_load(lanes.data());
}

[[nodiscard]] inline std::array<std::uint32_t, 4> storeU32x4(v128_t value) {
  std::array<std::uint32_t, 4> out{};
  wasm_v128_store(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpEq(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(wasm_i32x4_eq(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpNe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(wasm_i32x4_ne(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpLt(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(wasm_u32x4_lt(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpLe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(wasm_u32x4_le(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpGt(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(wasm_u32x4_gt(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpGe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(wasm_u32x4_ge(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Not(
    const std::array<std::uint32_t, 4>& lanes) {
  return storeU32x4(wasm_v128_not(loadU32x4(lanes)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Add(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(wasm_i32x4_add(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4And(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(wasm_v128_and(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Or(const std::array<std::uint32_t, 4>& lhs,
                                                          const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(wasm_v128_or(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Xor(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(wasm_v128_xor(loadU32x4(lhs), loadU32x4(rhs)));
}

#else

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpEq(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4CmpEq(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpNe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4CmpNe(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpLt(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4CmpLt(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpLe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4CmpLe(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpGt(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4CmpGt(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpGe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4CmpGe(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Not(
    const std::array<std::uint32_t, 4>& lanes) {
  return scalar::u32x4Not(lanes);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Add(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4Add(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4And(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4And(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Or(const std::array<std::uint32_t, 4>& lhs,
                                                          const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4Or(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Xor(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4Xor(lhs, rhs);
}

#endif

}  // namespace tiny_skia::wide::backend::wasm_simd128
