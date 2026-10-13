#pragma once

#include <span>

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace tiny_skia::filter {

/// Edge mode for feConvolveMatrix out-of-bounds pixel access.
enum class ConvolveEdgeMode {
  Duplicate,  ///< Clamp to nearest edge pixel.
  Wrap,       ///< Wrap around (modular arithmetic).
  None,       ///< Treat as transparent black (0,0,0,0).
};

/// Parameters for the convolve matrix filter.
struct ConvolveParams {
  int orderX = 3;                  ///< Kernel width.
  int orderY = 3;                  ///< Kernel height.
  std::span<const double> kernel;  ///< Kernel values (orderX * orderY, row-major).
  double divisor = 1.0;            ///< Divides the weighted sum.
  double bias = 0.0;               ///< Offset added after division.
  int targetX = 1;                 ///< X offset of kernel center.
  int targetY = 1;                 ///< Y offset of kernel center.
  ConvolveEdgeMode edgeMode = ConvolveEdgeMode::Duplicate;  ///< Edge handling.
  bool preserveAlpha = false;  ///< If true, alpha passes through unchanged.
};

/// Applies a 2D convolution matrix to a premultiplied RGBA pixmap.
///
/// With preserveAlpha=false, convolution uses premultiplied channels. Alpha receives the
/// unscaled bias and is clamped to [0, 1]; RGB receives bias times that resulting alpha and
/// is clamped to [0, alpha]. The kernel is rotated 180 degrees before sampling.
///
/// With preserveAlpha=true, convolution uses unpremultiplied RGB with unscaled bias, then
/// clamps and premultiplies by the unchanged source alpha.
///
/// @param src Source pixmap (premultiplied RGBA).
/// @param dst Destination pixmap (must be same size as src).
/// @param params Convolution parameters.
void convolveMatrix(const Pixmap& src, Pixmap& dst, const ConvolveParams& params);

/// Float-precision version of convolveMatrix.
void convolveMatrix(const FloatPixmap& src, FloatPixmap& dst, const ConvolveParams& params);

}  // namespace tiny_skia::filter
