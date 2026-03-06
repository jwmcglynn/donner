#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/ScalarI32x4T.h"

#if defined(__AVX2__) && defined(__FMA__) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#endif

namespace tiny_skia::wide::backend::x86_avx2_fma {

#if defined(__AVX2__) && defined(__FMA__) && (defined(__x86_64__) || defined(__i386__))

[[nodiscard]] inline __m128i loadI32x4(const std::array<std::int32_t, 4>& lanes) {
  return _mm_loadu_si128(reinterpret_cast<const __m128i*>(lanes.data()));
}

[[nodiscard]] inline std::array<std::int32_t, 4> storeI32x4(__m128i value) {
  std::array<std::int32_t, 4> out{};
  _mm_storeu_si128(reinterpret_cast<__m128i*>(out.data()), value);
  return out;
}

[[nodiscard]] inline std::array<float, 4> storeF32x4(__m128 value) {
  std::array<float, 4> out{};
  _mm_storeu_ps(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Blend(const std::array<std::int32_t, 4>& mask,
                                                            const std::array<std::int32_t, 4>& t,
                                                            const std::array<std::int32_t, 4>& f) {
  const __m128i maskLanes = loadI32x4(mask);
  const __m128i trueLanes = loadI32x4(t);
  const __m128i falseLanes = loadI32x4(f);
  return storeI32x4(_mm_or_si128(_mm_and_si128(maskLanes, trueLanes),
                                 _mm_andnot_si128(maskLanes, falseLanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpEq(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(_mm_cmpeq_epi32(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpGt(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(_mm_cmpgt_epi32(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpLt(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(_mm_cmpgt_epi32(loadI32x4(rhs), loadI32x4(lhs)));
}

[[nodiscard]] inline std::array<float, 4> i32x4ToF32(const std::array<std::int32_t, 4>& lanes) {
  return storeF32x4(_mm_cvtepi32_ps(loadI32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> i32x4ToF32Bitcast(
    const std::array<std::int32_t, 4>& lanes) {
  return storeF32x4(_mm_castsi128_ps(loadI32x4(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Add(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(_mm_add_epi32(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Mul(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(_mm_mullo_epi32(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4And(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(_mm_and_si128(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Or(const std::array<std::int32_t, 4>& lhs,
                                                         const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(_mm_or_si128(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Xor(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(_mm_xor_si128(loadI32x4(lhs), loadI32x4(rhs)));
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

[[nodiscard]] inline std::array<float, 4> i32x4ToF32Bitcast(
    const std::array<std::int32_t, 4>& lanes) {
  return scalar::i32x4ToF32Bitcast(lanes);
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

#endif

}  // namespace tiny_skia::wide::backend::x86_avx2_fma
