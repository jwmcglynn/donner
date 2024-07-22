#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg::components {

/**
 * A component attached to entities that have a `viewbox` attribute, such as \ref xml_svg and \ref
 * xml_pattern.
 */
struct ViewboxComponent {
  /// Stored viewbox, if any.
  std::optional<Boxd> viewbox;
};

/**
 * Computed value of a viewbox for the current element. If this element does not define a viewbox,
 * this is the viewbox of the nearest ancestor or the document itself.
 */
struct ComputedViewboxComponent {
  Boxd viewbox;  //!< The viewbox of the element, or the viewbox of the nearest ancestor.
};

}  // namespace donner::svg::components
