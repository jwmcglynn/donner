#include "tiny_skia/shaders/LinearGradient.h"

#include <cmath>

#include "tiny_skia/Math.h"

namespace tiny_skia {

namespace {

float sdot(float a, float b, float c, float d) { return a * b + c * d; }

Transform tsFromSinCosAt(float sin, float cos, float px, float py) {
  const float cosInv = 1.0f - cos;
  return Transform::fromRow(cos, sin, -sin, cos, sdot(sin, py, cosInv, px),
                            sdot(-sin, px, cosInv, py));
}

std::optional<Transform> pointsToUnitTs(Point start, Point end) {
  auto vec = end - start;
  const float mag = vec.length();
  const float inv = (mag != 0.0f) ? tiny_skia::invert(mag) : 0.0f;

  vec.scale(inv);

  auto ts = tsFromSinCosAt(-vec.y, vec.x, start.x, start.y);
  ts = ts.postTranslate(-start.x, -start.y);
  ts = ts.postScale(inv, inv);
  return ts;
}

Color averageGradientColor(const std::vector<GradientStop>& stops) {
  // Simplified average: accumulate weighted color contributions.
  float blendR = 0.0f, blendG = 0.0f, blendB = 0.0f, blendA = 0.0f;

  for (std::size_t i = 0; i + 1 < stops.size(); ++i) {
    const auto c0 = stops[i].color;
    const auto c1 = stops[i + 1].color;
    const float w = 0.5f * (stops[i + 1].position.get() - stops[i].position.get());
    blendR += w * (c0.red() + c1.red());
    blendG += w * (c0.green() + c1.green());
    blendB += w * (c0.blue() + c1.blue());
    blendA += w * (c0.alpha() + c1.alpha());
  }

  // Account for implicit interval at start.
  if (stops[0].position.get() > 0.0f) {
    const float p = stops[0].position.get();
    blendR += p * stops[0].color.red();
    blendG += p * stops[0].color.green();
    blendB += p * stops[0].color.blue();
    blendA += p * stops[0].color.alpha();
  }

  // Account for implicit interval at end.
  const auto lastIdx = stops.size() - 1;
  if (stops[lastIdx].position.get() < 1.0f) {
    const float p = 1.0f - stops[lastIdx].position.get();
    blendR += p * stops[lastIdx].color.red();
    blendG += p * stops[lastIdx].color.green();
    blendB += p * stops[lastIdx].color.blue();
    blendA += p * stops[lastIdx].color.alpha();
  }

  return Color::fromRgba(blendR, blendG, blendB, blendA).value_or(Color::transparent);
}

}  // namespace

std::optional<std::variant<Color, LinearGradient>> LinearGradient::create(
    Point start, Point end, std::vector<GradientStop> stops, SpreadMode mode, Transform transform) {
  if (stops.empty()) return std::nullopt;

  if (stops.size() == 1) {
    return std::variant<Color, LinearGradient>{stops[0].color};
  }

  const float length = (end - start).length();
  if (!std::isfinite(length)) return std::nullopt;

  if (isNearlyZeroWithinTolerance(length, kDegenerateThreshold)) {
    switch (mode) {
      case SpreadMode::Pad:
        return std::variant<Color, LinearGradient>{stops.back().color};
      case SpreadMode::Reflect:
      case SpreadMode::Repeat:
        return std::variant<Color, LinearGradient>{averageGradientColor(stops)};
    }
  }

  if (!transform.invert().has_value()) return std::nullopt;

  const auto unitTs = pointsToUnitTs(start, end);
  if (!unitTs.has_value()) return std::nullopt;

  return std::variant<Color, LinearGradient>{
      LinearGradient{Gradient(std::move(stops), mode, transform, *unitTs)}};
}

bool LinearGradient::pushStages(ColorSpace cs, pipeline::RasterPipelineBuilder& p) const {
  if (base_.tryPushFusedLinear2Stop(p, cs)) {
    return true;
  }
  return base_.pushStages(p, cs, [](auto&) {}, [](auto&) {});
}

}  // namespace tiny_skia
