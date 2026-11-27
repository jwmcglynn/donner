#pragma once
/// @file

#include <array>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <tuple>
#include <vector>

#include "donner/base/EcsRegistry.h"
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
  /// Independent snapshot of the computed values, shared among entries captured from the same
  /// record.
  std::shared_ptr<const PropertyRegistry> properties;
  /// Final stop and flood opacities.
  std::optional<ComputedResourceOpacities> resourceOpacities;
  /// Whether fill is inherited from an external-use host.
  bool externalFill = false;
  /// Whether stroke is inherited from an external-use host.
  bool externalStroke = false;

  /// Exact equality including every property payload and resource-opacity priority.
  bool operator==(const PositionedStyle& other) const;

  /// Print the position and the whole computed style, for test failure output.
  friend std::ostream& operator<<(std::ostream& os, const PositionedStyle& value);
};

/// An entity reachable from a document's root, with its position as in \ref PositionedStyle.
struct PositionedEntity {
  std::string position;  //!< See \ref PositionedStyle::position.
  Entity entity;         //!< The entity.
};

/**
 * Every entity reachable from the root of \p document through the document tree and the shadow
 * trees, in the order and with the positions \ref PositionedStyles uses.
 *
 * @param document Document to walk.
 */
std::vector<PositionedEntity> PositionedEntities(SVGDocument& document);

/**
 * Computed style of every entity reachable from the root of \p document through the document tree
 * and the shadow trees, in a traversal order that two frames of equal documents share, so styles
 * can be compared entity by entity across frames and across documents.
 *
 * @param document Document whose computed styles are read; nothing is computed.
 */
std::vector<PositionedStyle> PositionedStyles(SVGDocument& document);

/// Ordered rendering data with entity references normalized to document tree positions.
using PositionedRenderInstance =
    std::tuple<std::string, int, bool, bool, std::string, Transform2d, std::array<std::string, 8>>;

/// Capture each render instance and its paint, clip, mask, filter and marker targets.
std::vector<PositionedRenderInstance> PositionedRenderInstances(SVGDocument& document);

}  // namespace donner::svg::components
