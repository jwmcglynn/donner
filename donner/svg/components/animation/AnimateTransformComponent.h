#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace donner::svg::components {

/// Transform type for `<animateTransform>`.
enum class TransformAnimationType : uint8_t {
  Translate,  ///< translate(tx [ty])
  Scale,      ///< scale(sx [sy])
  Rotate,     ///< rotate(angle [cx cy])
  SkewX,      ///< skewX(angle)
  SkewY,      ///< skewY(angle)
};

/**
 * Component storing `<animateTransform>`-specific data.
 *
 * Values are stored as strings containing space-separated numbers.
 * The number of values per entry depends on the transform type.
 */
struct AnimateTransformComponent {
  /// The transform type (translate, scale, rotate, skewX, skewY).
  TransformAnimationType type = TransformAnimationType::Translate;

  /// Optional `href` pointing to the target element.
  std::optional<std::string> href;

  /// The `from` value (space-separated numbers).
  std::optional<std::string> from;

  /// The `to` value (space-separated numbers).
  std::optional<std::string> to;

  /// The `by` value (space-separated numbers).
  std::optional<std::string> by;

  /// The `values` list (semicolon-separated keyframes).
  std::vector<std::string> values;

  /// `additive`: false = replace, true = sum.
  bool additive = false;

  /// `accumulate`: false = none, true = sum.
  bool accumulate = false;
};

}  // namespace donner::svg::components
