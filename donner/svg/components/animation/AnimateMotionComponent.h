#pragma once
/// @file

#include <optional>
#include <string>
#include <vector>

namespace donner::svg::components {

/**
 * Component storing `<animateMotion>`-specific data.
 *
 * Motion path is specified via `path` attribute (SVG path data), or implicitly
 * via `from`/`to`/`by`/`values` as point pairs.
 */
struct AnimateMotionComponent {
  /// Optional `href` pointing to the target element.
  std::optional<std::string> href;

  /// Path data string (from `path` attribute).
  std::optional<std::string> path;

  /// The `from` value (point: "x y" or "x,y").
  std::optional<std::string> from;

  /// The `to` value (point: "x y" or "x,y").
  std::optional<std::string> to;

  /// The `by` value (point offset: "dx dy").
  std::optional<std::string> by;

  /// The `values` list (semicolon-separated point pairs).
  std::vector<std::string> values;

  /// The `rotate` attribute: "auto", "auto-reverse", or a fixed angle in degrees.
  /// Stored as string; "auto" and "auto-reverse" are special values.
  std::string rotate = "0";

  /// `keyPoints`: semicolon-separated [0,1] values mapping keyTimes to path progress.
  std::vector<double> keyPoints;
};

}  // namespace donner::svg::components
