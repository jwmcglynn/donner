#pragma once

/// @file shaders/Gradient.h
/// @brief Base gradient data and gradient stop type.

#include <functional>
#include <vector>

#include "tiny_skia/Color.h"
#include "tiny_skia/Point.h"
#include "tiny_skia/pipeline/Pipeline.h"

namespace tiny_skia {

/// @internal
constexpr float kDegenerateThreshold = 1.0f / (1 << 15);

/// A color stop in a gradient.
struct GradientStop {
  NormalizedF32 position; ///< Position along the gradient [0,1].
  Color color;            ///< Color at this position.

  /// Creates a stop, clamping position to [0,1].
  static GradientStop create(float position, Color color) {
    return GradientStop{NormalizedF32::newClamped(position), color};
  }
};

/// @internal
/// Base gradient data shared by all gradient types.
class Gradient {
 public:
  Gradient(std::vector<GradientStop> stops, SpreadMode tileMode, Transform transform,
           Transform pointsToUnit);

  [[nodiscard]] bool colorsAreOpaque() const { return colorsAreOpaque_; }

  [[nodiscard]] bool pushStages(
      pipeline::RasterPipelineBuilder& p, ColorSpace cs,
      const std::function<void(pipeline::RasterPipelineBuilder&)>& pushStagesPre,
      const std::function<void(pipeline::RasterPipelineBuilder&)>& pushStagesPost) const;

  void applyOpacity(float opacity);

  Transform transform;

 private:
  std::vector<GradientStop> stops_;
  SpreadMode tileMode_ = SpreadMode::Pad;
  Transform pointsToUnit_;
  bool colorsAreOpaque_ = true;
  bool hasUniformStops_ = true;
};

}  // namespace tiny_skia
