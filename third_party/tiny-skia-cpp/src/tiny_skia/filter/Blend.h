#pragma once

#include <cstdint>

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace tiny_skia::filter {

/// Blend mode for feBlend.
enum class BlendMode : std::uint8_t {
  Normal,
  Multiply,
  Screen,
  Darken,
  Lighten,
  Overlay,
  ColorDodge,
  ColorBurn,
  HardLight,
  SoftLight,
  Difference,
  Exclusion,
  Hue,
  Saturation,
  Color,
  Luminosity,
};

/// Blends two pixmaps using the specified CSS blend mode.
///
/// The foreground (`fg`, corresponding to SVG `in`) is blended over the background (`bg`,
/// corresponding to SVG `in2`).
///
/// Operates on premultiplied RGBA values. The blend formula follows the CSS compositing spec:
/// Co = (1-Ab)*Cs + (1-As)*Cb + As*Ab*B(Cb,Cs) where B is the blend function.
///
/// @param bg Background pixmap (in2).
/// @param fg Foreground pixmap (in).
/// @param dst Output pixmap (must be same dimensions as bg/fg).
/// @param mode Blend mode.
void blend(const Pixmap& bg, const Pixmap& fg, Pixmap& dst, BlendMode mode);

/// Float-precision version of blend.
void blend(const FloatPixmap& bg, const FloatPixmap& fg, FloatPixmap& dst, BlendMode mode);

}  // namespace tiny_skia::filter
