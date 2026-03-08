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
    return FloatPixmap(std::vector<float>(count, 0.0f), width, height);
  }

  /// Creates from a uint8 Pixmap, converting [0,255] → [0,1].
  static FloatPixmap fromPixmap(const Pixmap& pixmap) {
    const auto src = pixmap.data();
    const std::size_t count = src.size();
    std::vector<float> data(count);
    for (std::size_t i = 0; i < count; ++i) {
      data[i] = src[i] / 255.0f;
    }
    return FloatPixmap(std::move(data), pixmap.width(), pixmap.height());
  }

  /// Converts to a uint8 Pixmap, converting [0,1] → [0,255].
  Pixmap toPixmap() const {
    const std::size_t count = data_.size();
    std::vector<std::uint8_t> bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
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
