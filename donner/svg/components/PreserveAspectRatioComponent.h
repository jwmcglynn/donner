#pragma once
/// @file

#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg::components {

/**
 * Stores the `preserveAspectRatio` attribute of \ref xml_svg and \ref xml_pattern.
 */
struct PreserveAspectRatioComponent {
  /// The preserveAspectRatio of the element, defaults to \ref PreserveAspectRatio::None().
  PreserveAspectRatio preserveAspectRatio;
};

}  // namespace donner::svg::components
