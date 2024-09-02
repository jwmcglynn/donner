#pragma once
/// @file

#include <optional>

#include "donner/svg/core/MaskUnits.h"

namespace donner::svg::components {

/**
 * Parameters for the \ref xml_mask element.
 */
struct MaskComponent {
  /// The parsed value of the "maskUnits" attribute, which defines the coordinate system for the
  /// `x`, `y`, `width`, and `height` attributes of the mask.
  std::optional<MaskUnits> maskUnits;

  /// The parsed value of the "maskContentUnits" attribute, which defines the coordinate system for
  /// the content of the mask.
  std::optional<MaskContentUnits> maskContentUnits;
};

}  // namespace donner::svg::components
