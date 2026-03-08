#pragma once

/// @file GaussianBlur.h
/// @brief Gaussian blur filter operation on Pixmap buffers.

#include "tiny_skia/Pixmap.h"

namespace tiny_skia::filter {

/// Applies a Gaussian blur to the given pixmap in-place.
///
/// Uses a discrete Gaussian kernel for sigma < 2.0, and a 3-pass box blur approximation
/// (matching Skia's behavior) for larger sigma values.
///
/// @param pixmap Pixmap to blur (modified in-place).
/// @param sigmaX Standard deviation of the blur in the X direction. 0 = no horizontal blur.
/// @param sigmaY Standard deviation of the blur in the Y direction. 0 = no vertical blur.
void gaussianBlur(Pixmap& pixmap, double sigmaX, double sigmaY);

}  // namespace tiny_skia::filter
