#pragma once

/// @file Composite.h
/// @brief Porter-Duff compositing and arithmetic combination of two pixmaps.

#include <cstdint>

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace tiny_skia::filter {

/// Porter-Duff compositing operator.
enum class CompositeOp : std::uint8_t {
  Over,
  In,
  Out,
  Atop,
  Xor,
  Lighter,
  Arithmetic,
};

/// Composites two premultiplied RGBA pixmaps using the specified operator.
///
/// For the arithmetic operator, the formula is:
///   result = k1*in1*in2 + k2*in1 + k3*in2 + k4  (per channel, clamped to [0, 255])
///
/// @param in1 First input (source / `in`).
/// @param in2 Second input (destination / `in2`).
/// @param dst Output pixmap (must be same dimensions).
/// @param op Compositing operator.
/// @param k1 Arithmetic coefficient (only used when op == Arithmetic).
/// @param k2 Arithmetic coefficient.
/// @param k3 Arithmetic coefficient.
/// @param k4 Arithmetic coefficient.
void composite(const Pixmap& in1, const Pixmap& in2, Pixmap& dst, CompositeOp op,
               double k1 = 0.0, double k2 = 0.0, double k3 = 0.0, double k4 = 0.0);

/// Float-precision version of composite.
void composite(const FloatPixmap& in1, const FloatPixmap& in2, FloatPixmap& dst, CompositeOp op,
               double k1 = 0.0, double k2 = 0.0, double k3 = 0.0, double k4 = 0.0);

}  // namespace tiny_skia::filter
