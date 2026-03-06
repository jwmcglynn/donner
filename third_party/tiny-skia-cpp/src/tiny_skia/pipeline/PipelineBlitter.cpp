#include "tiny_skia/pipeline/Blitter.h"

#include <algorithm>
#include <cstring>

#include "tiny_skia/Painter.h"
#include "tiny_skia/shaders/Shaders.h"

namespace tiny_skia::pipeline {
namespace {

constexpr std::size_t kBytesPerPixel = 4;

[[nodiscard]] MaskCtx makeMaskCtx(const std::optional<SubMaskView>& mask) {
  if (!mask.has_value()) {
    return MaskCtx{};
  }
  return MaskCtx{mask->data, mask->realWidth};
}

[[nodiscard]] Pixmap makeDummyPixmapSrc() {
  auto p = Pixmap::fromSize(1, 1);
  return p.has_value() ? std::move(*p) : Pixmap{};
}

}  // namespace

RasterPipelineBlitter::RasterPipelineBlitter(MutableSubPixmapView* pixmap, bool isMaskOnly,
                                             std::optional<PremultipliedColorU8> memsetColor,
                                             std::optional<SubMaskView> mask,
                                             Pixmap pixmapSrcStorage, RasterPipeline blitAntiHRp,
                                             RasterPipeline blitRectRp, RasterPipeline blitMaskRp)
    : pixmap_(pixmap),
      isMaskOnly_(isMaskOnly),
      memsetColor_(memsetColor),
      mask_(mask),
      pixmapSrcStorage_(std::move(pixmapSrcStorage)),
      blitAntiHRp_(blitAntiHRp),
      blitRectRp_(blitRectRp),
      blitMaskRp_(blitMaskRp) {}

std::optional<RasterPipelineBlitter> RasterPipelineBlitter::create(PremultipliedColorU8 color,
                                                                   MutableSubPixmapView* pixmap,
                                                                   BlendMode blendMode,
                                                                   std::optional<SubMaskView> mask) {
  if (pixmap == nullptr) {
    return std::nullopt;
  }

  if (mask.has_value()) {
    if (mask->size.width() != pixmap->width() || mask->size.height() != pixmap->height()) {
      return std::nullopt;
    }
  }

  // Fast-reject: Destination keeps the pixmap unchanged.
  if (blendMode == BlendMode::Destination) {
    return std::nullopt;
  }

  // Strength-reduce SourceOver to Source when opaque and no mask.
  if (color.alpha() == 255u && blendMode == BlendMode::SourceOver && !mask.has_value()) {
    blendMode = BlendMode::Source;
  }

  // Compute memset2d_color for Source mode with no mask.
  std::optional<PremultipliedColorU8> memsetColor;
  if (blendMode == BlendMode::Source && !mask.has_value()) {
    memsetColor = color;
  }

  // Clear is transparent memset (when no mask).
  if (blendMode == BlendMode::Clear && !mask.has_value()) {
    blendMode = BlendMode::Source;
    memsetColor = PremultipliedColorU8::fromRgbaUnchecked(0, 0, 0, 0);
  }

  // Convert color to premultiplied float for pipeline.
  const auto premultColor =
      PremultipliedColor{NormalizedF32::newClamped(static_cast<float>(color.red()) / 255.0f),
                         NormalizedF32::newClamped(static_cast<float>(color.green()) / 255.0f),
                         NormalizedF32::newClamped(static_cast<float>(color.blue()) / 255.0f),
                         NormalizedF32::newClamped(static_cast<float>(color.alpha()) / 255.0f)};

  // Build blit_anti_h pipeline.
  auto blitAntiHRp = [&]() {
    RasterPipelineBuilder p;
    p.pushUniformColor(premultColor);
    if (mask.has_value()) {
      p.push(Stage::MaskU8);
    }
    if (shouldPreScaleCoverage(blendMode)) {
      p.push(Stage::Scale1Float);
      p.push(Stage::LoadDestination);
      if (const auto stage = toStage(blendMode)) {
        p.push(*stage);
      }
    } else {
      p.push(Stage::LoadDestination);
      if (const auto stage = toStage(blendMode)) {
        p.push(*stage);
      }
      p.push(Stage::Lerp1Float);
    }
    p.push(Stage::Store);
    return p.compile();
  }();

  // Build blit_rect pipeline.
  auto blitRectRp = [&]() {
    RasterPipelineBuilder p;
    p.pushUniformColor(premultColor);
    if (mask.has_value()) {
      p.push(Stage::MaskU8);
    }
    if (blendMode == BlendMode::SourceOver && !mask.has_value()) {
      p.push(Stage::SourceOverRgba);
    } else {
      if (blendMode != BlendMode::Source) {
        p.push(Stage::LoadDestination);
        if (const auto stage = toStage(blendMode)) {
          p.push(*stage);
        }
      }
      p.push(Stage::Store);
    }
    return p.compile();
  }();

  // Build blit_mask pipeline.
  auto blitMaskRp = [&]() {
    RasterPipelineBuilder p;
    p.pushUniformColor(premultColor);
    if (mask.has_value()) {
      p.push(Stage::MaskU8);
    }
    if (shouldPreScaleCoverage(blendMode)) {
      p.push(Stage::ScaleU8);
      p.push(Stage::LoadDestination);
      if (const auto stage = toStage(blendMode)) {
        p.push(*stage);
      }
    } else {
      p.push(Stage::LoadDestination);
      if (const auto stage = toStage(blendMode)) {
        p.push(*stage);
      }
      p.push(Stage::LerpU8);
    }
    p.push(Stage::Store);
    return p.compile();
  }();

  return RasterPipelineBlitter(pixmap, false, memsetColor, mask, makeDummyPixmapSrc(), blitAntiHRp,
                               blitRectRp, blitMaskRp);
}

std::optional<RasterPipelineBlitter> RasterPipelineBlitter::create(const tiny_skia::Paint& paint,
                                                                   std::optional<SubMaskView> mask,
                                                                   MutableSubPixmapView* pixmap) {
  if (pixmap == nullptr) {
    return std::nullopt;
  }

  // Validate mask size matches pixmap.
  if (mask.has_value()) {
    if (mask->size.width() != pixmap->width() || mask->size.height() != pixmap->height()) {
      return std::nullopt;
    }
  }

  // Fast-reject: Destination keeps the pixmap unchanged.
  if (paint.blendMode == BlendMode::Destination) {
    return std::nullopt;
  }

  // Fast-reject: DestinationIn with opaque solid color is a no-op.
  if (paint.blendMode == BlendMode::DestinationIn && isShaderOpaque(paint.shader) &&
      paint.isSolidColor()) {
    return std::nullopt;
  }

  // Strength-reduce SourceOver to Source when opaque and no mask.
  auto blendMode = paint.blendMode;
  if (isShaderOpaque(paint.shader) && blendMode == BlendMode::SourceOver && !mask.has_value()) {
    blendMode = BlendMode::Source;
  }

  // When drawing a constant color in Source mode with no mask, use memset.
  std::optional<PremultipliedColorU8> memsetColor;
  if (paint.isSolidColor() && blendMode == BlendMode::Source && !mask.has_value()) {
    const auto& color = std::get<Color>(paint.shader);
    memsetColor = color.premultiply().toColorU8();
  }

  // Clear is just a transparent color memset (when not anti-aliased and no mask).
  if (blendMode == BlendMode::Clear && !paint.antiAlias && !mask.has_value()) {
    blendMode = BlendMode::Source;
    memsetColor = PremultipliedColorU8::fromRgbaUnchecked(0, 0, 0, 0);
  }

  // Build blit_anti_h pipeline.
  auto blitAntiHRp = [&]() -> std::optional<RasterPipeline> {
    RasterPipelineBuilder p;
    p.setForceHqPipeline(paint.forceHqPipeline);
    if (!pushShaderStages(paint.shader, paint.colorspace, p)) {
      return std::nullopt;
    }
    if (mask.has_value()) {
      p.push(Stage::MaskU8);
    }
    if (shouldPreScaleCoverage(blendMode)) {
      p.push(Stage::Scale1Float);
      p.push(Stage::LoadDestination);
      if (const auto stage = expandDestStage(paint.colorspace)) {
        p.push(*stage);
      }
      if (const auto stage = toStage(blendMode)) {
        p.push(*stage);
      }
    } else {
      p.push(Stage::LoadDestination);
      if (const auto stage = expandDestStage(paint.colorspace)) {
        p.push(*stage);
      }
      if (const auto stage = toStage(blendMode)) {
        p.push(*stage);
      }
      p.push(Stage::Lerp1Float);
    }
    if (const auto stage = compressStage(paint.colorspace)) {
      p.push(*stage);
    }
    p.push(Stage::Store);
    return p.compile();
  }();
  if (!blitAntiHRp.has_value()) {
    return std::nullopt;
  }

  // Build blit_rect pipeline.
  auto blitRectRp = [&]() -> std::optional<RasterPipeline> {
    RasterPipelineBuilder p;
    p.setForceHqPipeline(paint.forceHqPipeline);
    if (!pushShaderStages(paint.shader, paint.colorspace, p)) {
      return std::nullopt;
    }
    if (mask.has_value()) {
      p.push(Stage::MaskU8);
    }
    if (blendMode == BlendMode::SourceOver && !mask.has_value()) {
      if (const auto stage = compressStage(paint.colorspace)) {
        p.push(*stage);
      }
      p.push(Stage::SourceOverRgba);
    } else {
      if (blendMode != BlendMode::Source) {
        p.push(Stage::LoadDestination);
        if (const auto blendStage = toStage(blendMode)) {
          if (const auto stage = expandDestStage(paint.colorspace)) {
            p.push(*stage);
          }
          p.push(*blendStage);
        }
      }
      if (const auto stage = compressStage(paint.colorspace)) {
        p.push(*stage);
      }
      p.push(Stage::Store);
    }
    return p.compile();
  }();
  if (!blitRectRp.has_value()) {
    return std::nullopt;
  }

  // Build blit_mask pipeline.
  auto blitMaskRp = [&]() -> std::optional<RasterPipeline> {
    RasterPipelineBuilder p;
    p.setForceHqPipeline(paint.forceHqPipeline);
    if (!pushShaderStages(paint.shader, paint.colorspace, p)) {
      return std::nullopt;
    }
    if (mask.has_value()) {
      p.push(Stage::MaskU8);
    }
    if (shouldPreScaleCoverage(blendMode)) {
      p.push(Stage::ScaleU8);
      p.push(Stage::LoadDestination);
      if (const auto stage = expandDestStage(paint.colorspace)) {
        p.push(*stage);
      }
      if (const auto stage = toStage(blendMode)) {
        p.push(*stage);
      }
    } else {
      p.push(Stage::LoadDestination);
      if (const auto stage = expandDestStage(paint.colorspace)) {
        p.push(*stage);
      }
      if (const auto stage = toStage(blendMode)) {
        p.push(*stage);
      }
      p.push(Stage::LerpU8);
    }
    if (const auto stage = compressStage(paint.colorspace)) {
      p.push(*stage);
    }
    p.push(Stage::Store);
    return p.compile();
  }();
  if (!blitMaskRp.has_value()) {
    return std::nullopt;
  }

  // Get the pixmap source from the shader.
  // Pattern shaders need the actual pixmap data; others use a dummy.
  Pixmap pixmapSrcStorage;
  if (std::holds_alternative<Pattern>(paint.shader)) {
    const auto& patt = std::get<Pattern>(paint.shader);
    auto cloned = patt.pixmap_.cloneRect(
        *IntRect::fromXYWH(0, 0, patt.pixmap_.width(), patt.pixmap_.height()));
    if (cloned.has_value()) {
      pixmapSrcStorage = std::move(*cloned);
    } else {
      pixmapSrcStorage = makeDummyPixmapSrc();
    }
  } else {
    pixmapSrcStorage = makeDummyPixmapSrc();
  }

  return RasterPipelineBlitter(pixmap, false, memsetColor, mask, std::move(pixmapSrcStorage),
                               *blitAntiHRp, *blitRectRp, *blitMaskRp);
}

std::optional<RasterPipelineBlitter> RasterPipelineBlitter::createMask(MutableSubPixmapView* pixmap) {
  if (pixmap == nullptr) {
    return std::nullopt;
  }

  const auto color = Color::white.premultiply();
  const auto memsetColor = color.toColorU8();

  // Build blit_anti_h pipeline (mask mode: lerp dest with coverage).
  auto blitAntiHRp = [&]() {
    RasterPipelineBuilder p;
    p.pushUniformColor(color);
    p.push(Stage::LoadDestination);
    p.push(Stage::Lerp1Float);
    p.push(Stage::Store);
    return p.compile();
  }();

  // Build blit_rect pipeline (mask mode: overwrite).
  auto blitRectRp = [&]() {
    RasterPipelineBuilder p;
    p.pushUniformColor(color);
    p.push(Stage::Store);
    return p.compile();
  }();

  // Build blit_mask pipeline (mask mode: lerp dest with mask coverage).
  auto blitMaskRp = [&]() {
    RasterPipelineBuilder p;
    p.pushUniformColor(color);
    p.push(Stage::LoadDestination);
    p.push(Stage::LerpU8);
    p.push(Stage::Store);
    return p.compile();
  }();

  return RasterPipelineBlitter(pixmap, true, memsetColor, std::nullopt, makeDummyPixmapSrc(),
                               blitAntiHRp, blitRectRp, blitMaskRp);
}

void RasterPipelineBlitter::blitH(std::uint32_t x, std::uint32_t y, LengthU32 width) {
  blitRect(ScreenIntRect::fromXYWHSafe(x, y, width, 1u));
}

void RasterPipelineBlitter::blitAntiH(std::uint32_t x, std::uint32_t y,
                                      std::span<std::uint8_t> alpha, std::span<AlphaRun> runs) {
  if (pixmap_ == nullptr || alpha.empty() || runs.empty()) {
    return;
  }

  const auto maskCtx = makeMaskCtx(mask_);
  const auto pixmapSrcRef = pixmapSrcStorage_.view();

  std::size_t aaOffset = 0;
  std::size_t runOffset = 0;
  while (runOffset < runs.size() && runs[runOffset].has_value()) {
    const auto run = static_cast<LengthU32>(runs[runOffset].value());
    if (run == 0u || aaOffset >= alpha.size()) {
      break;
    }

    const auto coverage = alpha[aaOffset];
    if (coverage == 255u) {
      blitH(x, y, run);
    } else if (coverage != 0u) {
      blitAntiHRp_.ctx().currentCoverage = static_cast<float>(coverage) * (1.0f / 255.0f);
      const auto rect = ScreenIntRect::fromXYWHSafe(x, y, run, 1u);
      blitAntiHRp_.run(rect, AAMaskCtx{}, maskCtx, pixmapSrcRef, pixmap_);
    }

    x += run;
    runOffset += static_cast<std::size_t>(run);
    aaOffset += static_cast<std::size_t>(run);
  }
}

void RasterPipelineBlitter::blitV(std::uint32_t x, std::uint32_t y, LengthU32 height,
                                  AlphaU8 alpha) {
  if (alpha == 0u) {
    return;
  }

  const auto bounds = ScreenIntRect::fromXYWHSafe(x, y, 1u, height);
  const AAMaskCtx aaMaskCtx{{alpha, alpha},
                              0u,  // rowBytes=0: reuse same data for all rows
                              static_cast<std::size_t>(bounds.x())};
  const auto maskCtx = makeMaskCtx(mask_);
  const auto pixmapSrcRef = pixmapSrcStorage_.view();
  blitMaskRp_.run(bounds, aaMaskCtx, maskCtx, pixmapSrcRef, pixmap_);
}

void RasterPipelineBlitter::blitAntiH2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0,
                                       AlphaU8 alpha1) {
  const auto bounds = ScreenIntRect::fromXYWH(x, y, 2, 1);
  if (!bounds.has_value()) {
    return;
  }
  const AAMaskCtx aaMaskCtx{
      {alpha0, alpha1}, 2u, static_cast<std::size_t>(bounds->x() + bounds->y() * 2)};
  const auto maskCtx = makeMaskCtx(mask_);
  const auto pixmapSrcRef = pixmapSrcStorage_.view();
  blitMaskRp_.run(*bounds, aaMaskCtx, maskCtx, pixmapSrcRef, pixmap_);
}

void RasterPipelineBlitter::blitAntiV2(std::uint32_t x, std::uint32_t y, AlphaU8 alpha0,
                                       AlphaU8 alpha1) {
  const auto bounds = ScreenIntRect::fromXYWH(x, y, 1, 2);
  if (!bounds.has_value()) {
    return;
  }
  const AAMaskCtx aaMaskCtx{
      {alpha0, alpha1}, 1u, static_cast<std::size_t>(bounds->x() + bounds->y() * 1)};
  const auto maskCtx = makeMaskCtx(mask_);
  const auto pixmapSrcRef = pixmapSrcStorage_.view();
  blitMaskRp_.run(*bounds, aaMaskCtx, maskCtx, pixmapSrcRef, pixmap_);
}

void RasterPipelineBlitter::blitRect(const ScreenIntRect& rect) {
  if (pixmap_ == nullptr) {
    return;
  }

  if (memsetColor_.has_value()) {
    const auto c = *memsetColor_;
    const auto maxX = std::min<std::size_t>(pixmap_->width(), rect.x() + rect.width());
    const auto maxY = std::min<std::size_t>(pixmap_->height(), rect.y() + rect.height());
    const auto rowWidth = maxX - rect.x();
    auto* data = pixmap_->data;

    if (isMaskOnly_) {
      for (std::size_t yy = rect.y(); yy < maxY; ++yy) {
        for (std::size_t xx = rect.x(); xx < maxX; ++xx) {
          const auto offset = (yy * pixmap_->realWidth + xx) * kBytesPerPixel;
          data[offset + 3] = c.alpha();
        }
      }
    } else {
      // Pack the 4-byte color into a uint32_t and fill entire rows at once.
      std::uint32_t pixel = 0;
      std::memcpy(&pixel, &c, sizeof(pixel));
      auto* pixels = reinterpret_cast<std::uint32_t*>(data);
      for (std::size_t yy = rect.y(); yy < maxY; ++yy) {
        auto* row = pixels + yy * pixmap_->realWidth + rect.x();
        std::fill(row, row + rowWidth, pixel);
      }
    }
    return;
  }

  const auto maskCtx = makeMaskCtx(mask_);
  const auto pixmapSrcRef = pixmapSrcStorage_.view();
  blitRectRp_.run(rect, AAMaskCtx{}, maskCtx, pixmapSrcRef, pixmap_);
}

void RasterPipelineBlitter::blitMask(const Mask& mask, const ScreenIntRect& clip) {
  if (pixmap_ == nullptr) {
    return;
  }

  const auto maskData = mask.data();
  const auto maxWidth = std::min<std::size_t>(clip.width(), mask.width());
  const auto maxHeight = std::min<std::size_t>(clip.height(), mask.height());
  const auto maxX = std::min<std::size_t>(pixmap_->width(), clip.x() + maxWidth);
  const auto maxY = std::min<std::size_t>(pixmap_->height(), clip.y() + maxHeight);

  const auto maskCtxExternal = makeMaskCtx(mask_);
  const auto pixmapSrcRef = pixmapSrcStorage_.view();

  // Process row by row, 2 pixels at a time through the pipeline.
  for (std::size_t yy = clip.y(); yy < maxY; ++yy) {
    const auto maskY = yy - clip.y();
    std::size_t xx = clip.x();
    while (xx < maxX) {
      const auto maskX = xx - clip.x();
      const auto remaining = maxX - xx;
      const auto count = std::min<std::size_t>(remaining, 2u);

      const auto c0 = maskData[maskY * mask.width() + maskX];
      const auto c1 = (count > 1 && maskX + 1 < mask.width())
                          ? maskData[maskY * mask.width() + maskX + 1]
                          : static_cast<std::uint8_t>(0u);

      if (c0 == 0u && c1 == 0u) {
        xx += count;
        continue;
      }

      const auto chunkWidth = static_cast<std::uint32_t>(count);
      const auto chunkRect = ScreenIntRect::fromXYWH(
          static_cast<std::uint32_t>(xx), static_cast<std::uint32_t>(yy), chunkWidth, 1u);
      if (!chunkRect.has_value()) {
        xx += count;
        continue;
      }

      const AAMaskCtx aaMaskCtx{
          {c0, c1},
          chunkWidth,
          static_cast<std::size_t>(chunkRect->x() + chunkRect->y() * chunkWidth)};
      blitMaskRp_.run(*chunkRect, aaMaskCtx, maskCtxExternal, pixmapSrcRef, pixmap_);
      xx += count;
    }
  }
}

}  // namespace tiny_skia::pipeline
