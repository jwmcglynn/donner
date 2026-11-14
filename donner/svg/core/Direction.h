#pragma once
/**
 * @file Direction.h
 *
 * Defines the \ref donner::svg::Direction enum for the `direction` CSS property.
 */

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the `direction` property.
 *
 * Sets the base direction of a text paragraph and which end of a text chunk `text-anchor`
 * measures from.
 *
 * @see https://www.w3.org/TR/css-writing-modes-3/#direction
 * @see https://www.w3.org/TR/SVG2/text.html#DirectionProperty
 */
enum class Direction : uint8_t {
  Ltr,  ///< [DEFAULT] Left-to-right base direction.
  Rtl,  ///< Right-to-left base direction.
};

/**
 * Ostream output operator for \ref Direction enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, Direction value) {
  switch (value) {
    case Direction::Ltr: return os << "ltr";
    case Direction::Rtl: return os << "rtl";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
