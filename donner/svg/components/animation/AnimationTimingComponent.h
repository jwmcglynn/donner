#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <string>

#include "donner/svg/components/animation/ClockValue.h"

namespace donner::svg::components {

/// Fill behavior when an animation finishes.
enum class AnimationFill : uint8_t {
  Remove,  ///< Revert to base value when the animation ends.
  Freeze,  ///< Persist the final animation value.
};

/// Restart behavior for an animation.
enum class AnimationRestart : uint8_t {
  Always,        ///< Can always restart.
  WhenNotActive, ///< Can only restart when not active.
  Never,         ///< Cannot restart.
};

/// A syncbase reference to another animation's begin or end time.
struct SyncbaseRef {
  /// The id of the referenced animation element.
  std::string id;

  /// Whether this references the begin or end of the target.
  enum class Event : uint8_t { Begin, End };
  Event event = Event::End;

  /// Offset to add to the referenced time.
  double offsetSeconds = 0.0;
};

/**
 * Stores timing attributes for an animation element.
 *
 * These correspond to the SMIL timing attributes: `begin`, `dur`, `end`,
 * `repeatCount`, `repeatDur`, `fill`, `restart`, `min`, `max`.
 */
struct AnimationTimingComponent {
  /// Raw `begin` attribute value (semicolon-separated list of time values).
  std::optional<std::string> beginValue;

  /// Parsed begin offset in seconds (from the first offset value in the begin list).
  std::optional<ClockValue> beginOffset;

  /// Syncbase reference for begin (e.g., "anim1.end+0.5s").
  std::optional<SyncbaseRef> beginSyncbase;

  /// The `dur` attribute: simple duration of the animation.
  std::optional<ClockValue> dur;

  /// Raw `end` attribute value.
  std::optional<std::string> endValue;

  /// Parsed end offset in seconds.
  std::optional<ClockValue> endOffset;

  /// Syncbase reference for end.
  std::optional<SyncbaseRef> endSyncbase;

  /// `repeatCount`: number of iterations. Use infinity for "indefinite".
  std::optional<double> repeatCount;

  /// `repeatDur`: total repeat duration.
  std::optional<ClockValue> repeatDur;

  /// `fill`: what to do when the animation ends.
  AnimationFill fill = AnimationFill::Remove;

  /// `restart`: when the animation can restart.
  AnimationRestart restart = AnimationRestart::Always;

  /// `min`: minimum active duration.
  std::optional<ClockValue> min;

  /// `max`: maximum active duration.
  std::optional<ClockValue> max;
};

}  // namespace donner::svg::components
