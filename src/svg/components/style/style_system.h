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
  /**
   * Compute the style for the given entity handle, applying the CSS cascade and inheritance rules.
   *
   * @param handle Entity handle to compute the style for
   * @param outWarnings Containing any warnings found
   * @returns Computed style component for the entity
   */
  const ComputedStyleComponent& computeStyle(EntityHandle handle,
                                             std::vector<ParseError>* outWarnings);

  /**
   * Updates \ref LayoutSystem with computed style information, creating \ref
   * ComputedSizedElementComponent for the given entity.
   *
   * @param handle Entity handle to compute the style for
   * @param outWarnings Containing any warnings found
   */
  void applyStyleToLayout(EntityHandle handle, std::vector<ParseError>* outWarnings);

  /**
   * Computes the style for all entities in the registry.
   *
   * @param registry Registry to compute the styles, used to query for all entities in the tree.
   * @param outWarnings Containing any warnings found
   */
  void computeAllStyles(Registry& registry, std::vector<ParseError>* outWarnings);

  /**
   * Computes the style for the given entities in the registry.
   *
   * @param registry Registry containing the entities
   * @param entities Entities to compute
   * @param outWarnings Containing any warnings found
   */
  void computeStylesFor(Registry& registry, std::span<const Entity> entities,
                        std::vector<ParseError>* outWarnings);

private:
  void computePropertiesInto(EntityHandle handle, ComputedStyleComponent& computedStyle,
                             std::vector<ParseError>* outWarnings);
};

}  // namespace donner::svg::components
