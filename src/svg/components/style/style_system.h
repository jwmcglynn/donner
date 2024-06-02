#pragma once
/// @file

#include "src/svg/components/style/computed_style_component.h"
#include "src/svg/registry/registry.h"

namespace donner::svg::components {

/**
 * Computes stylesheet information for elements, applying the CSS cascade and inheritance rules.
 *
 * @ingroup ecs_systems
 * @see https://www.w3.org/TR/SVG2/shapes.html
 */
class StyleSystem {
public:
  const ComputedStyleComponent& computeStyle(EntityHandle handle);

  void applyStyleToLayout(EntityHandle handle);

  void computeAllStyles(Registry& registry);

  void computeStylesFor(Registry& registry, std::span<const Entity> entities);

private:
  void computePropertiesInto(EntityHandle handle, ComputedStyleComponent& computedStyle);
};

}  // namespace donner::svg::components
