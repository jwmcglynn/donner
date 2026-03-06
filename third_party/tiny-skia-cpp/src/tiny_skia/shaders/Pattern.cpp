#include "tiny_skia/shaders/Pattern.h"

#include <cmath>

namespace tiny_skia {

Pattern::Pattern(PixmapView pixmap, SpreadMode spreadMode, FilterQuality quality, float opacity,
                 Transform transform)
    : pixmap_(pixmap),
      opacity_(NormalizedF32::newClamped(opacity)),
      transform_(transform),
      quality_(quality),
      spreadMode_(spreadMode) {}

bool Pattern::isOpaque() const {
  // Pattern always reports as non-opaque.
  return false;
}

bool Pattern::pushStages(ColorSpace cs, pipeline::RasterPipelineBuilder& p) const {
  using Stage = pipeline::Stage;

  const auto ts = transform_.invert();
  if (!ts.has_value()) return false;

  p.push(Stage::SeedShader);
  p.pushTransform(*ts);

  auto quality = quality_;

  // Reduce quality if transform is identity or translate-only.
  if (ts->isIdentity() || ts->isTranslate()) {
    quality = FilterQuality::Nearest;
  }

  if (quality == FilterQuality::Bilinear && ts->isTranslate()) {
    if (ts->tx == std::trunc(ts->tx) && ts->ty == std::trunc(ts->ty)) {
      quality = FilterQuality::Nearest;
    }
  }

  switch (quality) {
    case FilterQuality::Nearest: {
      p.ctx().limitX = pipeline::TileCtx{.scale = static_cast<float>(pixmap_.width()),
                                          .invScale = 1.0f / static_cast<float>(pixmap_.width())};
      p.ctx().limitY = pipeline::TileCtx{.scale = static_cast<float>(pixmap_.height()),
                                          .invScale = 1.0f / static_cast<float>(pixmap_.height())};

      switch (spreadMode_) {
        case SpreadMode::Pad:
          break;  // Gather stage will clamp.
        case SpreadMode::Repeat:
          p.push(Stage::Repeat);
          break;
        case SpreadMode::Reflect:
          p.push(Stage::Reflect);
          break;
      }

      p.push(Stage::Gather);
      break;
    }
    case FilterQuality::Bilinear: {
      p.ctx().sampler =
          pipeline::SamplerCtx{.spreadMode = spreadMode_,
                               .invWidth = 1.0f / static_cast<float>(pixmap_.width()),
                               .invHeight = 1.0f / static_cast<float>(pixmap_.height())};
      p.push(Stage::Bilinear);
      break;
    }
    case FilterQuality::Bicubic: {
      p.ctx().sampler =
          pipeline::SamplerCtx{.spreadMode = spreadMode_,
                               .invWidth = 1.0f / static_cast<float>(pixmap_.width()),
                               .invHeight = 1.0f / static_cast<float>(pixmap_.height())};
      p.push(Stage::Bicubic);
      p.push(Stage::Clamp0);
      p.push(Stage::ClampA);
      break;
    }
  }

  if (opacity_ != NormalizedF32::one()) {
    p.ctx().currentCoverage = opacity_.get();
    p.push(Stage::Scale1Float);
  }

  const auto expandSt = expandStage(cs);
  if (expandSt.has_value()) {
    p.push(*expandSt);
  }

  return true;
}

}  // namespace tiny_skia
