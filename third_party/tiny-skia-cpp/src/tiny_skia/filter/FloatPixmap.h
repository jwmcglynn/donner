#pragma once

/// @file FloatPixmap.h
/// @brief Float-precision pixel buffer for filter operations.
///
/// FloatPixmap stores RGBA values as float in [0,1] range with premultiplied alpha.
/// This avoids the 8-bit quantization errors that occur when the sRGB↔linearRGB
/// conversion is done in uint8 space.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "tiny_skia/Pixmap.h"
// Selects the ISA branch below and pulls in the matching intrinsics header.
#include "tiny_skia/filter/SimdVec.h"

namespace tiny_skia::filter {

/// Float-precision pixel buffer (RGBA premultiplied, values in [0,1]).
class FloatPixmap {
 public:
  FloatPixmap() = default;

  /// Creates a zero-filled float pixmap.
  static std::optional<FloatPixmap> fromSize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
      return std::nullopt;
    }
    const std::size_t count = static_cast<std::size_t>(width) * height * 4;
    // Defensive cap: reject malicious / accidental large allocations rather
    // than crashing the process. With -fno-exceptions, OOM calls std::terminate
    // instead of throwing. 1 GiB cap matches `Pixmap::fromSize` and the filter
    // primitives' caps so a `FloatPixmap` can hold the float conversion of
    // any `Pixmap` the allocator already accepted. The prior 64 MiB capped
    // float pixmaps at ~2048×2048, which contributed to filters silently
    // failing at high editor zoom - see `GaussianBlur.cpp` and
    // `ZoomFilterRepro_tests.cc`.
    constexpr std::size_t kMaxAllocationBytes = 1024ULL * 1024ULL * 1024ULL;
    if (count * sizeof(float) > kMaxAllocationBytes) {
      return std::nullopt;
    }
    return FloatPixmap(std::vector<float>(count, 0.0f), width, height);
  }

  /// Creates from a uint8 Pixmap, converting [0,255] → [0,1].
  static FloatPixmap fromPixmap(const Pixmap& pixmap) {
    const auto src = pixmap.data();
    const std::size_t count = src.size();
    std::vector<float> data(count);
    std::size_t i = 0;
#if defined(TINY_SKIA_SIMD_NEON)
    const float32x4_t scale = vdupq_n_f32(1.0f / 255.0f);
    // Process 16 bytes (4 RGBA pixels) at a time.
    for (; i + 16 <= count; i += 16) {
      const uint8x16_t bytes = vld1q_u8(&src[i]);
      // Widen 8→16→32 and convert to float.
      const uint8x8_t lo8 = vget_low_u8(bytes);
      const uint8x8_t hi8 = vget_high_u8(bytes);
      const uint16x8_t lo16 = vmovl_u8(lo8);
      const uint16x8_t hi16 = vmovl_u8(hi8);

      const uint32x4_t u0 = vmovl_u16(vget_low_u16(lo16));
      const uint32x4_t u1 = vmovl_u16(vget_high_u16(lo16));
      const uint32x4_t u2 = vmovl_u16(vget_low_u16(hi16));
      const uint32x4_t u3 = vmovl_u16(vget_high_u16(hi16));

      vst1q_f32(&data[i + 0], vmulq_f32(vcvtq_f32_u32(u0), scale));
      vst1q_f32(&data[i + 4], vmulq_f32(vcvtq_f32_u32(u1), scale));
      vst1q_f32(&data[i + 8], vmulq_f32(vcvtq_f32_u32(u2), scale));
      vst1q_f32(&data[i + 12], vmulq_f32(vcvtq_f32_u32(u3), scale));
    }
#elif defined(TINY_SKIA_SIMD_WASM_SIMD128)
    // Divides instead of multiplying by the reciprocal of 255. 1/255 is not
    // exactly representable, and the rounded reciprocal disagrees with the
    // divide in the last place for 126 of the 256 byte values, so only the
    // divide reproduces the scalar tail below bit for bit. The NEON branch
    // above multiplies; that predates this branch and changing it would move
    // rendered output on ARM.
    const v128_t scale = wasm_f32x4_splat(255.0f);
    // Process 16 bytes (4 RGBA pixels) at a time.
    for (; i + 16 <= count; i += 16) {
      const v128_t bytes = wasm_v128_load(&src[i]);
      // Widen 8->16->32 and convert to float.
      const v128_t lo16 = wasm_u16x8_extend_low_u8x16(bytes);
      const v128_t hi16 = wasm_u16x8_extend_high_u8x16(bytes);

      const v128_t f0 = wasm_f32x4_convert_u32x4(wasm_u32x4_extend_low_u16x8(lo16));
      const v128_t f1 = wasm_f32x4_convert_u32x4(wasm_u32x4_extend_high_u16x8(lo16));
      const v128_t f2 = wasm_f32x4_convert_u32x4(wasm_u32x4_extend_low_u16x8(hi16));
      const v128_t f3 = wasm_f32x4_convert_u32x4(wasm_u32x4_extend_high_u16x8(hi16));

      wasm_v128_store(&data[i + 0], wasm_f32x4_div(f0, scale));
      wasm_v128_store(&data[i + 4], wasm_f32x4_div(f1, scale));
      wasm_v128_store(&data[i + 8], wasm_f32x4_div(f2, scale));
      wasm_v128_store(&data[i + 12], wasm_f32x4_div(f3, scale));
    }
#elif defined(TINY_SKIA_SIMD_SSE2)
    // Divides for the same reason as the wasm128 branch above: the rounded
    // reciprocal of 255 is not bit-exact against the scalar tail.
    const __m128 scale = _mm_set1_ps(255.0f);
    const __m128i zero = _mm_setzero_si128();
    // Process 16 bytes (4 RGBA pixels) at a time.
    for (; i + 16 <= count; i += 16) {
      const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&src[i]));
      // Widen 8->16->32 by interleaving with zero, then convert to float. Every
      // lane is at most 255, so the signed 32-bit conversion is exact.
      const __m128i lo16 = _mm_unpacklo_epi8(bytes, zero);
      const __m128i hi16 = _mm_unpackhi_epi8(bytes, zero);

      const __m128 f0 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(lo16, zero));
      const __m128 f1 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(lo16, zero));
      const __m128 f2 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(hi16, zero));
      const __m128 f3 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(hi16, zero));

      _mm_storeu_ps(&data[i + 0], _mm_div_ps(f0, scale));
      _mm_storeu_ps(&data[i + 4], _mm_div_ps(f1, scale));
      _mm_storeu_ps(&data[i + 8], _mm_div_ps(f2, scale));
      _mm_storeu_ps(&data[i + 12], _mm_div_ps(f3, scale));
    }
#endif
    for (; i < count; ++i) {
      data[i] = src[i] / 255.0f;
    }
    return FloatPixmap(std::move(data), pixmap.width(), pixmap.height());
  }

  /// Converts to a uint8 Pixmap, converting [0,1] → [0,255].
  Pixmap toPixmap() const {
    const std::size_t count = data_.size();
    std::vector<std::uint8_t> bytes(count);
    std::size_t i = 0;
#if defined(TINY_SKIA_SIMD_NEON)
    const float32x4_t scale = vdupq_n_f32(255.0f);
    const float32x4_t half = vdupq_n_f32(0.5f);
    // Process 16 floats (4 RGBA pixels) at a time.
    for (; i + 16 <= count; i += 16) {
      // Multiply by 255, add 0.5 for rounding, convert to uint32, narrow to uint8.
      const float32x4_t f0 = vaddq_f32(vmulq_f32(vld1q_f32(&data_[i + 0]), scale), half);
      const float32x4_t f1 = vaddq_f32(vmulq_f32(vld1q_f32(&data_[i + 4]), scale), half);
      const float32x4_t f2 = vaddq_f32(vmulq_f32(vld1q_f32(&data_[i + 8]), scale), half);
      const float32x4_t f3 = vaddq_f32(vmulq_f32(vld1q_f32(&data_[i + 12]), scale), half);

      // Clamp to [0, 255] then convert to uint32.
      const uint32x4_t u0 = vcvtq_u32_f32(vmaxq_f32(vminq_f32(f0, scale), vdupq_n_f32(0.0f)));
      const uint32x4_t u1 = vcvtq_u32_f32(vmaxq_f32(vminq_f32(f1, scale), vdupq_n_f32(0.0f)));
      const uint32x4_t u2 = vcvtq_u32_f32(vmaxq_f32(vminq_f32(f2, scale), vdupq_n_f32(0.0f)));
      const uint32x4_t u3 = vcvtq_u32_f32(vmaxq_f32(vminq_f32(f3, scale), vdupq_n_f32(0.0f)));

      // Narrow 32→16→8.
      const uint16x4_t n0 = vmovn_u32(u0);
      const uint16x4_t n1 = vmovn_u32(u1);
      const uint16x4_t n2 = vmovn_u32(u2);
      const uint16x4_t n3 = vmovn_u32(u3);
      const uint16x8_t lo = vcombine_u16(n0, n1);
      const uint16x8_t hi = vcombine_u16(n2, n3);
      const uint8x8_t b0 = vmovn_u16(lo);
      const uint8x8_t b1 = vmovn_u16(hi);
      vst1q_u8(&bytes[i], vcombine_u8(b0, b1));
    }
#elif defined(TINY_SKIA_SIMD_WASM_SIMD128)
    const v128_t scale = wasm_f32x4_splat(255.0f);
    const v128_t half = wasm_f32x4_splat(0.5f);
    const v128_t zero = wasm_f32x4_splat(0.0f);
    // Process 16 floats (4 RGBA pixels) at a time.
    for (; i + 16 <= count; i += 16) {
      // Multiply by 255 and add 0.5 for rounding, as two separately rounded
      // operations: wasm128 has no fused multiply-add outside relaxed SIMD, so
      // this rounds exactly where the scalar tail rounds.
      const v128_t f0 = wasm_f32x4_add(wasm_f32x4_mul(wasm_v128_load(&data_[i + 0]), scale), half);
      const v128_t f1 = wasm_f32x4_add(wasm_f32x4_mul(wasm_v128_load(&data_[i + 4]), scale), half);
      const v128_t f2 = wasm_f32x4_add(wasm_f32x4_mul(wasm_v128_load(&data_[i + 8]), scale), half);
      const v128_t f3 = wasm_f32x4_add(wasm_f32x4_mul(wasm_v128_load(&data_[i + 12]), scale), half);

      // pmin and pmax are plain lane selects, pmin(x, y) = y < x ? y : x and
      // pmax(x, y) = x < y ? y : x, so this operand order evaluates
      // std::clamp's `v < lo ? lo : (hi < v ? hi : v)` exactly. The IEEE
      // f32x4.min/max would order the operands the other way round.
      const v128_t c0 = wasm_f32x4_pmin(wasm_f32x4_pmax(f0, zero), scale);
      const v128_t c1 = wasm_f32x4_pmin(wasm_f32x4_pmax(f1, zero), scale);
      const v128_t c2 = wasm_f32x4_pmin(wasm_f32x4_pmax(f2, zero), scale);
      const v128_t c3 = wasm_f32x4_pmin(wasm_f32x4_pmax(f3, zero), scale);

      // Truncation toward zero is what the scalar cast to uint8 does. The
      // conversion's saturation only applies to inputs the clamp already
      // removed, and its one remaining special case is NaN, which the clamp
      // passes through and this conversion maps to zero.
      const v128_t u0 = wasm_u32x4_trunc_sat_f32x4(c0);
      const v128_t u1 = wasm_u32x4_trunc_sat_f32x4(c1);
      const v128_t u2 = wasm_u32x4_trunc_sat_f32x4(c2);
      const v128_t u3 = wasm_u32x4_trunc_sat_f32x4(c3);

      // Narrow 32->16->8. Every lane is already in [0, 255], so the signed
      // saturating narrows never actually saturate.
      const v128_t lo = wasm_i16x8_narrow_i32x4(u0, u1);
      const v128_t hi = wasm_i16x8_narrow_i32x4(u2, u3);
      wasm_v128_store(&bytes[i], wasm_u8x16_narrow_i16x8(lo, hi));
    }
#elif defined(TINY_SKIA_SIMD_SSE2)
    const __m128 scale = _mm_set1_ps(255.0f);
    const __m128 half = _mm_set1_ps(0.5f);
    const __m128 zero = _mm_setzero_ps();
    // Process 16 floats (4 RGBA pixels) at a time.
    for (; i + 16 <= count; i += 16) {
      const __m128 f0 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(&data_[i + 0]), scale), half);
      const __m128 f1 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(&data_[i + 4]), scale), half);
      const __m128 f2 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(&data_[i + 8]), scale), half);
      const __m128 f3 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(&data_[i + 12]), scale), half);

      // _mm_max_ps(a, b) selects `a > b ? a : b` and _mm_min_ps(a, b) selects
      // `a < b ? a : b`, so naming the bound first evaluates std::clamp's
      // `v < lo ? lo : (hi < v ? hi : v)` exactly.
      const __m128 c0 = _mm_min_ps(scale, _mm_max_ps(zero, f0));
      const __m128 c1 = _mm_min_ps(scale, _mm_max_ps(zero, f1));
      const __m128 c2 = _mm_min_ps(scale, _mm_max_ps(zero, f2));
      const __m128 c3 = _mm_min_ps(scale, _mm_max_ps(zero, f3));

      // Truncates toward zero, like the scalar cast to uint8.
      const __m128i u0 = _mm_cvttps_epi32(c0);
      const __m128i u1 = _mm_cvttps_epi32(c1);
      const __m128i u2 = _mm_cvttps_epi32(c2);
      const __m128i u3 = _mm_cvttps_epi32(c3);

      // Narrow 32->16->8. A lane that came from a finite value is already in
      // [0, 255] and both packs leave it alone. A NaN lane is the one case that
      // does saturate, and it has to: the clamp passes NaN through, the
      // conversion turns it into INT_MIN, the signed pack clamps that to
      // -32768, and the unsigned pack clamps that to 0. That is the same byte
      // the wasm128 branch's saturating conversion produces for NaN.
      const __m128i lo = _mm_packs_epi32(u0, u1);
      const __m128i hi = _mm_packs_epi32(u2, u3);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&bytes[i]), _mm_packus_epi16(lo, hi));
    }
#endif
    for (; i < count; ++i) {
      const float scaled = data_[i] * 255.0f + 0.5f;
      bytes[i] =
          std::isfinite(scaled) ? static_cast<std::uint8_t>(std::clamp(scaled, 0.0f, 255.0f)) : 0;
    }
    return *Pixmap::fromVec(std::move(bytes), IntSize::fromWH(width_, height_).value());
  }

  [[nodiscard]] std::uint32_t width() const { return width_; }
  [[nodiscard]] std::uint32_t height() const { return height_; }

  /// Raw float data (premultiplied RGBA, values in [0,1]).
  [[nodiscard]] std::span<const float> data() const {
    return std::span<const float>(data_.data(), data_.size());
  }

  /// Mutable float data.
  [[nodiscard]] std::span<float> data() { return std::span<float>(data_.data(), data_.size()); }

  /// Fill with a color (values already in [0,1] premultiplied).
  void fill(float r, float g, float b, float a) {
    const std::size_t pixelCount = static_cast<std::size_t>(width_) * height_;
    for (std::size_t i = 0; i < pixelCount; ++i) {
      data_[i * 4 + 0] = r;
      data_[i * 4 + 1] = g;
      data_[i * 4 + 2] = b;
      data_[i * 4 + 3] = a;
    }
  }

  /// Fill with transparent black.
  void clear() { std::fill(data_.begin(), data_.end(), 0.0f); }

 private:
  explicit FloatPixmap(std::vector<float> data, std::uint32_t width, std::uint32_t height)
      : data_(std::move(data)), width_(width), height_(height) {}

  std::vector<float> data_;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
};

}  // namespace tiny_skia::filter
