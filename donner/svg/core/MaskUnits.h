#pragma once
/// @file

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * Values for the `"maskUnits"` attribute,
 * https://drafts.fxtf.org/css-masking-1/#element-attrdef-mask-maskunits.
 *
 * This is used on the \ref xml_mask element, and defines the coordinate system for the `x`, `y`,
 * `width`, and `height` attributes of the mask.
 */
enum class MaskUnits : uint8_t {
  /**
   * The attributes are defined in user space, which is the coordinate system of the element that
   * references the mask.
   */
  UserSpaceOnUse,
  /**
   * The attributes are defined in object space, where (0, 0) is the top-left corner of the element
   * that references the mask, and (1, 1) is the bottom-right corner.
   */
  ObjectBoundingBox,
  /**
   * The default value for the `"maskUnits"` attribute, which is `objectBoundingBox`.
   */
  Default = ObjectBoundingBox,
};

/// Ostream output operator for \ref MaskUnits enum, outputs the enum name with prefix, e.g.
/// `MaskUnits::UserSpaceOnUse`.
inline std::ostream& operator<<(std::ostream& os, MaskUnits units) {
  switch (units) {
    case MaskUnits::UserSpaceOnUse: return os << "MaskUnits::UserSpaceOnUse";
    case MaskUnits::ObjectBoundingBox: return os << "MaskUnits::ObjectBoundingBox";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

/**
 * Values for the `"maskContentUnits"` attribute,
 * https://drafts.fxtf.org/css-masking-1/#element-attrdef-mask-maskcontentunits.
 *
 * This is used on the \ref xml_mask element, and defines the coordinate system for the contents of
 * the mask.
 */
enum class MaskContentUnits : uint8_t {
  /**
   * The mask contents are defined in user space, which is the coordinate system of the element that
   * references the mask.
   */
  UserSpaceOnUse,
  /**
   * The mask contents are defined in object space, where (0, 0) is the top-left corner of the
   * element that references the mask, and (1, 1) is the bottom-right corner. Note that this may
   * result in non-uniform scaling if the element is not square.
   */
  ObjectBoundingBox,
  /**
   * The default value for the `"maskContentUnits"` attribute, which is `userSpaceOnUse`.
   */
  Default = UserSpaceOnUse,
};

/// Ostream output operator for \ref MaskContentUnits enum, outputs the enum name with prefix, e.g.
/// `MaskContentUnits::UserSpaceOnUse`.
inline std::ostream& operator<<(std::ostream& os, MaskContentUnits units) {
  switch (units) {
    case MaskContentUnits::UserSpaceOnUse: return os << "MaskContentUnits::UserSpaceOnUse";
    case MaskContentUnits::ObjectBoundingBox: return os << "MaskContentUnits::ObjectBoundingBox";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
