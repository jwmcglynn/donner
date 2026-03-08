#pragma once

#include "tiny_skia/Pixmap.h"

namespace tiny_skia::filter {

/// Convert each pixel from sRGB to linear RGB (inverse gamma).
///
/// Uses the standard sRGB transfer function:
///   C_linear = C_srgb <= 0.04045 ? C_srgb/12.92 : pow((C_srgb+0.055)/1.055, 2.4)
///
/// Operates on premultiplied data: unpremultiplies, converts, re-premultiplies.
///
/// @param pixmap Pixmap to convert in-place.
void srgbToLinear(Pixmap& pixmap);

/// Convert each pixel from linear RGB to sRGB (apply gamma).
///
/// Uses the standard sRGB transfer function:
///   C_srgb = C_linear <= 0.0031308 ? 12.92*C_linear : 1.055*pow(C_linear, 1/2.4)-0.055
///
/// Operates on premultiplied data: unpremultiplies, converts, re-premultiplies.
///
/// @param pixmap Pixmap to convert in-place.
void linearToSrgb(Pixmap& pixmap);

}  // namespace tiny_skia::filter
