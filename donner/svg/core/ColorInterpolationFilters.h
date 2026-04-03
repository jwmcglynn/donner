#pragma once
/// @file

#include <cstdint>
#include <ostream>

namespace donner::svg {

/**
 * Values for the `"color-interpolation-filters"` property which specifies the color space for
 * filter operations.
 *
 * @see https://www.w3.org/TR/filter-effects/#element-attrdef-filter-color-interpolation-filters
 */
enum class ColorInterpolationFilters : std::uint8_t {
  /// Operations are performed in the sRGB color space.
  SRGB,
  /// Operations are performed in the linearRGB color space.
  LinearRGB,
  /// The default value, which is `linearRGB`.
  Default = LinearRGB,
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
