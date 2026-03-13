#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace donner::svg::components {

/// Interpolation mode for `<animate>`.
enum class CalcMode : uint8_t {
  Discrete, ///< Jump between values at keyTime boundaries.
  Linear,   ///< Linear interpolation (default for `<animate>`).
  Paced,    ///< Constant velocity (default for `<animateMotion>`).
  Spline,   ///< Cubic Bezier easing per interval.
};

/**
 * Component storing `<animate>`-specific value data.
 *
 * Values can be specified via `from`/`to`/`by` pairs or a `values` list.
 * The `values` list takes precedence when present.
 */
struct AnimateValueComponent {
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

  /// Interpolation mode.
  CalcMode calcMode = CalcMode::Linear;

  /// `keyTimes`: maps values to positions in [0,1]. Count must match values count.
  std::vector<double> keyTimes;

  /// `keySplines`: cubic Bezier control points per interval (x1 y1 x2 y2).
  /// Count = (keyTimes count - 1) * 4. Only used when calcMode="spline".
  std::vector<double> keySplines;

  /// `additive`: "replace" (default) or "sum".
  bool additive = false;  // false = replace, true = sum

  /// `accumulate`: "none" (default) or "sum".
  bool accumulate = false;  // false = none, true = sum
};

}  // namespace donner::svg::components
