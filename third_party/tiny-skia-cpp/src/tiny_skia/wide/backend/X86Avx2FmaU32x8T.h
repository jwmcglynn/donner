#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(__AVX2__) && defined(__FMA__) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#endif

#include "tiny_skia/wide/backend/ScalarU32x8T.h"

namespace tiny_skia::wide::backend::x86_avx2_fma {

#if defined(__AVX2__) && defined(__FMA__) && (defined(__x86_64__) || defined(__i386__))

[[nodiscard]] inline __m256i loadU32x8(const std::array<std::uint32_t, 8>& lanes) {
  return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lanes.data()));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> storeU32x8(__m256i value) {
  std::array<std::uint32_t, 8> out{};
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(out.data()), value);
  return out;
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

[[nodiscard]] inline __m256i u32CmpAllBitsMask() { return _mm256_set1_epi32(-1); }

[[nodiscard]] inline __m256i u32SignedOrderMask() { return _mm256_set1_epi32(0x80000000u); }

[[nodiscard]] inline __m256i u32ToSignedDomain(__m256i value) {
  return _mm256_xor_si256(value, u32SignedOrderMask());
}

[[nodiscard]] inline std::array<std::int32_t, 8> u32x8ToI32Bitcast(
    const std::array<std::uint32_t, 8>& lanes) {
  return storeI32x8(loadU32x8(lanes));
}

[[nodiscard]] inline std::array<float, 8> u32x8ToF32Bitcast(
    const std::array<std::uint32_t, 8>& lanes) {
  return storeF32x8(_mm256_castsi256_ps(loadU32x8(lanes)));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpEq(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return storeU32x8(_mm256_cmpeq_epi32(loadU32x8(lhs), loadU32x8(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpNe(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  const __m256i eqMask = _mm256_cmpeq_epi32(loadU32x8(lhs), loadU32x8(rhs));
  return storeU32x8(_mm256_xor_si256(eqMask, u32CmpAllBitsMask()));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpLt(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  const __m256i lhsSigned = u32ToSignedDomain(loadU32x8(lhs));
  const __m256i rhsSigned = u32ToSignedDomain(loadU32x8(rhs));
  return storeU32x8(_mm256_cmpgt_epi32(rhsSigned, lhsSigned));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpLe(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  const __m256i lhsSigned = u32ToSignedDomain(loadU32x8(lhs));
  const __m256i rhsSigned = u32ToSignedDomain(loadU32x8(rhs));
  const __m256i gtMask = _mm256_cmpgt_epi32(lhsSigned, rhsSigned);
  return storeU32x8(_mm256_xor_si256(gtMask, u32CmpAllBitsMask()));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpGt(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  const __m256i lhsSigned = u32ToSignedDomain(loadU32x8(lhs));
  const __m256i rhsSigned = u32ToSignedDomain(loadU32x8(rhs));
  return storeU32x8(_mm256_cmpgt_epi32(lhsSigned, rhsSigned));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpGe(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  const __m256i lhsSigned = u32ToSignedDomain(loadU32x8(lhs));
  const __m256i rhsSigned = u32ToSignedDomain(loadU32x8(rhs));
  const __m256i ltMask = _mm256_cmpgt_epi32(rhsSigned, lhsSigned);
  return storeU32x8(_mm256_xor_si256(ltMask, u32CmpAllBitsMask()));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8Not(
    const std::array<std::uint32_t, 8>& lanes) {
  return storeU32x8(_mm256_xor_si256(loadU32x8(lanes), u32CmpAllBitsMask()));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8Add(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return storeU32x8(_mm256_add_epi32(loadU32x8(lhs), loadU32x8(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8And(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return storeU32x8(_mm256_and_si256(loadU32x8(lhs), loadU32x8(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8Or(const std::array<std::uint32_t, 8>& lhs,
                                                          const std::array<std::uint32_t, 8>& rhs) {
  return storeU32x8(_mm256_or_si256(loadU32x8(lhs), loadU32x8(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8Xor(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return storeU32x8(_mm256_xor_si256(loadU32x8(lhs), loadU32x8(rhs)));
}

#else

[[nodiscard]] inline std::array<std::int32_t, 8> u32x8ToI32Bitcast(
    const std::array<std::uint32_t, 8>& lanes) {
  return scalar::u32x8ToI32Bitcast(lanes);
}

[[nodiscard]] inline std::array<float, 8> u32x8ToF32Bitcast(
    const std::array<std::uint32_t, 8>& lanes) {
  return scalar::u32x8ToF32Bitcast(lanes);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpEq(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return scalar::u32x8CmpEq(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpNe(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return scalar::u32x8CmpNe(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpLt(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return scalar::u32x8CmpLt(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpLe(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return scalar::u32x8CmpLe(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpGt(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return scalar::u32x8CmpGt(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpGe(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return scalar::u32x8CmpGe(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8Not(
    const std::array<std::uint32_t, 8>& lanes) {
  return scalar::u32x8Not(lanes);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8Add(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return scalar::u32x8Add(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8And(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return scalar::u32x8And(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8Or(const std::array<std::uint32_t, 8>& lhs,
                                                          const std::array<std::uint32_t, 8>& rhs) {
  return scalar::u32x8Or(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8Xor(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  return scalar::u32x8Xor(lhs, rhs);
}

#endif

}  // namespace tiny_skia::wide::backend::x86_avx2_fma
