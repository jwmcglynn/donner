#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/ScalarU16x16T.h"

#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace tiny_skia::wide::backend::wasm_simd128 {

#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__wasm_simd128__)

namespace {

[[nodiscard]] inline v128_t div255Lo(v128_t v) {
  const v128_t v128 = wasm_i16x8_add(v, wasm_i16x8_splat(128));
  return wasm_u16x8_shr(wasm_i16x8_add(v128, wasm_u16x8_shr(v128, 8)), 8);
}

}  // namespace

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Min(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(wasm_u16x8_min(lhs.wasmLo(), rhs.wasmLo()),
                 wasm_u16x8_min(lhs.wasmHi(), rhs.wasmHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Max(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(wasm_u16x8_max(lhs.wasmLo(), rhs.wasmLo()),
                 wasm_u16x8_max(lhs.wasmHi(), rhs.wasmHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16CmpLe(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(wasm_u16x8_le(lhs.wasmLo(), rhs.wasmLo()),
                 wasm_u16x8_le(lhs.wasmHi(), rhs.wasmHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Blend(const U16x16T& mask, const U16x16T& t,
                                                          const U16x16T& e) {
  return U16x16T(wasm_v128_bitselect(t.wasmLo(), e.wasmLo(), mask.wasmLo()),
                 wasm_v128_bitselect(t.wasmHi(), e.wasmHi(), mask.wasmHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Add(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(wasm_i16x8_add(lhs.wasmLo(), rhs.wasmLo()),
                 wasm_i16x8_add(lhs.wasmHi(), rhs.wasmHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Sub(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(wasm_i16x8_sub(lhs.wasmLo(), rhs.wasmLo()),
                 wasm_i16x8_sub(lhs.wasmHi(), rhs.wasmHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Mul(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(wasm_i16x8_mul(lhs.wasmLo(), rhs.wasmLo()),
                 wasm_i16x8_mul(lhs.wasmHi(), rhs.wasmHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16And(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(wasm_v128_and(lhs.wasmLo(), rhs.wasmLo()),
                 wasm_v128_and(lhs.wasmHi(), rhs.wasmHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Or(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(wasm_v128_or(lhs.wasmLo(), rhs.wasmLo()),
                 wasm_v128_or(lhs.wasmHi(), rhs.wasmHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Not(const U16x16T& value) {
  return U16x16T(wasm_v128_not(value.wasmLo()), wasm_v128_not(value.wasmHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Div255(const U16x16T& value) {
  return U16x16T(div255Lo(value.wasmLo()), div255Lo(value.wasmHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16MulDiv255(const U16x16T& lhs,
                                                              const U16x16T& rhs) {
  return U16x16T(div255Lo(wasm_i16x8_mul(lhs.wasmLo(), rhs.wasmLo())),
                 div255Lo(wasm_i16x8_mul(lhs.wasmHi(), rhs.wasmHi())));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16MulAddDiv255(const U16x16T& lhs0,
                                                                 const U16x16T& rhs0,
                                                                 const U16x16T& lhs1,
                                                                 const U16x16T& rhs1) {
  return U16x16T(
      div255Lo(wasm_i16x8_add(wasm_i16x8_mul(lhs0.wasmLo(), rhs0.wasmLo()),
                               wasm_i16x8_mul(lhs1.wasmLo(), rhs1.wasmLo()))),
      div255Lo(wasm_i16x8_add(wasm_i16x8_mul(lhs0.wasmHi(), rhs0.wasmHi()),
                               wasm_i16x8_mul(lhs1.wasmHi(), rhs1.wasmHi()))));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16SourceOver(const U16x16T& source,
                                                               const U16x16T& dest,
                                                               const U16x16T& sourceAlpha) {
  const auto max255 = wasm_i16x8_splat(255);
  const auto invSaLo = wasm_i16x8_sub(max255, sourceAlpha.wasmLo());
  const auto invSaHi = wasm_i16x8_sub(max255, sourceAlpha.wasmHi());
  const auto dstLo = div255Lo(wasm_i16x8_mul(dest.wasmLo(), invSaLo));
  const auto dstHi = div255Lo(wasm_i16x8_mul(dest.wasmHi(), invSaHi));
  return U16x16T(wasm_i16x8_add(source.wasmLo(), dstLo),
                 wasm_i16x8_add(source.wasmHi(), dstHi));
}

#else

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Min(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Min(lhs, rhs);
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Max(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Max(lhs, rhs);
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16CmpLe(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16CmpLe(lhs, rhs);
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Blend(const U16x16T& mask, const U16x16T& t,
                                                          const U16x16T& e) {
  return scalar::u16x16Blend(mask, t, e);
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Add(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Add(lhs, rhs);
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Sub(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Sub(lhs, rhs);
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Mul(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Mul(lhs, rhs);
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16And(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16And(lhs, rhs);
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Div255(const U16x16T& value) {
  const auto v128 = scalar::u16x16Add(value, U16x16T::splat(128));
  return scalar::u16x16Shr(scalar::u16x16Add(v128, scalar::u16x16Shr(v128, U16x16T::splat(8))),
                            U16x16T::splat(8));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16MulDiv255(const U16x16T& lhs,
                                                              const U16x16T& rhs) {
  return u16x16Div255(scalar::u16x16Mul(lhs, rhs));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16MulAddDiv255(const U16x16T& lhs0,
                                                                 const U16x16T& rhs0,
                                                                 const U16x16T& lhs1,
                                                                 const U16x16T& rhs1) {
  return u16x16Div255(
      scalar::u16x16Add(scalar::u16x16Mul(lhs0, rhs0), scalar::u16x16Mul(lhs1, rhs1)));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16SourceOver(const U16x16T& source,
                                                               const U16x16T& dest,
                                                               const U16x16T& sourceAlpha) {
  return scalar::u16x16Add(source, u16x16Div255(scalar::u16x16Mul(
                                       dest, scalar::u16x16Sub(U16x16T::splat(255), sourceAlpha))));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Or(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Or(lhs, rhs);
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Not(const U16x16T& value) {
  return scalar::u16x16Not(value);
}

#endif

}  // namespace tiny_skia::wide::backend::wasm_simd128
