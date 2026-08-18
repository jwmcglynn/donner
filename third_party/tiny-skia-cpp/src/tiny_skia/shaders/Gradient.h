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

  friend bool operator==(const GradientStop&, const GradientStop&) = default;
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

  /// Attempts to emit a single fused stage for 2-stop Pad linear gradients.
  /// Returns true if fused stage was emitted, false if caller should use normal path.
  [[nodiscard]] bool tryPushFusedLinear2Stop(pipeline::RasterPipelineBuilder& p,
                                             ColorSpace cs) const;

  /// Attempts to emit a single fused stage for 2-stop Pad radial gradients.
  /// Returns true if fused stage was emitted, false if caller should use normal path.
  [[nodiscard]] bool tryPushFusedRadial2Stop(pipeline::RasterPipelineBuilder& p,
                                             ColorSpace cs) const;

  void applyOpacity(float opacity);

  /// Two gradients compare equal when every value the pipeline stages read from them is
  /// equal, which is what lets a caller decide that a shader it built earlier still describes
  /// the paint it wants now. Floats compare bitwise-exactly, so an equal result means the
  /// stages are built from identical inputs, and an unequal one is at worst a missed reuse.
  friend bool operator==(const Gradient&, const Gradient&) = default;

  Transform transform;

 private:
  std::vector<GradientStop> stops_;
  SpreadMode tileMode_ = SpreadMode::Pad;
  Transform pointsToUnit_;
  bool colorsAreOpaque_ = true;
  bool hasUniformStops_ = true;
};

}  // namespace tiny_skia
