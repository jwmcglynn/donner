#pragma once
/**
 * @file
 * Defines the \ref donner::svg::FillRule enum used for determining how fills are painted on shapes.
 */

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the 'fill-rule' property, see:
 * https://www.w3.org/TR/SVG2/painting.html#FillRuleProperty
 */
enum class FillRule : uint8_t {
  NonZero,  ///< [DEFAULT] Determines "insideness" of a point by counting crossings of a ray drawn
            ///< from that point to infinity and path segments. If crossings is non-zero, the point
            ///< is inside, else outside.
  EvenOdd   ///< Determines "insideness" of a point by counting the number of path segments from the
            ///< shape crossed by a ray drawn from that point to infinity. If count is odd, point is
            ///< inside, else outside.
};

/**
 * Ostream output operator for \ref FillRule enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, FillRule value) {
  switch (value) {
    case FillRule::NonZero: return os << "nonzero";
    case FillRule::EvenOdd: return os << "evenodd";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
