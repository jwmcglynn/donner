#pragma once

/// @file FloatPixmap.h
/// @brief Float-precision pixel buffer for filter operations.
///
/// FloatPixmap stores RGBA values as float in [0,1] range with premultiplied alpha.
/// This avoids the 8-bit quantization errors that occur when the sRGB↔linearRGB
/// conversion is done in uint8 space.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#endif

#include "tiny_skia/Pixmap.h"

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
    // Defensive cap: reject allocations > 256MB (matching Pixmap::fromSize).
    constexpr std::size_t kMaxAllocationBytes = 256 * 1024 * 1024;
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
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
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
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
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
#endif
    for (; i < count; ++i) {
      bytes[i] = static_cast<std::uint8_t>(
          std::clamp(data_[i] * 255.0f + 0.5f, 0.0f, 255.0f));
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
  [[nodiscard]] std::span<float> data() {
    return std::span<float>(data_.data(), data_.size());
  }

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
