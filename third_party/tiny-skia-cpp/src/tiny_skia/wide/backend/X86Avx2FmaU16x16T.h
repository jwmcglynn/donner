#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(__AVX2__) && defined(__FMA__) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#endif

#include "tiny_skia/wide/backend/ScalarU16x16T.h"

namespace tiny_skia::wide::backend::x86_avx2_fma {

#if defined(__AVX2__) && defined(__FMA__) && (defined(__x86_64__) || defined(__i386__))

[[nodiscard]] inline __m256i loadU16x16(const U16x16T& value) {
  return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(value.lanes().data()));
}

[[nodiscard]] inline U16x16T storeU16x16(__m256i value) {
  std::array<std::uint16_t, 16> out{};
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(out.data()), value);
  return U16x16T(out);
}

[[nodiscard]] inline __m256i u16AllBitsMask() {
  return _mm256_set1_epi16(static_cast<std::int16_t>(-1));
}

[[nodiscard]] inline __m256i u16SignedOrderMask() {
  return _mm256_set1_epi16(static_cast<std::int16_t>(0x8000u));
}

[[nodiscard]] inline __m256i u16ToSignedDomain(__m256i value) {
  return _mm256_xor_si256(value, u16SignedOrderMask());
}

[[nodiscard]] inline __m256i div255Avx2(__m256i value) {
  const __m256i v128 = _mm256_add_epi16(value, _mm256_set1_epi16(128));
  return _mm256_srli_epi16(_mm256_add_epi16(v128, _mm256_srli_epi16(v128, 8)), 8);
}

[[nodiscard]] inline U16x16T u16x16Min(const U16x16T& lhs, const U16x16T& rhs) {
  return storeU16x16(_mm256_min_epu16(loadU16x16(lhs), loadU16x16(rhs)));
}

[[nodiscard]] inline U16x16T u16x16Max(const U16x16T& lhs, const U16x16T& rhs) {
  return storeU16x16(_mm256_max_epu16(loadU16x16(lhs), loadU16x16(rhs)));
}

[[nodiscard]] inline U16x16T u16x16CmpLe(const U16x16T& lhs, const U16x16T& rhs) {
  const __m256i lhsSigned = u16ToSignedDomain(loadU16x16(lhs));
  const __m256i rhsSigned = u16ToSignedDomain(loadU16x16(rhs));
  const __m256i gtMask = _mm256_cmpgt_epi16(lhsSigned, rhsSigned);
  return storeU16x16(_mm256_xor_si256(gtMask, u16AllBitsMask()));
}

[[nodiscard]] inline U16x16T u16x16Blend(const U16x16T& mask, const U16x16T& t, const U16x16T& e) {
  const __m256i maskLanes = loadU16x16(mask);
  const __m256i trueLanes = loadU16x16(t);
  const __m256i falseLanes = loadU16x16(e);
  return storeU16x16(_mm256_or_si256(_mm256_and_si256(trueLanes, maskLanes),
                                     _mm256_andnot_si256(maskLanes, falseLanes)));
}

[[nodiscard]] inline U16x16T u16x16Add(const U16x16T& lhs, const U16x16T& rhs) {
  return storeU16x16(_mm256_add_epi16(loadU16x16(lhs), loadU16x16(rhs)));
}

[[nodiscard]] inline U16x16T u16x16Sub(const U16x16T& lhs, const U16x16T& rhs) {
  return storeU16x16(_mm256_sub_epi16(loadU16x16(lhs), loadU16x16(rhs)));
}

[[nodiscard]] inline U16x16T u16x16Mul(const U16x16T& lhs, const U16x16T& rhs) {
  return storeU16x16(_mm256_mullo_epi16(loadU16x16(lhs), loadU16x16(rhs)));
}

[[nodiscard]] inline U16x16T u16x16And(const U16x16T& lhs, const U16x16T& rhs) {
  return storeU16x16(_mm256_and_si256(loadU16x16(lhs), loadU16x16(rhs)));
}

[[nodiscard]] inline U16x16T u16x16Or(const U16x16T& lhs, const U16x16T& rhs) {
  return storeU16x16(_mm256_or_si256(loadU16x16(lhs), loadU16x16(rhs)));
}

[[nodiscard]] inline U16x16T u16x16Not(const U16x16T& value) {
  return storeU16x16(_mm256_xor_si256(loadU16x16(value), u16AllBitsMask()));
}

[[nodiscard]] inline U16x16T u16x16Shr(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Shr(lhs, rhs);
}

[[nodiscard]] inline U16x16T u16x16Div255(const U16x16T& value) {
  return storeU16x16(div255Avx2(loadU16x16(value)));
}

[[nodiscard]] inline U16x16T u16x16MulDiv255(const U16x16T& lhs, const U16x16T& rhs) {
  const __m256i product = _mm256_mullo_epi16(loadU16x16(lhs), loadU16x16(rhs));
  return storeU16x16(div255Avx2(product));
}

[[nodiscard]] inline U16x16T u16x16MulAddDiv255(const U16x16T& lhs0, const U16x16T& rhs0,
                                                const U16x16T& lhs1, const U16x16T& rhs1) {
  const __m256i prod0 = _mm256_mullo_epi16(loadU16x16(lhs0), loadU16x16(rhs0));
  const __m256i prod1 = _mm256_mullo_epi16(loadU16x16(lhs1), loadU16x16(rhs1));
  const __m256i sum = _mm256_add_epi16(prod0, prod1);
  return storeU16x16(div255Avx2(sum));
}

[[nodiscard]] inline U16x16T u16x16SourceOver(const U16x16T& source, const U16x16T& dest,
                                              const U16x16T& sourceAlpha) {
  const __m256i max255 = _mm256_set1_epi16(255);
  const __m256i src = loadU16x16(source);
  const __m256i dst = loadU16x16(dest);
  const __m256i srcAlpha = loadU16x16(sourceAlpha);
  const __m256i invSa = _mm256_sub_epi16(max255, srcAlpha);
  const __m256i dstTerm = _mm256_mullo_epi16(dst, invSa);
  const __m256i dstDiv255 = div255Avx2(dstTerm);
  return storeU16x16(_mm256_add_epi16(src, dstDiv255));
}

#else

[[nodiscard]] inline U16x16T u16x16Min(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Min(lhs, rhs);
}

[[nodiscard]] inline U16x16T u16x16Max(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Max(lhs, rhs);
}

[[nodiscard]] inline U16x16T u16x16CmpLe(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16CmpLe(lhs, rhs);
}

[[nodiscard]] inline U16x16T u16x16Blend(const U16x16T& mask, const U16x16T& t, const U16x16T& e) {
  return scalar::u16x16Blend(mask, t, e);
}

[[nodiscard]] inline U16x16T u16x16Add(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Add(lhs, rhs);
}

[[nodiscard]] inline U16x16T u16x16Sub(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Sub(lhs, rhs);
}

[[nodiscard]] inline U16x16T u16x16Mul(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Mul(lhs, rhs);
}

[[nodiscard]] inline U16x16T u16x16And(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16And(lhs, rhs);
}

[[nodiscard]] inline U16x16T u16x16Or(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Or(lhs, rhs);
}

[[nodiscard]] inline U16x16T u16x16Not(const U16x16T& value) { return scalar::u16x16Not(value); }

[[nodiscard]] inline U16x16T u16x16Shr(const U16x16T& lhs, const U16x16T& rhs) {
  return scalar::u16x16Shr(lhs, rhs);
}

[[nodiscard]] inline U16x16T u16x16Div255(const U16x16T& value) {
  const auto v128 = scalar::u16x16Add(value, U16x16T::splat(128));
  return scalar::u16x16Shr(scalar::u16x16Add(v128, scalar::u16x16Shr(v128, U16x16T::splat(8))),
                            U16x16T::splat(8));
}

[[nodiscard]] inline U16x16T u16x16MulDiv255(const U16x16T& lhs, const U16x16T& rhs) {
  return u16x16Div255(scalar::u16x16Mul(lhs, rhs));
}

[[nodiscard]] inline U16x16T u16x16MulAddDiv255(const U16x16T& lhs0, const U16x16T& rhs0,
                                                const U16x16T& lhs1, const U16x16T& rhs1) {
  return u16x16Div255(
      scalar::u16x16Add(scalar::u16x16Mul(lhs0, rhs0), scalar::u16x16Mul(lhs1, rhs1)));
}

[[nodiscard]] inline U16x16T u16x16SourceOver(const U16x16T& source, const U16x16T& dest,
                                              const U16x16T& sourceAlpha) {
  return scalar::u16x16Add(source, u16x16Div255(scalar::u16x16Mul(
                                       dest, scalar::u16x16Sub(U16x16T::splat(255), sourceAlpha))));
}

#endif

}  // namespace tiny_skia::wide::backend::x86_avx2_fma
