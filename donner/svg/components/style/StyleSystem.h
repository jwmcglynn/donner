#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::components {

/**
 * Computes stylesheet information for elements, applying the CSS cascade and inheritance rules.
 *
 * @ingroup ecs_systems
 * @see https://www.w3.org/TR/SVG2/shapes.html
 */
class StyleSystem {
public:
  /**
   * Compute the style for the given entity handle, applying the CSS cascade and inheritance rules.
   *
   * @param handle Entity handle to compute the style for
   * @param warningSink Containing any warnings found
   * @returns Computed style component for the entity
   */
  const ComputedStyleComponent& computeStyle(EntityHandle handle,
                                             ParseWarningSink& warningSink);

  /**
   * Computes the style for all entities in the registry.
   *
   * @param registry Registry to compute the styles, used to query for all entities in the tree.
   * @param warningSink Containing any warnings found
   */
  void computeAllStyles(Registry& registry, ParseWarningSink& warningSink);

  /**
   * Computes the style for the given entities in the registry.
   *
   * @param registry Registry containing the entities
   * @param entities Entities to compute
   * @param warningSink Containing any warnings found
   */
  void computeStylesFor(Registry& registry, std::span<const Entity> entities,
                        ParseWarningSink& warningSink);

  /**
   * Update the style attribute on an element, merging new declarations with existing ones.
   *
   * Declarations in \p style override existing declarations with the same property name.
   * The merged result is written back to the `style` attribute and the PropertyRegistry is updated.
   *
   * @param handle Entity handle to update.
   * @param style CSS style string to merge, e.g. "fill: red; opacity: 0.5".
   */
  void updateStyle(EntityHandle handle, std::string_view style);

  /**
   * Invalidate the computed style for a given entity.
   *
   * @param handle Entity handle to invalidate
   */
  void invalidateComputed(EntityHandle handle);

  /**
   * Invalidate the full style and reparse attributes.
   *
   * @param handle Entity handle to invalidate
   */
  void invalidateAll(EntityHandle handle);

private:
  void computePropertiesInto(EntityHandle handle, ComputedStyleComponent& computedStyle,
                             ParseWarningSink& warningSink);
};

}  // namespace donner::svg::components
