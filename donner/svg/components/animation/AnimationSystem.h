#pragma once
/// @file

#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseError.h"

namespace donner::svg::components {

/**
 * System that advances SVG animations based on the current document time.
 *
 * Runs after style computation but before layout, so that animated property values
 * are applied before layout-dependent computations.
 *
 * For each animation entity:
 * 1. Resolves the target entity (from href or parent).
 * 2. Computes timing: determines if the animation is before/active/after.
 * 3. For active `<set>` animations, stores the override value on the target entity.
 * 4. For frozen animations (fill="freeze"), persists the final value.
 * 5. For removed animations (fill="remove"), clears the override.
 */
class AnimationSystem {
public:
  /**
   * Advance all animations to the given document time.
   *
   * @param registry The ECS registry.
   * @param documentTime The current document time in seconds.
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  void advance(Registry& registry, double documentTime, std::vector<ParseError>* outWarnings);
};

}  // namespace donner::svg::components
