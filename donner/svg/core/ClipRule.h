#pragma once

#include <ostream>

/**
 * @file
 * Defines the ClipRule enum used for determining how paths are clipped.
 */

namespace donner::svg::core {

/**
 * Enum for the clip-rule property, which indicates the algorithm used to determine what parts of
 * the canvas are included inside the shape.
 */
enum class ClipRule {
  NonZero,  ///< Nonzero rule.
  EvenOdd   ///< Even-odd rule.
};

/**
 * Output stream operator for ClipRule enum.
 * 
 * @param os The output stream.
 * @param clipRule The ClipRule value to output.
 * @return The output stream.
 */
std::ostream& operator<<(std::ostream& os, const ClipRule& clipRule) {
  switch (clipRule) {
    case ClipRule::NonZero:
      os << "nonzero";
      break;
    case ClipRule::EvenOdd:
      os << "evenodd";
      break;
  }
  return os;
}

}  // namespace donner::svg::core
