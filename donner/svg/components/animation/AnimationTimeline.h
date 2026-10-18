#pragma once
/// @file

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"

namespace donner::svg::components {

/// One declarative timing source and its offset.
struct AnimationTimeSpecifier {
  /// An offset, a begin/end dependency, a repeat dependency, or a host event.
  enum class Kind { Offset, Begin, End, Repeat, Event };
  Kind kind = Kind::Offset;  ///< Source type.
  std::string sourceId;      ///< Empty for an event on the animation target.
  std::string event;         ///< Host event name.
  double offset = 0;         ///< Signed offset in seconds.
  unsigned repeat = 0;       ///< One-based repeat index.
};

/** Parse a bounded timing list. Unsupported entries remain unresolved.
 * @param value Authored timing list.
 * @return Parsed sources, or nullopt when a list exceeds its resource budget.
 */
std::optional<std::vector<AnimationTimeSpecifier>> ParseAnimationTimeList(std::string_view value);

/** Resolve animation intervals and select their state at an arbitrary document time.
 * @param registry Document storage.
 * @param animations Admitted animation declarations, in document order.
 * @param time Document time in seconds.
 * @return False if dependency expansion exceeds the bounded timing budget.
 */
bool ResolveAnimationTimeline(Registry& registry, std::span<const Entity> animations, double time);

/** Resolve intervals using the caller's shared reference allowance.
 * @param registry Document storage.
 * @param animations Admitted declarations in document order.
 * @param time Document time in seconds.
 * @param budget Shared sample or event allowance.
 * @return False on timing or reference-work exhaustion.
 */
bool ResolveAnimationTimeline(Registry& registry, std::span<const Entity> animations, double time,
                              AnimationResourceBudget& budget);

/** Retain a finite event instance time, without discarding earlier intervals.
 * @param times Sorted runtime instance times.
 * @param time New instance time.
 * @return Whether a new instance was retained.
 */
bool InsertAnimationInstance(std::vector<double>& times, double time);

}  // namespace donner::svg::components
