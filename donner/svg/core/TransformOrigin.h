#pragma once
/// @file

#include <ostream>

#include "donner/base/Length.h"
#include "donner/base/Vector2.h"

namespace donner::svg {

/**
 * Represents the `transform-origin` property value.
 */
struct TransformOrigin {
  Lengthd x;  ///< X coordinate.
  Lengthd y;  ///< Y coordinate.
};

inline std::ostream& operator<<(std::ostream& os, const TransformOrigin& origin) {
  return os << origin.x << ' ' << origin.y;
}

}  // namespace donner::svg
