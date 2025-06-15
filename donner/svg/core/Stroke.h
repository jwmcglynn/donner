#pragma once
/**
 * @file Stroke.h
 *
 * Defines stroke CSS property enums and types, \ref donner::svg::StrokeLinecap, \ref
 * donner::svg::StrokeLinejoin, and \ref donner::svg::StrokeDasharray.
 */

#include <cstdint>
#include <ostream>
#include <vector>

#include "donner/base/Length.h"
#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the 'stroke-linecap' property, see:
 * https://www.w3.org/TR/SVG2/painting.html#StrokeLinecapProperty
 */
enum class StrokeLinecap : uint8_t {
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
enum class StrokeLinejoin : uint8_t {
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

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

/**
 * The parsed result of the 'stroke-dasharray' property, see:
 * https://www.w3.org/TR/SVG2/painting.html#StrokeDasharrayProperty
 *
 * This is a vector of lengths, where each length represents the length of a dash or gap in the
 * stroke. The first length is the length of the first dash in the stroke, the second length is the
 * length of the first gap, the third length is the length of the second dash, and so on.
 *
 * If the number of lengths is odd, the list of lengths is repeated to yield an even number of
 * lengths.
 */
class StrokeDasharray : private std::vector<Lengthd> {
public:
  // Use private inheritance and re-export the necessary methods to avoid inheriting the
  // `operator<<`, so we can define our own ostream operator.

  using std::vector<Lengthd>::vector;

  // Iterator methods.
  using std::vector<Lengthd>::iterator;
  using std::vector<Lengthd>::const_iterator;

  using std::vector<Lengthd>::cbegin;
  using std::vector<Lengthd>::cend;
  using std::vector<Lengthd>::begin;
  using std::vector<Lengthd>::end;

  // Accessor methods.
  using std::vector<Lengthd>::at;
  using std::vector<Lengthd>::front;
  using std::vector<Lengthd>::back;

  using std::vector<Lengthd>::data;
  using std::vector<Lengthd>::size;
  using std::vector<Lengthd>::capacity;
  using std::vector<Lengthd>::operator[];
  using std::vector<Lengthd>::empty;

  // Mutator methods.
  using std::vector<Lengthd>::push_back;
  using std::vector<Lengthd>::emplace_back;
  using std::vector<Lengthd>::insert;
  using std::vector<Lengthd>::erase;
  using std::vector<Lengthd>::clear;
  using std::vector<Lengthd>::reserve;
  using std::vector<Lengthd>::resize;

  /**
   * Ostream output operator for \ref StrokeDasharray enum, outputs the CSS value.
   */
  friend std::ostream& operator<<(std::ostream& os, const StrokeDasharray& value) {
    for (size_t i = 0; i < value.size(); ++i) {
      if (i > 0) {
        os << ",";
      }
      os << value[i];
    }
    return os;
  }
};

}  // namespace donner::svg
