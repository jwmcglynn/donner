#pragma once

#include "tiny_skia/Geom.h"

namespace tiny_skia::pathSizeRs {

// Wrappers for `third_party/tiny-skia/path/src/size.rs`.

using IntSize = ::tiny_skia::IntSize;
using IntRect = ::tiny_skia::IntRect;
using Rect = ::tiny_skia::Rect;
using ScreenIntRect = ::tiny_skia::ScreenIntRect;

inline std::optional<IntSize> fromWH(LengthU32 width, LengthU32 height) {
  return IntSize::fromWH(width, height);
}

inline ScreenIntRect toScreenIntRect(const IntSize& size, std::uint32_t x, std::uint32_t y) {
  return size.toScreenIntRect(x, y);
}

inline std::optional<IntRect> toIntRect(const IntSize& size, std::int32_t x, std::int32_t y) {
  return size.toIntRect(x, y);
}

inline Rect toRect(const IntSize& size) { return size.toRect(); }

}  // namespace tiny_skia::pathSizeRs
