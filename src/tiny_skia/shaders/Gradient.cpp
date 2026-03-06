#include "tiny_skia/shaders/Gradient.h"

#include <algorithm>

#include "tiny_skia/Math.h"

namespace tiny_skia {

Gradient::Gradient(std::vector<GradientStop> stops, SpreadMode tileMode, Transform transform,
                   Transform pointsToUnit)
    : transform(transform),
      stops_(std::move(stops)),
      tileMode_(tileMode),
      pointsToUnit_(pointsToUnit) {
  // Insert dummy endpoints if needed (matching Rust Gradient::new).
  const bool dummyFirst = stops_[0].position.get() != 0.0f;
  const bool dummyLast = stops_[stops_.size() - 1].position.get() != 1.0f;

  if (dummyFirst) {
    stops_.insert(stops_.begin(), GradientStop::create(0.0f, stops_[0].color));
  }
  if (dummyLast) {
    stops_.push_back(GradientStop::create(1.0f, stops_[stops_.size() - 1].color));
  }

  colorsAreOpaque_ = true;
  for (const auto& s : stops_) {
    if (!s.color.isOpaque()) {
      colorsAreOpaque_ = false;
      break;
    }
  }

  // Ensure monotonic positions and check for uniform stops.
  const std::size_t startIndex = dummyFirst ? 0 : 1;
  float prev = 0.0f;
  hasUniformStops_ = true;
  const float uniformStep = stops_[startIndex].position.get() - prev;

  for (std::size_t i = startIndex; i < stops_.size(); ++i) {
    float curr;
    if (i + 1 == stops_.size()) {
      curr = 1.0f;
    } else {
      curr = bound(prev, stops_[i].position.get(), 1.0f);
    }
    hasUniformStops_ &= isNearlyEqual(uniformStep, curr - prev);
    stops_[i].position = NormalizedF32::newClamped(curr);
    prev = curr;
  }
}

bool Gradient::pushStages(
    pipeline::RasterPipelineBuilder& p, ColorSpace cs,
    const std::function<void(pipeline::RasterPipelineBuilder&)>& pushStagesPre,
    const std::function<void(pipeline::RasterPipelineBuilder&)>& pushStagesPost) const {
  using Stage = pipeline::Stage;

  p.push(Stage::SeedShader);

  const auto ts = transform.invert();
  if (!ts.has_value()) {
    return false;
  }
  const auto finalTs = ts->postConcat(pointsToUnit_);
  p.pushTransform(finalTs);

  pushStagesPre(p);

  switch (tileMode_) {
    case SpreadMode::Reflect:
      p.push(Stage::ReflectX1);
      break;
    case SpreadMode::Repeat:
      p.push(Stage::RepeatX1);
      break;
    case SpreadMode::Pad:
      if (hasUniformStops_) {
        p.push(Stage::PadX1);
      }
      break;
  }

  // Two-stop optimization.
  if (stops_.size() == 2) {
    const auto c0 = expandColor(cs, stops_[0].color);
    const auto c1 = expandColor(cs, stops_[1].color);

    p.ctx().evenlySpaced2StopGradient = pipeline::EvenlySpaced2StopGradientCtx{
        .factor = pipeline::GradientColor{c1.red() - c0.red(), c1.green() - c0.green(),
                                          c1.blue() - c0.blue(), c1.alpha() - c0.alpha()},
        .bias = pipeline::GradientColor{c0.red(), c0.green(), c0.blue(), c0.alpha()}};

    p.push(Stage::EvenlySpaced2StopGradient);
  } else {
    pipeline::Context::GradientCtx ctx;

    ctx.factors.reserve(std::max(stops_.size() + 1, std::size_t{16}));
    ctx.biases.reserve(std::max(stops_.size() + 1, std::size_t{16}));
    ctx.tValues.reserve(stops_.size() + 1);

    // Remove dummy stops for the search (matching Rust logic).
    std::size_t firstStop, lastStop;
    if (stops_.size() > 2) {
      firstStop = (stops_[0].color != stops_[1].color) ? 0 : 1;
      const auto len = stops_.size();
      lastStop = (stops_[len - 2].color != stops_[len - 1].color) ? len - 1 : len - 2;
    } else {
      firstStop = 0;
      lastStop = 1;
    }

    float tL = stops_[firstStop].position.get();
    auto cL = ([&] {
      const auto c = expandColor(cs, stops_[firstStop].color);
      return pipeline::GradientColor{c.red(), c.green(), c.blue(), c.alpha()};
    }());
    ctx.pushConstColor(cL);
    ctx.tValues.push_back(0.0f);

    for (std::size_t i = firstStop; i < lastStop; ++i) {
      const float tR = stops_[i + 1].position.get();
      const auto cExpanded = expandColor(cs, stops_[i + 1].color);
      const auto cR = pipeline::GradientColor{cExpanded.red(), cExpanded.green(), cExpanded.blue(),
                                              cExpanded.alpha()};

      if (tL < tR) {
        const float invDt = 1.0f / (tR - tL);
        const auto f = pipeline::GradientColor{(cR.r - cL.r) * invDt, (cR.g - cL.g) * invDt,
                                               (cR.b - cL.b) * invDt, (cR.a - cL.a) * invDt};
        ctx.factors.push_back(f);
        ctx.biases.push_back(pipeline::GradientColor{cL.r - f.r * tL, cL.g - f.g * tL,
                                                     cL.b - f.b * tL, cL.a - f.a * tL});
        ctx.tValues.push_back(bound(0.0f, tL, 1.0f));
      }

      tL = tR;
      cL = cR;
    }

    ctx.pushConstColor(cL);
    ctx.tValues.push_back(bound(0.0f, tL, 1.0f));

    ctx.len = ctx.factors.size();

    // Pad to 16 for lowp F32x16 alignment.
    while (ctx.factors.size() < 16) {
      ctx.factors.push_back(pipeline::GradientColor{});
      ctx.biases.push_back(pipeline::GradientColor{});
    }

    p.push(Stage::Gradient);
    p.ctx().gradient = std::move(ctx);
  }

  if (!colorsAreOpaque_) {
    p.push(Stage::Premultiply);
  }

  pushStagesPost(p);

  return true;
}

void Gradient::applyOpacity(float opacity) {
  for (auto& stop : stops_) {
    stop.color.applyOpacity(opacity);
  }
  colorsAreOpaque_ = true;
  for (const auto& s : stops_) {
    if (!s.color.isOpaque()) {
      colorsAreOpaque_ = false;
      break;
    }
  }
}

}  // namespace tiny_skia
