#pragma once

/// @file shaders/LinearGradient.h
/// @brief Two-point linear gradient shader.

#include <optional>
#include <variant>
#include <vector>

#include "tiny_skia/shaders/Gradient.h"

namespace tiny_skia {

/// Two-point linear gradient shader.
///
/// @par Example
/// @snippet linear_gradient.cpp linear_gradient_example
class LinearGradient {
 public:
  /// Creates a linear gradient between two points.
  /// Returns a Color if the gradient degenerates to a single color.
  static std::optional<std::variant<Color, LinearGradient>> create(Point start, Point end,
                                                                   std::vector<GradientStop> stops,
                                                                   SpreadMode mode,
                                                                   Transform transform);

  /// @internal
  [[nodiscard]] bool isOpaque() const { return base_.colorsAreOpaque(); }

  /// @internal
  [[nodiscard]] bool pushStages(ColorSpace cs, pipeline::RasterPipelineBuilder& p) const;

  /// @internal
  Gradient base_;
};

}  // namespace tiny_skia
