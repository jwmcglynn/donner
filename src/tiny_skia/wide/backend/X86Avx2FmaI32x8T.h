#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(__AVX2__) && defined(__FMA__) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#endif

#include "tiny_skia/wide/backend/ScalarI32x8T.h"

namespace tiny_skia::wide::backend::x86_avx2_fma {

#if defined(__AVX2__) && defined(__FMA__) && (defined(__x86_64__) || defined(__i386__))

[[nodiscard]] inline __m256i loadI32x8(const std::array<std::int32_t, 8>& lanes) {
  return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lanes.data()));
}

[[nodiscard]] inline std::array<std::int32_t, 8> storeI32x8(__m256i value) {
  std::array<std::int32_t, 8> out{};
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(out.data()), value);
  return out;
}

[[nodiscard]] inline std::array<float, 8> storeF32x8(__m256 value) {
  std::array<float, 8> out{};
  _mm256_storeu_ps(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> storeU32x8(__m256i value) {
  std::array<std::uint32_t, 8> out{};
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(out.data()), value);
  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Blend(const std::array<std::int32_t, 8>& mask,
                                                            const std::array<std::int32_t, 8>& t,
                                                            const std::array<std::int32_t, 8>& f) {
  return storeI32x8(_mm256_blendv_epi8(loadI32x8(f), loadI32x8(t), loadI32x8(mask)));
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8CmpEq(
    const std::array<std::int32_t, 8>& lhs, const std::array<std::int32_t, 8>& rhs) {
  return storeI32x8(_mm256_cmpeq_epi32(loadI32x8(lhs), loadI32x8(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8CmpGt(
    const std::array<std::int32_t, 8>& lhs, const std::array<std::int32_t, 8>& rhs) {
  return storeI32x8(_mm256_cmpgt_epi32(loadI32x8(lhs), loadI32x8(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8CmpLt(
    const std::array<std::int32_t, 8>& lhs, const std::array<std::int32_t, 8>& rhs) {
  return storeI32x8(_mm256_cmpgt_epi32(loadI32x8(rhs), loadI32x8(lhs)));
}

[[nodiscard]] inline std::array<float, 8> i32x8ToF32(const std::array<std::int32_t, 8>& lanes) {
  return storeF32x8(_mm256_cvtepi32_ps(loadI32x8(lanes)));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> i32x8ToU32Bitcast(
    const std::array<std::int32_t, 8>& lanes) {
  return storeU32x8(loadI32x8(lanes));
}

[[nodiscard]] inline std::array<float, 8> i32x8ToF32Bitcast(
    const std::array<std::int32_t, 8>& lanes) {
  return storeF32x8(_mm256_castsi256_ps(loadI32x8(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Add(const std::array<std::int32_t, 8>& lhs,
                                                          const std::array<std::int32_t, 8>& rhs) {
  return storeI32x8(_mm256_add_epi32(loadI32x8(lhs), loadI32x8(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Mul(const std::array<std::int32_t, 8>& lhs,
                                                          const std::array<std::int32_t, 8>& rhs) {
  return storeI32x8(_mm256_mullo_epi32(loadI32x8(lhs), loadI32x8(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8And(const std::array<std::int32_t, 8>& lhs,
                                                          const std::array<std::int32_t, 8>& rhs) {
  return storeI32x8(_mm256_and_si256(loadI32x8(lhs), loadI32x8(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Or(const std::array<std::int32_t, 8>& lhs,
                                                         const std::array<std::int32_t, 8>& rhs) {
  return storeI32x8(_mm256_or_si256(loadI32x8(lhs), loadI32x8(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Xor(const std::array<std::int32_t, 8>& lhs,
                                                          const std::array<std::int32_t, 8>& rhs) {
  return storeI32x8(_mm256_xor_si256(loadI32x8(lhs), loadI32x8(rhs)));
}

#else

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Blend(const std::array<std::int32_t, 8>& mask,
                                                            const std::array<std::int32_t, 8>& t,
                                                            const std::array<std::int32_t, 8>& f) {
  return scalar::i32x8Blend(mask, t, f);
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8CmpEq(
    const std::array<std::int32_t, 8>& lhs, const std::array<std::int32_t, 8>& rhs) {
  return scalar::i32x8CmpEq(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8CmpGt(
    const std::array<std::int32_t, 8>& lhs, const std::array<std::int32_t, 8>& rhs) {
  return scalar::i32x8CmpGt(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8CmpLt(
    const std::array<std::int32_t, 8>& lhs, const std::array<std::int32_t, 8>& rhs) {
  return scalar::i32x8CmpLt(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> i32x8ToF32(const std::array<std::int32_t, 8>& lanes) {
  return scalar::i32x8ToF32(lanes);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> i32x8ToU32Bitcast(
    const std::array<std::int32_t, 8>& lanes) {
  return scalar::i32x8ToU32Bitcast(lanes);
}

[[nodiscard]] inline std::array<float, 8> i32x8ToF32Bitcast(
    const std::array<std::int32_t, 8>& lanes) {
  return scalar::i32x8ToF32Bitcast(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Add(const std::array<std::int32_t, 8>& lhs,
                                                          const std::array<std::int32_t, 8>& rhs) {
  return scalar::i32x8Add(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Mul(const std::array<std::int32_t, 8>& lhs,
                                                          const std::array<std::int32_t, 8>& rhs) {
  return scalar::i32x8Mul(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8And(const std::array<std::int32_t, 8>& lhs,
                                                          const std::array<std::int32_t, 8>& rhs) {
  return scalar::i32x8And(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Or(const std::array<std::int32_t, 8>& lhs,
                                                         const std::array<std::int32_t, 8>& rhs) {
  return scalar::i32x8Or(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Xor(const std::array<std::int32_t, 8>& lhs,
                                                          const std::array<std::int32_t, 8>& rhs) {
  return scalar::i32x8Xor(lhs, rhs);
}

#endif

}  // namespace tiny_skia::wide::backend::x86_avx2_fma
