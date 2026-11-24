#pragma once
/// @file

#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg::components {

/// One entity's computed style, keyed by where the entity sits in the document and its shadow
/// trees.
struct PositionedStyle {
  /// Tag name and child index of each step from the root, with `~branch N` for the root of the
  /// Nth offscreen shadow tree of an entity.
  std::string position;
  /// Computed properties, or nullopt when the entity has no computed style.
  std::optional<PropertyRegistry> properties;
  /// Final stop and flood opacities.
  std::optional<ComputedResourceOpacities> resourceOpacities;
  /// Whether fill is inherited from an external-use host.
  bool externalFill = false;
  /// Whether stroke is inherited from an external-use host.
  bool externalStroke = false;

  /// Equality operator.
  bool operator==(const PositionedStyle&) const = default;

  /// Print the position and the whole computed style, for test failure output.
  friend std::ostream& operator<<(std::ostream& os, const PositionedStyle& value);
};

/**
 * Computed style of every entity reachable from the root of \p document through the document tree
 * and the shadow trees, in a traversal order that two frames of equal documents share, so styles
 * can be compared entity by entity across frames and across documents.
 *
 * @param document Document whose computed styles are read; nothing is computed.
 */
std::vector<PositionedStyle> PositionedStyles(SVGDocument& document);

}  // namespace donner::svg::components
