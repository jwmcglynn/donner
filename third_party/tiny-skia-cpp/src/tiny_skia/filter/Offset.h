#pragma once

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace tiny_skia::filter {

/// Copies pixels from src to dst, shifted by (dx, dy) in pixel coordinates.
/// Pixels that shift outside the buffer are discarded; uncovered pixels become transparent black.
///
/// @param src Source pixmap.
/// @param dst Destination pixmap (must be same dimensions as src). Cleared to transparent first.
/// @param dx Horizontal offset in pixels.
/// @param dy Vertical offset in pixels.
void offset(const Pixmap& src, Pixmap& dst, int dx, int dy);

/// Float-precision version of offset.
void offset(const FloatPixmap& src, FloatPixmap& dst, int dx, int dy);

}  // namespace tiny_skia::filter
