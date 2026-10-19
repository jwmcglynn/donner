#pragma once
/// @file

#include <cstdint>
#include <vector>

#include "donner/base/EcsRegistry.h"

namespace donner::svg::components {

/// Phase of an animation element's lifecycle.
enum class AnimationPhase : uint8_t {
  Before,  ///< Document time is before the animation's begin time.
  Active,  ///< Animation is currently active.
  After,   ///< Animation has finished its active duration.
};

/// One resolved active interval, with the unconstrained repeating duration.
struct AnimationInterval {
  double begin = 0;           ///< Inclusive start time.
  double end = 0;             ///< Exclusive active end time.
  double repeatDuration = 0;  ///< End of repeating content relative to begin.

  /// Equality operator.
  bool operator==(const AnimationInterval&) const = default;
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

  /// Retained runtime begin instance times, sorted in document seconds.
  std::vector<double> eventBeginTimes;

  /// Retained runtime end instance times, sorted in document seconds.
  std::vector<double> eventEndTimes;

  /// Resolved intervals for syncbase dependencies and lifecycle observation.
  std::vector<AnimationInterval> intervals;

  /// Duration of repeating content before min/max/end constraints.
  double repeatDuration = 0;

  /// Computed simple duration in seconds. 0 means unresolved, infinity means indefinite.
  double simpleDuration = 0.0;

  /// Computed active duration in seconds.
  double activeDuration = 0.0;
};

}  // namespace donner::svg::components
