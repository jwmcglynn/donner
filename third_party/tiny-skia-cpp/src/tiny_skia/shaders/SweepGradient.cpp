#include "tiny_skia/shaders/SweepGradient.h"

#include <cmath>

#include "tiny_skia/Math.h"

namespace tiny_skia {

std::optional<std::variant<Color, SweepGradient>> SweepGradient::create(
    Point center, float startAngle, float endAngle, std::vector<GradientStop> stops,
    SpreadMode mode, Transform transform) {
  if (!std::isfinite(startAngle) || !std::isfinite(endAngle) || startAngle > endAngle) {
    return std::nullopt;
  }

  if (stops.empty()) return std::nullopt;

  if (stops.size() == 1) {
    return std::variant<Color, SweepGradient>{stops[0].color};
  }

  if (!transform.invert().has_value()) return std::nullopt;

  if (isNearlyEqualWithinTolerance(startAngle, endAngle, kDegenerateThreshold)) {
    if (mode == SpreadMode::Pad && endAngle > kDegenerateThreshold) {
      const auto frontColor = stops.front().color;
      const auto backColor = stops.back().color;
      std::vector<GradientStop> newStops;
      newStops.push_back(GradientStop::create(0.0f, frontColor));
      newStops.push_back(GradientStop::create(1.0f, frontColor));
      newStops.push_back(GradientStop::create(1.0f, backColor));
      return SweepGradient::create(center, 0.0f, endAngle, std::move(newStops), mode, transform);
    }
    return std::nullopt;
  }

  if (startAngle <= 0.0f && endAngle >= 360.0f) {
    mode = SpreadMode::Pad;
  }

  const float t0 = startAngle / 360.0f;
  const float t1 = endAngle / 360.0f;

  SweepGradient sg{
      Gradient(std::move(stops), mode, transform, Transform::fromTranslate(-center.x, -center.y))};
  sg.t0_ = t0;
  sg.t1_ = t1;
  return std::variant<Color, SweepGradient>{std::move(sg)};
}

bool SweepGradient::pushStages(ColorSpace cs, pipeline::RasterPipelineBuilder& p) const {
  using Stage = pipeline::Stage;

  const float scale = 1.0f / (t1_ - t0_);
  const float bias = -scale * t0_;
  p.ctx().twoPointConicalGradient.p0 = scale;
  p.ctx().twoPointConicalGradient.p1 = bias;

  return base_.pushStages(
      p, cs,
      [scale, bias](pipeline::RasterPipelineBuilder& b) {
        b.push(Stage::XYToUnitAngle);
        if (scale != 1.0f || bias != 0.0f) {
          b.push(Stage::ApplyConcentricScaleBias);
        }
      },
      [](auto&) {});
}

}  // namespace tiny_skia
