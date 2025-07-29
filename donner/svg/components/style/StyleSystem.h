#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::components {

class ResourceManagerContext;

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
   * @param resourceManager The resource manager to use for loading fonts.
   * @param outWarnings Containing any warnings found
   * @returns Computed style component for the entity
   */
  static const ComputedStyleComponent& computeStyle(EntityHandle handle,
                                                    ResourceManagerContext& resourceManager,
                                                    std::vector<ParseError>* outWarnings);

  /**
   * Computes the style for all entities in the registry.
   *
   * @param registry Registry to compute the styles, used to query for all entities in the tree.
   * @param resourceManager The resource manager to use for loading fonts.
   * @param outWarnings Containing any warnings found
   */
  static void computeAllStyles(Registry& registry, ResourceManagerContext& resourceManager,
                               std::vector<ParseError>* outWarnings);

  /**
   * Computes the style for the given entities in the registry.
   *
   * @param registry Registry containing the entities
   * @param entities Entities to compute
   * @param resourceManager The resource manager to use for loading fonts.
   * @param outWarnings Containing any warnings found
   */
  static void computeStylesFor(Registry& registry, std::span<const Entity> entities,
                               ResourceManagerContext& resourceManager,
                               std::vector<ParseError>* outWarnings);

  /**
   * Invalidate the computed style for a given entity.
   *
   * @param handle Entity handle to invalidate
   */
  static void invalidateComputed(EntityHandle handle);

  /**
   * Invalidate the full style and reparse attributes.
   *
   * @param handle Entity handle to invalidate
   */
  static void invalidateAll(EntityHandle handle);

private:
  static void computePropertiesInto(EntityHandle handle, ComputedStyleComponent& computedStyle,
                                    ResourceManagerContext& resourceManager,
                                    std::vector<ParseError>* outWarnings);
};

}  // namespace donner::svg::components
