#pragma once
/**
 * @file
 * Defines the ClipRule enum used for determining how paths are clipped with \ref xml_clipPath.
 */

#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the `clip-rule` property, see:
 * https://drafts.fxtf.org/css-masking-1/#propdef-clip-rule
 */
enum class ClipRule : uint8_t {
  NonZero,  ///< [DEFAULT] Determines "insideness" of a point by counting crossings of a ray drawn
            ///< from that point to infinity and path segments. If crossings is non-zero, the point
            ///< is inside, else outside.
  EvenOdd   ///< Determines "insideness" of a point by counting the number of path segments from the
            ///< shape crossed by a ray drawn from that point to infinity. If count is odd, point is
            ///< inside, else outside.
};

/**
 * Output stream operator for ClipRule enum.
 *
 * @param os The output stream.
 * @param clipRule The ClipRule value to output.
 * @return The output stream.
 */
inline std::ostream& operator<<(std::ostream& os, const ClipRule& clipRule) {
  switch (clipRule) {
    case ClipRule::NonZero: return os << "nonzero"; break;
    case ClipRule::EvenOdd: return os << "evenodd"; break;
  }

  UTILS_UNREACHABLE();
}

}  // namespace donner::svg
