#pragma once
/// @file

#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"

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
  void advance(Registry& registry, double documentTime,
               std::vector<ParseDiagnostic>* outWarnings = nullptr);

  /**
   * Return true when any animation element targets \p targetEntity.
   *
   * An animation targets the entity named by its `href`, or its own parent when `href` is absent,
   * so a container holding an animation element is a target without naming itself anywhere.
   * Resolved targets are read from \ref AnimationStateComponent where \ref advance has already
   * computed them, and resolved the same way as \ref advance where it has not, so the answer does
   * not depend on whether the document has been advanced yet.
   *
   * @param registry The ECS registry.
   * @param targetEntity Entity to test.
   * @return True when an animation element targets \p targetEntity.
   */
  bool isAnimationTarget(Registry& registry, Entity targetEntity) const;
};

}  // namespace donner::svg::components
