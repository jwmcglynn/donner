#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/svg/core/MaskUnits.h"

namespace donner::svg::components {

/**
 * Parameters for the \ref xml_mask element.
 *
 * Contains the mask bounds, which determines how large the canvas needs to be when applying the
 * filter, and mask configuration such as units for its parameters.
 */
struct MaskComponent {
  /// The x-coordinate of the mask, defaults to -10% (outside the element itself).
  std::optional<Lengthd> x;
  /// The y-coordinate of the mask, defaults to -10% (outside the element itself).
  std::optional<Lengthd> y;
  /// Width of the mask, defaults to 120% (outside of the element itself).
  std::optional<Lengthd> width;
  /// Height of the mask, defaults to 120% (outside of the element itself).
  std::optional<Lengthd> height;

  /// The parsed value of the "maskUnits" attribute, which defines the coordinate system for the
  /// `x`, `y`, `width`, and `height` attributes of the mask.
  MaskUnits maskUnits = MaskUnits::Default;

  /// The parsed value of the "maskContentUnits" attribute, which defines the coordinate system for
  /// the content of the mask.
  MaskContentUnits maskContentUnits = MaskContentUnits::Default;

  /// Returns true if the mask should use the default bounds.
  bool useAutoBounds() const {
    return !x.has_value() && !y.has_value() && !width.has_value() && !height.has_value();
  }
};

}  // namespace donner::svg::components
