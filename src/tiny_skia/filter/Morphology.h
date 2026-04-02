#pragma once

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace tiny_skia::filter {

/// Morphology operator.
enum class MorphologyOp {
  Erode,   ///< Per-channel minimum in the window.
  Dilate,  ///< Per-channel maximum in the window.
};

/// Applies a morphology filter (erode or dilate) to the source pixmap and writes to the
/// destination.
///
/// For each output pixel, the filter computes the per-channel minimum (erode) or maximum (dilate)
/// over a rectangular window of size (2*radiusX+1) x (2*radiusY+1) centered on the pixel.
///
/// If either radius is zero or negative, the source is copied directly to the destination.
///
/// @param src Source pixmap (premultiplied RGBA).
/// @param dst Destination pixmap (must be same size as src, cleared to transparent first).
/// @param op Erode or dilate.
/// @param radiusX Horizontal radius in pixels.
/// @param radiusY Vertical radius in pixels.
void morphology(const Pixmap& src, Pixmap& dst, MorphologyOp op, int radiusX, int radiusY);

/// Float-precision version of morphology.
void morphology(const FloatPixmap& src, FloatPixmap& dst, MorphologyOp op, int radiusX,
                int radiusY);

}  // namespace tiny_skia::filter
