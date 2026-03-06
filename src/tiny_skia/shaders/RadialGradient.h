#pragma once

/// @file shaders/RadialGradient.h
/// @brief Two-point conical (radial) gradient shader.

#include <optional>
#include <variant>
#include <vector>

#include "tiny_skia/shaders/Gradient.h"

namespace tiny_skia {

/// @internal
/// Focal point data for two-point conical gradients.
struct FocalData {
  float r1 = 0.0f;
  float focalX = 0.0f;
  bool isSwapped = false;

  [[nodiscard]] bool isFocalOnCircle() const;
  [[nodiscard]] bool isWellBehaved() const;
  [[nodiscard]] bool isNativelyFocal() const;

  bool set(float r0, float r1, Transform& matrix);
};

/// @internal
struct RadialType {
  float radius1 = 0.0f;
  float radius2 = 0.0f;
};

/// @internal
struct StripType {
  float scaledR0 = 0.0f;
};

/// @internal
using GradientType = std::variant<RadialType, StripType, FocalData>;

/// Two-point conical (radial) gradient shader.
///
/// Supports equal-radius (simple radial), different-radius, and
/// focal (zero-radius start) configurations.
class RadialGradient {
 public:
  explicit RadialGradient(Gradient base) : base_(std::move(base)) {}

  /// Creates a radial gradient between two circles.
  /// Returns a Color if the gradient degenerates to a single color.
  static std::optional<std::variant<Color, RadialGradient>> create(
      Point startPoint, float startRadius, Point endPoint, float endRadius,
      std::vector<GradientStop> stops, SpreadMode mode, Transform transform);

  /// @internal
  [[nodiscard]] bool isOpaque() const { return base_.colorsAreOpaque(); }

  /// @internal
  [[nodiscard]] bool pushStages(ColorSpace cs, pipeline::RasterPipelineBuilder& p) const;

  /// @internal
  Gradient base_;

 private:
  static std::optional<std::variant<Color, RadialGradient>> createRadialUnchecked(
      Point center, float radius, std::vector<GradientStop> stops, SpreadMode mode,
      Transform transform);

  static std::optional<std::variant<Color, RadialGradient>> createTwoPoint(
      Point c0, float r0, Point c1, float r1, std::vector<GradientStop> stops, SpreadMode mode,
      Transform transform);

  GradientType gradientType_;
};

}  // namespace tiny_skia
