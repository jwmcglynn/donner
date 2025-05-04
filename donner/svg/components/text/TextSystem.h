// @file
#pragma once

#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseError.h"

namespace donner::svg::components {

/**
 * System to compute text layout spans from raw text and positioning attributes.
 *
 * @ingroup ecs_systems
 */
class TextSystem {
public:
  /**
   * Instantiate computed text spans for all entities with \ref TextComponent.
   *
   * @param registry The registry to instantiate the computed text spans for.
   * @param outWarnings If non-null, a vector to store any warnings that occur during the
   * instantiation.
   */
  void instantiateAllComputedComponents(Registry& registry, std::vector<ParseError>* outWarnings);
};

}  // namespace donner::svg::components
