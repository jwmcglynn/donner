#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(__AVX2__) && defined(__FMA__) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#endif

#include "tiny_skia/wide/backend/ScalarF32x8T.h"

namespace tiny_skia::wide::backend::x86_avx2_fma {

#if defined(__AVX2__) && defined(__FMA__) && (defined(__x86_64__) || defined(__i386__))

[[nodiscard]] inline __m256 loadF32x8(const std::array<float, 8>& lanes) {
  return _mm256_loadu_ps(lanes.data());
}

[[nodiscard]] inline std::array<float, 8> storeF32x8(__m256 value) {
  std::array<float, 8> out{};
  _mm256_storeu_ps(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> storeI32x8(__m256i value) {
  std::array<std::int32_t, 8> out{};
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(out.data()), value);
  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> storeU32x8(__m256i value) {
  std::array<std::uint32_t, 8> out{};
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(out.data()), value);
  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Floor(const std::array<float, 8>& lanes) {
  return storeF32x8(_mm256_floor_ps(loadF32x8(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8ToI32Bitcast(
    const std::array<float, 8>& lanes) {
  return storeI32x8(_mm256_castps_si256(loadF32x8(lanes)));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> f32x8ToU32Bitcast(
    const std::array<float, 8>& lanes) {
  return storeU32x8(_mm256_castps_si256(loadF32x8(lanes)));
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpEq(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_cmp_ps(loadF32x8(lhs), loadF32x8(rhs), _CMP_EQ_OQ));
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpNe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_cmp_ps(loadF32x8(lhs), loadF32x8(rhs), _CMP_NEQ_UQ));
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpGe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_cmp_ps(loadF32x8(lhs), loadF32x8(rhs), _CMP_GE_OQ));
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpGt(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_cmp_ps(loadF32x8(lhs), loadF32x8(rhs), _CMP_GT_OQ));
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpLe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_cmp_ps(loadF32x8(lhs), loadF32x8(rhs), _CMP_LE_OQ));
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpLt(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_cmp_ps(loadF32x8(lhs), loadF32x8(rhs), _CMP_LT_OQ));
}

[[nodiscard]] inline std::array<float, 8> f32x8Blend(const std::array<float, 8>& mask,
                                                     const std::array<float, 8>& t,
                                                     const std::array<float, 8>& f) {
  return storeF32x8(_mm256_blendv_ps(loadF32x8(f), loadF32x8(t), loadF32x8(mask)));
}

[[nodiscard]] inline std::array<float, 8> f32x8Abs(const std::array<float, 8>& lanes) {
  const __m256i nonSignMask = _mm256_set1_epi32(0x7fffffff);
  return storeF32x8(_mm256_and_ps(loadF32x8(lanes), _mm256_castsi256_ps(nonSignMask)));
}

[[nodiscard]] inline std::array<float, 8> f32x8Sqrt(const std::array<float, 8>& lanes) {
  return storeF32x8(_mm256_sqrt_ps(loadF32x8(lanes)));
}

[[nodiscard]] inline std::array<float, 8> f32x8RecipFast(const std::array<float, 8>& lanes) {
  const __m256 ones = _mm256_set1_ps(1.0f);
  return storeF32x8(_mm256_div_ps(ones, loadF32x8(lanes)));
}

[[nodiscard]] inline std::array<float, 8> f32x8RecipSqrt(const std::array<float, 8>& lanes) {
  const __m256 ones = _mm256_set1_ps(1.0f);
  return storeF32x8(_mm256_div_ps(ones, _mm256_sqrt_ps(loadF32x8(lanes))));
}

[[nodiscard]] inline std::array<float, 8> f32x8Max(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_max_ps(loadF32x8(lhs), loadF32x8(rhs)));
}

[[nodiscard]] inline std::array<float, 8> f32x8Min(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_min_ps(loadF32x8(lhs), loadF32x8(rhs)));
}

[[nodiscard]] inline std::array<float, 8> f32x8Round(const std::array<float, 8>& lanes) {
  return storeF32x8(
      _mm256_round_ps(loadF32x8(lanes), _MM_FROUND_NO_EXC | _MM_FROUND_TO_NEAREST_INT));
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8RoundInt(const std::array<float, 8>& lanes) {
  return storeI32x8(_mm256_cvtps_epi32(loadF32x8(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8TruncInt(const std::array<float, 8>& lanes) {
  return storeI32x8(_mm256_cvttps_epi32(loadF32x8(lanes)));
}

[[nodiscard]] inline std::array<float, 8> f32x8Add(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_add_ps(loadF32x8(lhs), loadF32x8(rhs)));
}

[[nodiscard]] inline std::array<float, 8> f32x8Sub(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_sub_ps(loadF32x8(lhs), loadF32x8(rhs)));
}

[[nodiscard]] inline std::array<float, 8> f32x8Mul(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_mul_ps(loadF32x8(lhs), loadF32x8(rhs)));
}

[[nodiscard]] inline std::array<float, 8> f32x8Div(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_div_ps(loadF32x8(lhs), loadF32x8(rhs)));
}

[[nodiscard]] inline std::array<float, 8> f32x8BitAnd(const std::array<float, 8>& lhs,
                                                      const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_and_ps(loadF32x8(lhs), loadF32x8(rhs)));
}

[[nodiscard]] inline std::array<float, 8> f32x8BitOr(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_or_ps(loadF32x8(lhs), loadF32x8(rhs)));
}

[[nodiscard]] inline std::array<float, 8> f32x8BitXor(const std::array<float, 8>& lhs,
                                                      const std::array<float, 8>& rhs) {
  return storeF32x8(_mm256_xor_ps(loadF32x8(lhs), loadF32x8(rhs)));
}

[[nodiscard]] inline std::array<float, 8> f32x8BitNot(const std::array<float, 8>& lanes) {
  const __m256i allBits = _mm256_set1_epi32(-1);
  return storeF32x8(_mm256_xor_ps(loadF32x8(lanes), _mm256_castsi256_ps(allBits)));
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

}  // namespace tiny_skia::wide::backend::x86_avx2_fma
