#include "tiny_skia/filter/Merge.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

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
    const std::size_t count = std::min(src.size(), out.size()) / 4;

    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t off = i * 4;

      // Source Over: dst = src + dst * (1 - srcA)
      const double sa = src[off + 3] / 255.0;
      const double oneMinusSa = 1.0 - sa;

      out[off + 0] = static_cast<std::uint8_t>(
          std::clamp(src[off + 0] + out[off + 0] * oneMinusSa, 0.0, 255.0));
      out[off + 1] = static_cast<std::uint8_t>(
          std::clamp(src[off + 1] + out[off + 1] * oneMinusSa, 0.0, 255.0));
      out[off + 2] = static_cast<std::uint8_t>(
          std::clamp(src[off + 2] + out[off + 2] * oneMinusSa, 0.0, 255.0));
      out[off + 3] = static_cast<std::uint8_t>(
          std::clamp(src[off + 3] + out[off + 3] * oneMinusSa, 0.0, 255.0));
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
