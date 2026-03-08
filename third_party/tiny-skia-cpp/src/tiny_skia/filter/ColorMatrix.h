#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/Pixmap.h"

namespace tiny_skia::filter {

/// Applies a 5x4 color matrix to each pixel of the pixmap.
///
/// The matrix transforms [R, G, B, A, 1] -> [R', G', B', A'] where values are in [0, 255].
/// Operates on unpremultiplied values: unpremultiplies input, applies matrix, clamps to [0, 255],
/// re-premultiplies.
///
/// The matrix is stored in row-major order: [r0 r1 r2 r3 r4  g0 g1 g2 g3 g4  b0 b1 b2 b3 b4  a0
/// a1 a2 a3 a4] where r4, g4, b4, a4 are the translation components (scaled to 0-255 range).
///
/// @param pixmap Pixmap to transform (in-place).
/// @param matrix 20-element color matrix in row-major order.
void colorMatrix(Pixmap& pixmap, const std::array<double, 20>& matrix);

/// Build a saturate color matrix (ITU-R BT.709 luminance weights).
/// @param s Saturation coefficient. 0 = grayscale, 1 = identity.
std::array<double, 20> saturateMatrix(double s);

/// Build a hueRotate color matrix.
/// @param angleDeg Rotation angle in degrees.
std::array<double, 20> hueRotateMatrix(double angleDeg);

/// Build a luminanceToAlpha color matrix.
/// Sets A' = 0.2126*R + 0.7152*G + 0.0722*B, R'=G'=B'=0.
std::array<double, 20> luminanceToAlphaMatrix();

/// Returns the 5x4 identity color matrix (no-op).
std::array<double, 20> identityMatrix();

}  // namespace tiny_skia::filter
