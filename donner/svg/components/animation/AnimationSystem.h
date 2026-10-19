#pragma once
/// @file

#include <string_view>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/svg/core/Event.h"

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
/// Whether declaration collection may retire excluded animation sessions.
enum class AnimationCollectionMode {
  ReadOnly,              ///< Preserve runtime state for read-side queries.
  DiscardExcludedState,  ///< Requires write access; discard sessions outside the SVG namespace.
};

/// Collect attached animation declarations in document order.
/// @param registry Document storage.
/// @param mode Read-only query or write-side session cleanup.
/// @return Bounded declaration list, including one overflow entry when rejected.
std::vector<Entity> CollectAnimationEntities(
    Registry& registry, AnimationCollectionMode mode = AnimationCollectionMode::ReadOnly);

/// Check that an element and its ancestors resolve to the SVG namespace.
/// @param registry Document storage.
/// @param entity Element to validate.
/// @return False for foreign, unbound-prefix, or excessively deep ancestry.
bool IsAnimationInSvgNamespace(Registry& registry, Entity entity);

/// Evaluates declarative SVG animation elements at a document time and applies their values.
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
   * References are resolved against the attached document without mutating animation state.
   * Resource exhaustion conservatively returns true so structure-preserving edits cannot treat
   * incomplete analysis as proof that an element is unobserved.
   *
   * @param registry The ECS registry.
   * @param targetEntity Entity to test.
   * @return True for a target or incomplete analysis; false only for a proven non-target.
   */
  bool isAnimationTarget(Registry& registry, Entity targetEntity) const;

  /**
   * Apply declarative begin/end mouse-event triggers to animation runtime state.
   * @param registry Document storage.
   * @param targetEntity Hit element receiving the event.
   * @param type Mouse event type.
   * @param documentTime Event time in document seconds.
   * @return True when an animation interval changed.
   */
  bool dispatchMouseEvent(Registry& registry, Entity targetEntity, EventType type,
                          double documentTime);
  /** Deliver a host event name to declarative timing.
   * @param registry Document storage.
   * @param targetEntity Target element.
   * @param name Event name; no script is executed.
   * @param bubbles Whether ancestors receive the event.
   * @param documentTime Current clock time.
   * @return Whether a runtime instance was retained.
   */
  bool dispatchEvent(Registry& registry, Entity targetEntity, std::string_view name, bool bubbles,
                     double documentTime);
};

}  // namespace donner::svg::components
