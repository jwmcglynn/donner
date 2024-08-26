#pragma once
/**
 * @file
 * Defines stroke CSS property enums and types, \ref StrokeLinecap, \ref StrokeLinejoin, and \ref
 * StrokeDasharray.
 */

#include <ostream>
#include <vector>

#include "donner/base/Length.h"
#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the 'stroke-linecap' property, see:
 * https://www.w3.org/TR/SVG2/painting.html#StrokeLinecapProperty
 */
enum class StrokeLinecap {
  Butt,   ///< [DEFAULT] The stroke is squared off at the endpoint of the path.
  Round,  ///< The stroke is rounded at the endpoint of the path.
  Square  ///< The stroke extends beyond the endpoint of the path by half of the stroke width and
          ///< is squared off.
};

/**
 * Ostream output operator for \ref StrokeLinecap enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, StrokeLinecap value) {
  switch (value) {
    case StrokeLinecap::Butt: return os << "butt";
    case StrokeLinecap::Round: return os << "round";
    case StrokeLinecap::Square: return os << "square";
  }

  UTILS_UNREACHABLE();
}

/**
 * The parsed result of the 'stroke-linejoin' property, see:
 * https://www.w3.org/TR/SVG2/painting.html#StrokeLinejoinProperty
 */
enum class StrokeLinejoin {
  Miter,      ///< [DEFAULT] The outer edges of the strokes for the two segments are extended until
              ///< they meet at an angle, creating a sharp point.
  MiterClip,  ///< Same as miter except the stroke will be clipped if the miter limit is exceeded.
  Round,      ///< The corners of the stroke are rounded off using an arc of a circle with a radius
              ///< equal to the half of the stroke width.
  Bevel,      ///< A triangular shape is used to fill the area between the two stroked segments.
  Arcs  ///< Similar to miter join, but uses an elliptical arc to join the segments, creating a
        ///< smoother joint than miter join when the angle is acute. It is only used for large
        ///< angles where a miter join would be too sharp.
};

/**
 * Ostream output operator for \ref StrokeLinejoin enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, StrokeLinejoin value) {
  switch (value) {
    case StrokeLinejoin::Miter: return os << "miter";
    case StrokeLinejoin::MiterClip: return os << "miter-clip";
    case StrokeLinejoin::Round: return os << "round";
    case StrokeLinejoin::Bevel: return os << "bevel";
    case StrokeLinejoin::Arcs: return os << "arcs";
  }

  UTILS_UNREACHABLE();
}

/**
 * The parsed result of the 'stroke-dasharray' property, see:
 * https://www.w3.org/TR/SVG2/painting.html#StrokeDasharrayProperty
 */
using StrokeDasharray = std::vector<Lengthd>;

/**
 * Ostream output operator for \ref StrokeDasharray enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, const StrokeDasharray& value) {
  for (size_t i = 0; i < value.size(); ++i) {
    if (i > 0) {
      os << ",";
    }
    os << value[i];
  }
  return os;
}

}  // namespace donner::svg
