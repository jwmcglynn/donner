#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/svg/compositor/CompositorController.h"

namespace donner::svg::compositor {

/**
 * RAII session for a drag interaction with composited rendering.
 *
 * Promotes the target entity on construction and demotes it on destruction, ensuring the compositor
 * layer lifecycle is tied to the drag gesture. During the drag, `updateTranslation()` sets the
 * composition transform for the promoted layer.
 *
 * Usage:
 * @code
 * // On pointer-down / drag start:
 * auto drag = DragSession::begin(compositor, hitEntity);
 * if (!drag) { return; }  // promotion failed, fall back to full re-render
 *
 * // On pointer-move:
 * drag->updateTranslation(currentPos - startPos);
 * compositor.renderFrame(viewport);
 *
 * // On pointer-up / drag end:
 * drag.reset();  // or let it fall out of scope
 * compositor.renderFrame(viewport);  // full re-render with entity at new position
 * @endcode
 */
class DragSession {
public:
  /**
   * Begin a drag session by promoting the target entity.
   *
   * @param compositor The compositor controller managing layers.
   * @param target The entity to drag.
   * @return A DragSession if promotion succeeded, or std::nullopt if the entity could not be
   *         promoted (invalid entity, layer limit reached, etc.).
   */
  static std::optional<DragSession> begin(CompositorController& compositor, Entity target);

  /// Destructor. Demotes the target entity if the session is still active.
  ~DragSession();

  // Non-copyable, movable.
  DragSession(const DragSession&) = delete;
  DragSession& operator=(const DragSession&) = delete;
  DragSession(DragSession&& other) noexcept;
  DragSession& operator=(DragSession&& other) noexcept;

  /**
   * Update the drag translation relative to the start position.
   *
   * @param delta Translation offset from the entity's original position, in document coordinates.
   */
  void updateTranslation(const Vector2d& delta);

  /// Returns the target entity being dragged.
  [[nodiscard]] Entity target() const { return target_; }

  /// Returns true if this session is still active (entity is promoted).
  [[nodiscard]] bool isActive() const { return compositor_ != nullptr; }

  /// Explicitly end the drag session, demoting the entity.
  void end();

private:
  DragSession(CompositorController& compositor, Entity target);

  CompositorController* compositor_ = nullptr;
  Entity target_ = entt::null;
};

}  // namespace donner::svg::compositor
