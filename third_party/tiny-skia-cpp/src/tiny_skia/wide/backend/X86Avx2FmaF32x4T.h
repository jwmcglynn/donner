#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/ScalarF32x4T.h"

#if defined(__AVX2__) && defined(__FMA__) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#endif

namespace tiny_skia::wide::backend::x86_avx2_fma {

#if defined(__AVX2__) && defined(__FMA__) && (defined(__x86_64__) || defined(__i386__))

[[nodiscard]] inline __m128 loadF32x4(const std::array<float, 4>& lanes) {
  return _mm_loadu_ps(lanes.data());
}

[[nodiscard]] inline std::array<float, 4> storeF32x4(__m128 value) {
  std::array<float, 4> out{};
  _mm_storeu_ps(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 4> storeI32x4(__m128i value) {
  std::array<std::int32_t, 4> out{};
  _mm_storeu_si128(reinterpret_cast<__m128i*>(out.data()), value);
  return out;
}

[[nodiscard]] inline std::array<float, 4> f32x4Abs(const std::array<float, 4>& lanes) {
  const __m128 signMask = _mm_set1_ps(-0.0f);
  return storeF32x4(_mm_andnot_ps(signMask, loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Max(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_max_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Min(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_min_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpEq(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_cmpeq_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpNe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_cmpneq_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpGe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_cmpge_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpGt(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_cmpgt_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpLe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_cmple_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpLt(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_cmplt_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Blend(const std::array<float, 4>& mask,
                                                     const std::array<float, 4>& t,
                                                     const std::array<float, 4>& f) {
  const __m128 maskLanes = loadF32x4(mask);
  const __m128 trueLanes = loadF32x4(t);
  const __m128 falseLanes = loadF32x4(f);
  return storeF32x4(
      _mm_or_ps(_mm_and_ps(maskLanes, trueLanes), _mm_andnot_ps(maskLanes, falseLanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Floor(const std::array<float, 4>& lanes) {
  return storeF32x4(_mm_floor_ps(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Round(const std::array<float, 4>& lanes) {
  return storeF32x4(_mm_round_ps(loadF32x4(lanes), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4RoundInt(const std::array<float, 4>& lanes) {
  return storeI32x4(_mm_cvtps_epi32(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4TruncInt(const std::array<float, 4>& lanes) {
  return storeI32x4(_mm_cvttps_epi32(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4ToI32Bitcast(
    const std::array<float, 4>& lanes) {
  return storeI32x4(_mm_castps_si128(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4RecipFast(const std::array<float, 4>& lanes) {
  return storeF32x4(_mm_rcp_ps(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4RecipSqrt(const std::array<float, 4>& lanes) {
  return storeF32x4(_mm_rsqrt_ps(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Sqrt(const std::array<float, 4>& lanes) {
  return storeF32x4(_mm_sqrt_ps(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Add(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_add_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Sub(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_sub_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Mul(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_mul_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Div(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_div_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4BitAnd(const std::array<float, 4>& lhs,
                                                      const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_and_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4BitOr(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_or_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4BitXor(const std::array<float, 4>& lhs,
                                                      const std::array<float, 4>& rhs) {
  return storeF32x4(_mm_xor_ps(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4BitNot(const std::array<float, 4>& lanes) {
  const __m128i allOnes = _mm_set1_epi32(-1);
  return storeF32x4(_mm_xor_ps(loadF32x4(lanes), _mm_castsi128_ps(allOnes)));
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

}  // namespace tiny_skia::wide::backend::x86_avx2_fma
