#pragma once
/// @file

#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * Values for the `"clipPathUnits"` attribute,
 * https://drafts.fxtf.org/css-masking-1/#element-attrdef-clippath-clippathunits.
 *
 * This is used on the \ref xml_clipPath element, and defines the coordinate system for the contents
 * of the clip path.
 */
enum class ClipPathUnits : uint8_t {
  /**
   * The clip path is defined in user space, which is the coordinate system of the element that
   * references the clip path.
   */
  UserSpaceOnUse,
  /**
   * The clip path is defined in object space, where (0, 0) is the top-left corner of the element
   * that references the clip path, and (1, 1) is the bottom-right corner. Note that this may result
   * in non-uniform scaling if the element is not square.
   */
  ObjectBoundingBox,
  /**
   * The default value for the `"clipPathUnits"` attribute, which is `userSpaceOnUse`.
   */
  Default = UserSpaceOnUse,
};

/// Ostream output operator for \ref ClipPathUnits enum, outputs the enum name with prefix, e.g.
/// `ClipPathUnits::UserSpaceOnUse`.
inline std::ostream& operator<<(std::ostream& os, ClipPathUnits units) {
  switch (units) {
    case ClipPathUnits::UserSpaceOnUse: return os << "userSpaceOnUse";
    case ClipPathUnits::ObjectBoundingBox: return os << "objectBoundingBox";
  }

  UTILS_UNREACHABLE();
}

}  // namespace donner::svg
