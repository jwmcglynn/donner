#pragma once
/// @file

#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg::components {

/**
 * Contains the computed style properties for an element, which is a combination of the `style=""`
 * attribute, the CSS stylesheet, and the CSS cascade where properties are inherited from the
 * parent.
 */
struct ComputedStyleComponent {
  /// The computed style properties. \c std::nullopt may be present mid-computation before all
  /// properties have been cascaded.
  std::optional<PropertyRegistry> properties;
};

}  // namespace donner::svg::components
