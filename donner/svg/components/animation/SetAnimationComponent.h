#pragma once
/// @file

#include <optional>
#include <string>

namespace donner::svg::components {

/**
 * Component storing `<set>`-specific animation data.
 *
 * `<set>` is the simplest animation element: it sets a target attribute to a discrete value
 * for the duration of the animation. It has no interpolation — the `to` value applies as-is
 * when the animation is active.
 */
struct SetAnimationComponent {
  /// The `attributeName` identifying which attribute to animate.
  std::string attributeName;

  /// The `to` value to set when the animation is active.
  std::string to;

  /// Optional `href` pointing to the target element. If absent, targets the parent element.
  std::optional<std::string> href;
};

}  // namespace donner::svg::components
