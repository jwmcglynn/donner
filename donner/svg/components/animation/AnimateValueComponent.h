#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "donner/svg/components/animation/AnimationInterpolation.h"

namespace donner::svg::components {

/**
 * Component storing `<animate>`-specific value data.
 *
 * Values can be specified via `from`/`to`/`by` pairs or a `values` list.
 * The `values` list takes precedence when present.
 */
struct AnimateValueComponent : AnimationInterpolation {
  /// The `attributeName` identifying which attribute to animate.
  std::string attributeName;

  /// Optional `href` pointing to the target element. If absent, targets the parent element.
  std::optional<std::string> href;

  /// The `from` value (start of interpolation).
  std::optional<std::string> from;

  /// The `to` value (end of interpolation).
  std::optional<std::string> to;

  /// The `by` value (offset from start).
  std::optional<std::string> by;

  /// The `values` list (semicolon-separated keyframes). Takes precedence over from/to/by.
  std::vector<std::string> values;
};

}  // namespace donner::svg::components
