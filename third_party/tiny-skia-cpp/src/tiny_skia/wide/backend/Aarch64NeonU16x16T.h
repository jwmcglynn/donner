#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/ScalarU16x16T.h"

#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace tiny_skia::wide::backend::aarch64_neon {

#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)

namespace {

[[nodiscard]] inline uint16x8_t div255Lo(uint16x8_t v) {
  return vshrq_n_u16(vaddq_u16(v, vdupq_n_u16(255)), 8);
}

}  // namespace

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Min(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(vminq_u16(lhs.neonLo(), rhs.neonLo()), vminq_u16(lhs.neonHi(), rhs.neonHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Max(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(vmaxq_u16(lhs.neonLo(), rhs.neonLo()), vmaxq_u16(lhs.neonHi(), rhs.neonHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16CmpLe(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(vcleq_u16(lhs.neonLo(), rhs.neonLo()), vcleq_u16(lhs.neonHi(), rhs.neonHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Blend(const U16x16T& mask, const U16x16T& t,
                                                          const U16x16T& e) {
  return U16x16T(vbslq_u16(mask.neonLo(), t.neonLo(), e.neonLo()),
                 vbslq_u16(mask.neonHi(), t.neonHi(), e.neonHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Add(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(vaddq_u16(lhs.neonLo(), rhs.neonLo()), vaddq_u16(lhs.neonHi(), rhs.neonHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Sub(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(vsubq_u16(lhs.neonLo(), rhs.neonLo()), vsubq_u16(lhs.neonHi(), rhs.neonHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Mul(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(vmulq_u16(lhs.neonLo(), rhs.neonLo()), vmulq_u16(lhs.neonHi(), rhs.neonHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16And(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(vandq_u16(lhs.neonLo(), rhs.neonLo()), vandq_u16(lhs.neonHi(), rhs.neonHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Or(const U16x16T& lhs, const U16x16T& rhs) {
  return U16x16T(vorrq_u16(lhs.neonLo(), rhs.neonLo()), vorrq_u16(lhs.neonHi(), rhs.neonHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Not(const U16x16T& value) {
  return U16x16T(vmvnq_u16(value.neonLo()), vmvnq_u16(value.neonHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16Div255(const U16x16T& value) {
  return U16x16T(div255Lo(value.neonLo()), div255Lo(value.neonHi()));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16MulDiv255(const U16x16T& lhs,
                                                              const U16x16T& rhs) {
  return U16x16T(div255Lo(vmulq_u16(lhs.neonLo(), rhs.neonLo())),
                 div255Lo(vmulq_u16(lhs.neonHi(), rhs.neonHi())));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16MulAddDiv255(const U16x16T& lhs0,
                                                                 const U16x16T& rhs0,
                                                                 const U16x16T& lhs1,
                                                                 const U16x16T& rhs1) {
  return U16x16T(
      div255Lo(vaddq_u16(vmulq_u16(lhs0.neonLo(), rhs0.neonLo()),
                          vmulq_u16(lhs1.neonLo(), rhs1.neonLo()))),
      div255Lo(vaddq_u16(vmulq_u16(lhs0.neonHi(), rhs0.neonHi()),
                          vmulq_u16(lhs1.neonHi(), rhs1.neonHi()))));
}

[[maybe_unused]] [[nodiscard]] inline U16x16T u16x16SourceOver(const U16x16T& source,
                                                               const U16x16T& dest,
                                                               const U16x16T& sourceAlpha) {
  const auto max255 = vdupq_n_u16(255);
  const auto invSaLo = vsubq_u16(max255, sourceAlpha.neonLo());
  const auto invSaHi = vsubq_u16(max255, sourceAlpha.neonHi());
  const auto dstLo = div255Lo(vmulq_u16(dest.neonLo(), invSaLo));
  const auto dstHi = div255Lo(vmulq_u16(dest.neonHi(), invSaHi));
  return U16x16T(vaddq_u16(source.neonLo(), dstLo), vaddq_u16(source.neonHi(), dstHi));
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
  return scalar::u16x16Shr(scalar::u16x16Add(value, U16x16T::splat(255)), U16x16T::splat(8));
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

}  // namespace tiny_skia::wide::backend::aarch64_neon
