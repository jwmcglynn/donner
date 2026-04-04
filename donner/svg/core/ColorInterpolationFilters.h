#pragma once
/// @file

#include <cstdint>
#include <ostream>

namespace donner::svg {

/**
 * Values for the `"color-interpolation-filters"` property which specifies the color space for
 * filter operations.
 */
enum class ColorInterpolationFilters : std::uint8_t {
  SRGB,                //!< Operations are performed in sRGB.
  LinearRGB,           //!< Operations are performed in linearRGB.
  Default = LinearRGB  //!< SVG default.
};

/// Ostream output operator for \ref ColorInterpolationFilters.
inline std::ostream& operator<<(std::ostream& os, ColorInterpolationFilters cif) {
  switch (cif) {
    case ColorInterpolationFilters::SRGB: return os << "sRGB";
    case ColorInterpolationFilters::LinearRGB: return os << "linearRGB";
  }
  return os << "ColorInterpolationFilters(" << static_cast<int>(cif) << ")";
}

}  // namespace donner::svg
