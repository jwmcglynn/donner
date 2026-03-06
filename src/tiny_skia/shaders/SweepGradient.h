#pragma once

/// @file shaders/SweepGradient.h
/// @brief Angular sweep gradient shader.

#include <optional>
#include <variant>
#include <vector>

#include "tiny_skia/shaders/Gradient.h"

namespace tiny_skia {

/// Angular sweep gradient shader.
///
/// Colors are distributed around a center point between a start and end angle.
class SweepGradient {
 public:
  explicit SweepGradient(Gradient base) : base_(std::move(base)) {}

  /// Creates a sweep gradient between startAngle and endAngle (degrees).
  /// Returns a Color if the gradient degenerates to a single color.
  static std::optional<std::variant<Color, SweepGradient>> create(Point center, float startAngle,
                                                                  float endAngle,
                                                                  std::vector<GradientStop> stops,
                                                                  SpreadMode mode,
                                                                  Transform transform);

  /// @internal
  [[nodiscard]] bool isOpaque() const { return base_.colorsAreOpaque(); }

  /// @internal
  [[nodiscard]] bool pushStages(ColorSpace cs, pipeline::RasterPipelineBuilder& p) const;

  /// @internal
  Gradient base_;

 private:
  float t0_ = 0.0f;
  float t1_ = 1.0f;
};

}  // namespace tiny_skia
