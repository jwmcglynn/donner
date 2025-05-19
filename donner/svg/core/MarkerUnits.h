#pragma once
/// @file

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * Values for the `"markerUnits"` attribute,
 * https://www.w3.org/TR/SVG2/painting.html#MarkerUnitsAttribute.
 *
 * This is used on the \ref xml_marker element, and defines the coordinate system for `markerWidth`,
 * `markerHeight`, and the contents of the marker.
 */
enum class MarkerUnits : uint8_t {
  /**
   * Marker scales to the stroke width. Defines a coordinate system where 1.0 is scaled to the
   * stroke-width of the shape.
   */
  StrokeWidth,
  /**
   * The marker contents are defined in user space, which is the coordinate system of the element
   * that references the marker.
   */
  UserSpaceOnUse,
  /**
   * The default value for the `"markerUnits"` attribute, which is `strokeWidth`.
   */
  Default = StrokeWidth,
};

/// Ostream output operator for \ref MarkerUnits enum, outputs the enum name with prefix, e.g.
/// `MarkerUnits::UserSpaceOnUse`.
inline std::ostream& operator<<(std::ostream& os, MarkerUnits units) {
  switch (units) {
    case MarkerUnits::StrokeWidth: return os << "MarkerUnits::StrokeWidth";
    case MarkerUnits::UserSpaceOnUse: return os << "MarkerUnits::UserSpaceOnUse";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
