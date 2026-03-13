#pragma once
/// @file

#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/svg/core/Event.h"

namespace donner::svg::components {

/**
 * Handles DOM event dispatch through the SVG element tree.
 *
 * Implements the DOM Level 2 event model with capture, target, and bubble phases.
 * Events are dispatched to event listeners registered via \ref EventListenersComponent.
 *
 * @ingroup ecs_systems
 */
class EventSystem {
public:
  /**
   * Dispatch an event to the target entity, propagating through capture and bubble phases.
   *
   * The dispatch follows DOM Level 2 semantics:
   * 1. Build ancestor path from root to target.
   * 2. **Capture phase:** Fire capture listeners on each ancestor (root to target-1).
   * 3. **Target phase:** Fire both capture and bubble listeners on the target.
   * 4. **Bubble phase:** Fire bubble listeners from target-1 back to root (if the event bubbles).
   *
   * Propagation stops early if \ref Event::stopPropagation() is called.
   *
   * @param registry The ECS registry.
   * @param event The event to dispatch. Its `target` field must be set.
   */
  void dispatch(Registry& registry, Event& event);

  /**
   * Update hover tracking and fire enter/leave events as needed.
   *
   * Compares the entity at `point` (from hit testing) with the previously hovered entity.
   * If the hovered entity changes, fires:
   * - `mouseleave` on the old entity (does not bubble).
   * - `mouseenter` on the new entity (does not bubble).
   * - `mouseout` on the old entity (bubbles).
   * - `mouseover` on the new entity (bubbles).
   *
   * @param registry The ECS registry.
   * @param newHoveredEntity The entity currently under the pointer (or entt::null).
   * @param documentPosition The pointer position in document coordinates.
   */
  void updateHover(Registry& registry, Entity newHoveredEntity, const Vector2d& documentPosition);

private:
  /**
   * Build the ancestor path from the root to the given entity.
   *
   * @param registry The ECS registry.
   * @param entity The target entity.
   * @param outPath Output vector, filled with [root, ..., parent, entity].
   */
  static void buildAncestorPath(Registry& registry, Entity entity,
                                std::vector<Entity>& outPath);

  /**
   * Fire all matching listeners on a specific entity for the given event.
   *
   * @param registry The ECS registry.
   * @param entity The entity whose listeners to invoke.
   * @param event The event being dispatched.
   * @param capture If true, fire capture-phase listeners; otherwise bubble-phase.
   */
  static void fireListeners(Registry& registry, Entity entity, Event& event, bool capture);
};

}  // namespace donner::svg::components
