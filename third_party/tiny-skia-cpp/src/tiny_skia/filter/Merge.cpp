#include "tiny_skia/filter/Merge.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace tiny_skia::filter {

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

#ifdef __ARM_NEON
    // Process 4 pixels (16 bytes) at a time with NEON.
    std::size_t i = 0;
    const std::size_t simdCount = pixelCount & ~3u;

    for (; i < simdCount; i += 4) {
      const std::size_t off = i * 4;

      // Load 4 pixels (16 bytes) from src and dst.
      uint8x16_t srcPx = vld1q_u8(&src[off]);
      uint8x16_t dstPx = vld1q_u8(&out[off]);

      // Extract alpha channel from each of the 4 source pixels.
      // srcPx layout: [R0 G0 B0 A0 R1 G1 B1 A1 R2 G2 B2 A2 R3 G3 B3 A3]
      // We need A0, A1, A2, A3 -> broadcast each to its pixel's 4 lanes.

      // Widen src and dst to 16-bit for arithmetic.
      uint8x8_t srcLo = vget_low_u8(srcPx);
      uint8x8_t srcHi = vget_high_u8(srcPx);
      uint8x8_t dstLo = vget_low_u8(dstPx);
      uint8x8_t dstHi = vget_high_u8(dstPx);

      uint16x8_t srcW0 = vmovl_u8(srcLo);
      uint16x8_t srcW1 = vmovl_u8(srcHi);
      uint16x8_t dstW0 = vmovl_u8(dstLo);
      uint16x8_t dstW1 = vmovl_u8(dstHi);

      // Extract and broadcast alpha for each pixel.
      // Pixel 0: alpha at index 3, Pixel 1: alpha at index 7, etc.
      uint16_t a0 = vgetq_lane_u16(srcW0, 3);
      uint16_t a1 = vgetq_lane_u16(srcW0, 7);
      uint16_t a2 = vgetq_lane_u16(srcW1, 3);
      uint16_t a3 = vgetq_lane_u16(srcW1, 7);

      // Build invAlpha vector: 255 - srcAlpha, broadcast per pixel.
      uint16x8_t invA0 = vdupq_n_u16(255 - a0);
      uint16x8_t invA1 = vdupq_n_u16(255 - a1);
      uint16x8_t invA2 = vdupq_n_u16(255 - a2);
      uint16x8_t invA3 = vdupq_n_u16(255 - a3);

      // Combine invAlpha for the 4 pixels into two uint16x8 vectors.
      // Pixels 0-1 interleaved in invAlphaLo, pixels 2-3 in invAlphaHi.
      uint16x4_t invALo0 = vget_low_u16(invA0);
      uint16x4_t invALo1 = vget_low_u16(invA1);
      uint16x4_t invAHi0 = vget_low_u16(invA2);
      uint16x4_t invAHi1 = vget_low_u16(invA3);
      uint16x8_t invAlphaLo = vcombine_u16(invALo0, invALo1);
      uint16x8_t invAlphaHi = vcombine_u16(invAHi0, invAHi1);

      // dst * invAlpha: 16-bit multiply.
      uint16x8_t prod0 = vmulq_u16(dstW0, invAlphaLo);
      uint16x8_t prod1 = vmulq_u16(dstW1, invAlphaHi);

      // div255: (v + 128 + ((v + 128) >> 8)) >> 8
      uint16x8_t t0 = vaddq_u16(prod0, vdupq_n_u16(128));
      uint16x8_t t1 = vaddq_u16(prod1, vdupq_n_u16(128));
      t0 = vshrq_n_u16(vaddq_u16(t0, vshrq_n_u16(t0, 8)), 8);
      t1 = vshrq_n_u16(vaddq_u16(t1, vshrq_n_u16(t1, 8)), 8);

      // result = src + div255(dst * invAlpha).
      uint16x8_t res0 = vaddq_u16(srcW0, t0);
      uint16x8_t res1 = vaddq_u16(srcW1, t1);

      // Narrow back to uint8, saturating.
      uint8x8_t resLo = vqmovn_u16(res0);
      uint8x8_t resHi = vqmovn_u16(res1);
      uint8x16_t result = vcombine_u8(resLo, resHi);

      vst1q_u8(&out[off], result);
    }

    // Handle remaining pixels (0-3).
    for (; i < pixelCount; ++i) {
      const std::size_t off = i * 4;
#else
    for (std::size_t i = 0; i < pixelCount; ++i) {
      const std::size_t off = i * 4;
#endif
      const std::uint32_t sa = src[off + 3];
      if (sa == 0) {
        continue;
      }
      if (sa == 255) {
        std::memcpy(&out[off], &src[off], 4);
        continue;
      }
      const std::uint32_t invSa = 255 - sa;
      auto div255 = [](std::uint32_t v) -> std::uint8_t {
        return static_cast<std::uint8_t>((v + 128 + ((v + 128) >> 8)) >> 8);
      };
      out[off + 0] = static_cast<std::uint8_t>(
          std::min(255u, static_cast<std::uint32_t>(src[off + 0]) +
                             div255(static_cast<std::uint32_t>(out[off + 0]) * invSa)));
      out[off + 1] = static_cast<std::uint8_t>(
          std::min(255u, static_cast<std::uint32_t>(src[off + 1]) +
                             div255(static_cast<std::uint32_t>(out[off + 1]) * invSa)));
      out[off + 2] = static_cast<std::uint8_t>(
          std::min(255u, static_cast<std::uint32_t>(src[off + 2]) +
                             div255(static_cast<std::uint32_t>(out[off + 2]) * invSa)));
      out[off + 3] = static_cast<std::uint8_t>(
          std::min(255u, static_cast<std::uint32_t>(src[off + 3]) +
                             div255(static_cast<std::uint32_t>(out[off + 3]) * invSa)));
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
