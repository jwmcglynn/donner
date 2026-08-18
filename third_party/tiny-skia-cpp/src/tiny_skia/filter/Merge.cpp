#include "tiny_skia/filter/Merge.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Selects the ISA branch below and pulls in the matching intrinsics header.
#include "tiny_skia/filter/SimdVec.h"

namespace tiny_skia::filter {

namespace {

/// Skia's div255: (v + 128 + ((v + 128) >> 8)) >> 8, exact for v <= 255*255.
std::uint32_t div255(std::uint32_t v) { return (v + 128 + ((v + 128) >> 8)) >> 8; }

/// Source-Over of one 8-bit pixel: dst = src + div255(dst * (255 - srcAlpha)),
/// saturated at 255.
///
/// Used directly by the scalar path and as the vector paths' tail, so both
/// produce the same bytes for a partial group of pixels.
void mergePixel(std::uint8_t* out, const std::uint8_t* src) {
  const std::uint32_t sa = src[3];
  if (sa == 0) {
    return;
  }
  if (sa == 255) {
    std::memcpy(out, src, 4);
    return;
  }

  const std::uint32_t invSa = 255 - sa;
  for (std::size_t channel = 0; channel < 4; ++channel) {
    out[channel] = static_cast<std::uint8_t>(
        std::min(255u, static_cast<std::uint32_t>(src[channel]) +
                           div255(static_cast<std::uint32_t>(out[channel]) * invSa)));
  }
}

#if defined(TINY_SKIA_SIMD_NEON) || defined(TINY_SKIA_SIMD_WASM_SIMD128) || \
    defined(TINY_SKIA_SIMD_SSE2)
#define TINY_SKIA_FILTER_MERGE_VECTOR 1

/// Source-Over of four consecutive 8-bit pixels (16 bytes).
///
/// Every branch evaluates the general formula for all four pixels, without
/// `mergePixel`'s two shortcuts. Both shortcuts are identities of the general
/// formula on premultiplied input: at srcAlpha 255 the `dst * 0` term vanishes,
/// and at srcAlpha 0 the term is `div255(dst * 255) == dst` while a
/// premultiplied source with zero alpha has zero color. Every intermediate
/// stays in 16-bit range (255 * 255 + 128 + 254 = 65407), and the final
/// saturating narrow reproduces `std::min(255u, ...)` because the sum of two
/// values at most 255 is never negative when read as signed.
void mergeFourPixels(std::uint8_t* out, const std::uint8_t* src) {
#if defined(TINY_SKIA_SIMD_NEON)
  const uint8x16_t srcPx = vld1q_u8(src);
  const uint8x16_t dstPx = vld1q_u8(out);

  // Widen src and dst to 16-bit for arithmetic.
  // srcPx layout: [R0 G0 B0 A0 R1 G1 B1 A1 R2 G2 B2 A2 R3 G3 B3 A3]
  const uint16x8_t srcW0 = vmovl_u8(vget_low_u8(srcPx));
  const uint16x8_t srcW1 = vmovl_u8(vget_high_u8(srcPx));
  const uint16x8_t dstW0 = vmovl_u8(vget_low_u8(dstPx));
  const uint16x8_t dstW1 = vmovl_u8(vget_high_u8(dstPx));

  // Broadcast 255 - srcAlpha across each pixel's four lanes.
  const uint16x8_t invA0 = vdupq_n_u16(255 - vgetq_lane_u16(srcW0, 3));
  const uint16x8_t invA1 = vdupq_n_u16(255 - vgetq_lane_u16(srcW0, 7));
  const uint16x8_t invA2 = vdupq_n_u16(255 - vgetq_lane_u16(srcW1, 3));
  const uint16x8_t invA3 = vdupq_n_u16(255 - vgetq_lane_u16(srcW1, 7));
  const uint16x8_t invAlphaLo = vcombine_u16(vget_low_u16(invA0), vget_low_u16(invA1));
  const uint16x8_t invAlphaHi = vcombine_u16(vget_low_u16(invA2), vget_low_u16(invA3));

  // div255(dst * invAlpha).
  uint16x8_t t0 = vaddq_u16(vmulq_u16(dstW0, invAlphaLo), vdupq_n_u16(128));
  uint16x8_t t1 = vaddq_u16(vmulq_u16(dstW1, invAlphaHi), vdupq_n_u16(128));
  t0 = vshrq_n_u16(vaddq_u16(t0, vshrq_n_u16(t0, 8)), 8);
  t1 = vshrq_n_u16(vaddq_u16(t1, vshrq_n_u16(t1, 8)), 8);

  // result = src + div255(dst * invAlpha), narrowed back to uint8 saturating.
  const uint8x8_t resLo = vqmovn_u16(vaddq_u16(srcW0, t0));
  const uint8x8_t resHi = vqmovn_u16(vaddq_u16(srcW1, t1));
  vst1q_u8(out, vcombine_u8(resLo, resHi));

#elif defined(TINY_SKIA_SIMD_WASM_SIMD128)
  const v128_t srcPx = wasm_v128_load(src);
  const v128_t dstPx = wasm_v128_load(out);

  // Broadcast each pixel's alpha byte over its own four bytes, then subtract
  // from 255 in byte lanes: 255 - alpha never underflows.
  const v128_t alphaBytes =
      wasm_i8x16_shuffle(srcPx, srcPx, 3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15);
  const v128_t invAlphaBytes = wasm_i8x16_sub(wasm_u8x16_splat(255), alphaBytes);

  // Widen src, dst, and the inverse alphas to 16-bit for arithmetic.
  const v128_t srcW0 = wasm_u16x8_extend_low_u8x16(srcPx);
  const v128_t srcW1 = wasm_u16x8_extend_high_u8x16(srcPx);
  const v128_t dstW0 = wasm_u16x8_extend_low_u8x16(dstPx);
  const v128_t dstW1 = wasm_u16x8_extend_high_u8x16(dstPx);
  const v128_t invAlphaLo = wasm_u16x8_extend_low_u8x16(invAlphaBytes);
  const v128_t invAlphaHi = wasm_u16x8_extend_high_u8x16(invAlphaBytes);

  // div255(dst * invAlpha). The shifts must be logical, so they use the u16
  // form; the adds and the multiply are bit-identical either way.
  v128_t t0 = wasm_i16x8_add(wasm_i16x8_mul(dstW0, invAlphaLo), wasm_i16x8_splat(128));
  v128_t t1 = wasm_i16x8_add(wasm_i16x8_mul(dstW1, invAlphaHi), wasm_i16x8_splat(128));
  t0 = wasm_u16x8_shr(wasm_i16x8_add(t0, wasm_u16x8_shr(t0, 8)), 8);
  t1 = wasm_u16x8_shr(wasm_i16x8_add(t1, wasm_u16x8_shr(t1, 8)), 8);

  // result = src + div255(dst * invAlpha), narrowed back to uint8 saturating.
  const v128_t res0 = wasm_i16x8_add(srcW0, t0);
  const v128_t res1 = wasm_i16x8_add(srcW1, t1);
  wasm_v128_store(out, wasm_u8x16_narrow_i16x8(res0, res1));

#elif defined(TINY_SKIA_SIMD_SSE2)
  const __m128i zero = _mm_setzero_si128();
  const __m128i srcPx = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
  const __m128i dstPx = _mm_loadu_si128(reinterpret_cast<const __m128i*>(out));

  // Widen src and dst to 16-bit for arithmetic.
  const __m128i srcW0 = _mm_unpacklo_epi8(srcPx, zero);
  const __m128i srcW1 = _mm_unpackhi_epi8(srcPx, zero);
  const __m128i dstW0 = _mm_unpacklo_epi8(dstPx, zero);
  const __m128i dstW1 = _mm_unpackhi_epi8(dstPx, zero);

  // Broadcast each pixel's alpha (lane 3 of its half) over that pixel's four
  // lanes, then subtract from 255. SSE2 has no byte shuffle, so this uses the
  // 16-bit half shuffles, which are exactly the right granularity here.
  const __m128i alphaW0 = _mm_shufflehi_epi16(_mm_shufflelo_epi16(srcW0, _MM_SHUFFLE(3, 3, 3, 3)),
                                              _MM_SHUFFLE(3, 3, 3, 3));
  const __m128i alphaW1 = _mm_shufflehi_epi16(_mm_shufflelo_epi16(srcW1, _MM_SHUFFLE(3, 3, 3, 3)),
                                              _MM_SHUFFLE(3, 3, 3, 3));
  const __m128i full = _mm_set1_epi16(255);
  const __m128i invAlphaLo = _mm_sub_epi16(full, alphaW0);
  const __m128i invAlphaHi = _mm_sub_epi16(full, alphaW1);

  // div255(dst * invAlpha). The shifts must be logical, hence _mm_srli_epi16.
  const __m128i round = _mm_set1_epi16(128);
  __m128i t0 = _mm_add_epi16(_mm_mullo_epi16(dstW0, invAlphaLo), round);
  __m128i t1 = _mm_add_epi16(_mm_mullo_epi16(dstW1, invAlphaHi), round);
  t0 = _mm_srli_epi16(_mm_add_epi16(t0, _mm_srli_epi16(t0, 8)), 8);
  t1 = _mm_srli_epi16(_mm_add_epi16(t1, _mm_srli_epi16(t1, 8)), 8);

  // result = src + div255(dst * invAlpha), narrowed back to uint8 saturating.
  const __m128i res0 = _mm_add_epi16(srcW0, t0);
  const __m128i res1 = _mm_add_epi16(srcW1, t1);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_packus_epi16(res0, res1));
#endif
}
#endif

}  // namespace

void merge(std::span<const Pixmap* const> layers, Pixmap& dst) {
  auto out = dst.data();

  // Clear to transparent black.
  std::fill(out.begin(), out.end(), std::uint8_t{0});

  for (const Pixmap* layer : layers) {
    if (!layer) {
      continue;
    }

    const auto src = layer->data();
    const std::size_t byteCount = std::min(src.size(), out.size());
    const std::size_t pixelCount = byteCount / 4;
    std::size_t i = 0;

#if defined(TINY_SKIA_FILTER_MERGE_VECTOR)
    // Process 4 pixels (16 bytes) at a time.
    const std::size_t vectorCount = pixelCount & ~std::size_t{3};
    for (; i < vectorCount; i += 4) {
      mergeFourPixels(&out[i * 4], &src[i * 4]);
    }
#endif

    // Handle the remaining pixels one at a time.
    for (; i < pixelCount; ++i) {
      mergePixel(&out[i * 4], &src[i * 4]);
    }
  }
}

void merge(std::span<const FloatPixmap* const> layers, FloatPixmap& dst) {
  auto out = dst.data();

  // Clear to transparent black.
  dst.clear();

  for (const FloatPixmap* layer : layers) {
    if (!layer) {
      continue;
    }

    const auto src = layer->data();
    const std::size_t count = std::min(src.size(), out.size()) / 4;

    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t off = i * 4;

      // Source Over: dst = src + dst * (1 - srcA)
      const float sa = src[off + 3];
      const float oneMinusSa = 1.0f - sa;

      out[off + 0] = std::clamp(src[off + 0] + out[off + 0] * oneMinusSa, 0.0f, 1.0f);
      out[off + 1] = std::clamp(src[off + 1] + out[off + 1] * oneMinusSa, 0.0f, 1.0f);
      out[off + 2] = std::clamp(src[off + 2] + out[off + 2] * oneMinusSa, 0.0f, 1.0f);
      out[off + 3] = std::clamp(src[off + 3] + out[off + 3] * oneMinusSa, 0.0f, 1.0f);
    }
  }
}

}  // namespace tiny_skia::filter

#undef TINY_SKIA_FILTER_MERGE_VECTOR
