#pragma once

#include "tiny_skia/Geom.h"

namespace tiny_skia::pathRectRs {

// Wrappers for `third_party/tiny-skia/path/src/rect.rs`.

using Rect = ::tiny_skia::Rect;
using IntRect = ::tiny_skia::IntRect;
using ScreenIntRect = ::tiny_skia::ScreenIntRect;

inline std::optional<Rect> fromLTRB(float left, float top, float right, float bottom) {
  return Rect::fromLTRB(left, top, right, bottom);
}

inline std::optional<Rect> fromXYWH(float x, float y, float w, float h) {
  return Rect::fromXYWH(x, y, w, h);
}

inline std::optional<IntRect> round(const Rect& rect) { return rect.round(); }

inline std::optional<IntRect> roundOut(const Rect& rect) { return rect.roundOut(); }

inline std::optional<ScreenIntRect> intRectToScreen(const IntRect& rect) {
  return ::tiny_skia::intRectToScreen(rect);
}

}  // namespace tiny_skia::pathRectRs
