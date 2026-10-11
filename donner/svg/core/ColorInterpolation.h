#pragma once
/// @file

#include <cstdint>
#include <ostream>

namespace donner::svg {

/// Values of the inherited `color-interpolation` property. Luminance masks use this color space.
enum class ColorInterpolation : std::uint8_t {
  /// Let the renderer choose its color space; luminance masks use sRGB.
  Auto,
  /// Use sRGB color values.
  SRGB,
  /// Convert color values to linear RGB.
  LinearRGB,
  /// Initial color space.
  Default = SRGB,
};

/// Prints a color interpolation value.
/// @param os Output stream.
/// @param value Value to print.
/// @return The output stream.
inline std::ostream& operator<<(std::ostream& os, ColorInterpolation value) {
  switch (value) {
    case ColorInterpolation::Auto: return os << "auto";
    case ColorInterpolation::SRGB: return os << "sRGB";
    case ColorInterpolation::LinearRGB: return os << "linearRGB";
  }
  return os << "ColorInterpolation(" << static_cast<int>(value) << ")";
}

}  // namespace donner::svg
