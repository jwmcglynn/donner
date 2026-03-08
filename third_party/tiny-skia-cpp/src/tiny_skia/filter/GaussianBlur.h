#pragma once

/// @file GaussianBlur.h
/// @brief Gaussian blur filter operation on Pixmap buffers.

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace tiny_skia::filter {

/// Edge handling mode for Gaussian blur.
enum class BlurEdgeMode {
  None,       ///< Treat out-of-bounds pixels as transparent black.
  Duplicate,  ///< Clamp to nearest edge pixel.
  Wrap,       ///< Wrap around (modular arithmetic).
};

/// Applies a Gaussian blur to the given pixmap in-place.
///
/// Uses a discrete Gaussian kernel for sigma < 2.0, and a 3-pass box blur approximation
/// (matching Skia's behavior) for larger sigma values.
///
/// @param pixmap Pixmap to blur (modified in-place).
/// @param sigmaX Standard deviation of the blur in the X direction. 0 = no horizontal blur.
/// @param sigmaY Standard deviation of the blur in the Y direction. 0 = no vertical blur.
/// @param edgeMode Edge handling mode. Default is None (transparent black).
void gaussianBlur(Pixmap& pixmap, double sigmaX, double sigmaY,
                  BlurEdgeMode edgeMode = BlurEdgeMode::None);

/// Float-precision version of gaussianBlur.
void gaussianBlur(FloatPixmap& pixmap, double sigmaX, double sigmaY,
                  BlurEdgeMode edgeMode = BlurEdgeMode::None);

}  // namespace tiny_skia::filter
