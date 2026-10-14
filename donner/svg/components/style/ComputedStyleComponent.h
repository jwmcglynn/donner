#pragma once
/// @file

#include <optional>

#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg::components {

/// Final resource opacities for this style instance, including explicit parent inheritance.
struct ComputedResourceOpacities {
  double stopOpacity = 1;             ///< Final gradient stop opacity.
  double floodOpacity = 1;            ///< Final filter flood opacity.
  css::Specificity stopSpecificity;   ///< Winning stop-opacity cascade priority.
  css::Specificity floodSpecificity;  ///< Winning flood-opacity cascade priority.
};

/**
 * Contains the computed style properties for an element, which is a combination of the `style=""`
 * attribute, the CSS stylesheet, and the CSS cascade where properties are inherited from the
 * parent.
 */
struct ComputedStyleComponent {
  /// The computed style properties. \c std::nullopt may be present mid-computation before all
  /// properties have been cascaded.
  std::optional<PropertyRegistry> properties;
  /// Absent for manually constructed or incomplete styles; populated with the final properties.
  std::optional<ComputedResourceOpacities> resourceOpacities;
};

}  // namespace donner::svg::components
