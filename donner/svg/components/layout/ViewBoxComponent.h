#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"

namespace donner::svg::components {

/**
 * A component attached to entities that have a `viewBox` attribute, such as \ref xml_svg and \ref
 * xml_pattern.
 */
struct ViewBoxComponent {
  /// Stored viewBox, if any.
  std::optional<Boxd> viewBox;
};

/**
 * Computed value of a viewBox for the current element. If this element does not define a viewBox,
 * this is the viewBox of the nearest ancestor or the document itself.
 */
struct ComputedViewBoxComponent {
  Boxd viewBox;  //!< The viewBox of the element, or the viewBox of the nearest ancestor.
};

}  // namespace donner::svg::components
