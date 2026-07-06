#pragma once
/// @file

#include <cstdint>
#include <optional>

#include "donner/base/EcsRegistry.h"

namespace donner::svg::components {

/// Phase of an animation element's lifecycle.
enum class AnimationPhase : uint8_t {
  Before, ///< Document time is before the animation's begin time.
  Active, ///< Animation is currently active.
  After,  ///< Animation has finished its active duration.
};

/**
 * Runtime state of an animation element, computed each frame by the AnimationSystem.
 *
 * This component is emplaced on animation entities and updated when the document time changes.
 */
struct AnimationStateComponent {
  /// Current phase of this animation.
  AnimationPhase phase = AnimationPhase::Before;

  /// The entity being targeted by this animation.
  /// Resolved from href or parent element.
  Entity targetEntity = entt::null;

  /// Resolved begin time in seconds (from timing attributes).
  double beginTime = 0.0;

  /// Computed simple duration in seconds. 0 means unresolved, infinity means indefinite.
  double simpleDuration = 0.0;

  /// Computed active duration in seconds.
  double activeDuration = 0.0;

  /// Whether this animation has completed at least one full cycle.
  /// Used to enforce restart="never" — once ended, never restarts.
  bool hasCompleted = false;

  /// Whether this animation was active in the previous frame.
  /// Used to enforce restart="whenNotActive".
  bool wasActive = false;
};

}  // namespace donner::svg::components
