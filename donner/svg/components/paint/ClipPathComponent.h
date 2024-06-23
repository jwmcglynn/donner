#pragma once
/// @file

#include <optional>

#include "donner/svg/core/ClipPathUnits.h"

namespace donner::svg::components {

/**
 * Parameters for the \ref xml_clipPath element.
 */
struct ClipPathComponent {
  /// The parsed value of the "clipPathUnits" attribute, which defines the coordinate system for the
  /// contents of the clip path.
  std::optional<ClipPathUnits> clipPathUnits;
};

}  // namespace donner::svg::components
