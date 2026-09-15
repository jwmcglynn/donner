#pragma once
/// @file
/// Independent raster reference for CSS nonseparable blend modes.
#include <array>
#include <cstdint>
#include <vector>

#include "tiny_skia/Painter.h"
#include "tiny_skia/filter/Blend.h"

namespace donner::gpu::tests {
/// Uses the raster implementation's CSS luminance coefficients.
/// @param mode Hue, saturation, color or luminosity, numbered twelve through fifteen.
/// @param background Premultiplied backdrop. @param foreground Premultiplied source.
/// @return Composited premultiplied RGBA8 pixels.
inline std::vector<uint8_t> NonseparableBlendReference(
    uint32_t mode, const tiny_skia::filter::FloatPixmap& background,
    const tiny_skia::filter::FloatPixmap& foreground) {
  constexpr std::array modes{tiny_skia::BlendMode::Hue, tiny_skia::BlendMode::Saturation,
                             tiny_skia::BlendMode::Color, tiny_skia::BlendMode::Luminosity};
  auto target = background.toPixmap();
  const auto source = foreground.toPixmap();
  auto view = target.mutableView();
  tiny_skia::PixmapPaint paint;
  paint.blendMode = modes.at(mode - 12);
  paint.forceHqPipeline = true;
  tiny_skia::Painter::drawPixmap(view, 0, 0, source.view(), paint);
  return {target.data().begin(), target.data().end()};
}
}  // namespace donner::gpu::tests
