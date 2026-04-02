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
                                             std::optional<PremultipliedColorU8> solidSrcOverColor,
                                             std::optional<SubMaskView> mask,
                                             Pixmap pixmapSrcStorage, RasterPipeline blitAntiHRp,
                                             RasterPipeline blitRectRp, RasterPipeline blitMaskRp)
    : pixmap_(pixmap),
      isMaskOnly_(isMaskOnly),
      memsetColor_(memsetColor),
      solidSrcOverColor_(solidSrcOverColor),
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

  // For solid SourceOver (including strength-reduced to Source), enable inline blend fast path.
  std::optional<PremultipliedColorU8> solidSrcOverColor;
  if (!mask.has_value() && (blendMode == BlendMode::Source || blendMode == BlendMode::SourceOver)) {
    solidSrcOverColor = color;
  }

  return RasterPipelineBlitter(pixmap, false, memsetColor, solidSrcOverColor, mask,
                               makeDummyPixmapSrc(), blitAntiHRp,
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
  // Disable memset when unpremulStore: the stored format is straight alpha, not premultiplied.
  std::optional<PremultipliedColorU8> memsetColor;
  if (!paint.unpremulStore && paint.isSolidColor() && blendMode == BlendMode::Source &&
      !mask.has_value()) {
    const auto& color = std::get<Color>(paint.shader);
    memsetColor = color.premultiply().toColorU8();
  }

  // Clear is just a transparent color memset (when not anti-aliased and no mask).
  if (blendMode == BlendMode::Clear && !paint.antiAlias && !mask.has_value()) {
    blendMode = BlendMode::Source;
    if (!paint.unpremulStore) {
      memsetColor = PremultipliedColorU8::fromRgbaUnchecked(0, 0, 0, 0);
    }
  }

  const bool unpremul = paint.unpremulStore;

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
      if (unpremul) {
        p.push(Stage::PremultiplyDestination);
      }
      if (const auto stage = expandDestStage(paint.colorspace)) {
        p.push(*stage);
      }
      if (const auto stage = toStage(blendMode)) {
        p.push(*stage);
      }
    } else {
      p.push(Stage::LoadDestination);
      if (unpremul) {
        p.push(Stage::PremultiplyDestination);
      }
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
    if (unpremul) {
      p.push(Stage::Unpremultiply);
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
    // SourceOverRgba is a combined lowp fast path; skip it when unpremulStore is set.
    if (blendMode == BlendMode::SourceOver && !mask.has_value() && !unpremul) {
      if (const auto stage = compressStage(paint.colorspace)) {
        p.push(*stage);
      }
      p.push(Stage::SourceOverRgba);
    } else {
      if (blendMode != BlendMode::Source) {
        p.push(Stage::LoadDestination);
        if (unpremul) {
          p.push(Stage::PremultiplyDestination);
        }
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
      if (unpremul) {
        p.push(Stage::Unpremultiply);
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
      if (unpremul) {
        p.push(Stage::PremultiplyDestination);
      }
      if (const auto stage = expandDestStage(paint.colorspace)) {
        p.push(*stage);
      }
      if (const auto stage = toStage(blendMode)) {
        p.push(*stage);
      }
    } else {
      p.push(Stage::LoadDestination);
      if (unpremul) {
        p.push(Stage::PremultiplyDestination);
      }
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
    if (unpremul) {
      p.push(Stage::Unpremultiply);
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

  // For solid color SourceOver (or strength-reduced to Source), enable inline blend fast path.
  std::optional<PremultipliedColorU8> solidSrcOverColor;
  if (paint.isSolidColor() && !mask.has_value() && !unpremul &&
      (blendMode == BlendMode::Source || blendMode == BlendMode::SourceOver)) {
    const auto& color = std::get<Color>(paint.shader);
    solidSrcOverColor = color.premultiply().toColorU8();
  }

  return RasterPipelineBlitter(pixmap, false, memsetColor, solidSrcOverColor, mask,
                               std::move(pixmapSrcStorage), *blitAntiHRp, *blitRectRp,
                               *blitMaskRp);
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

  return RasterPipelineBlitter(pixmap, true, memsetColor, std::nullopt, std::nullopt,
                               makeDummyPixmapSrc(), blitAntiHRp, blitRectRp, blitMaskRp);
}

void RasterPipelineBlitter::blitH(std::uint32_t x, std::uint32_t y, LengthU32 width) {
  blitRect(ScreenIntRect::fromXYWHSafe(x, y, width, 1u));
}

void RasterPipelineBlitter::blitAntiH(std::uint32_t x, std::uint32_t y,
                                      std::span<std::uint8_t> alpha, std::span<AlphaRun> runs) {
  if (pixmap_ == nullptr || alpha.empty() || runs.empty()) {
    return;
  }

  // Inline pattern blend fast path: bypass pipeline for identity+repeat+nearest patterns.
  const auto& patCtx = blitAntiHRp_.ctx().fusedBilinearPattern;
  if (patCtx.fuseSourceOverCoverage && patCtx.pixels != nullptr && patCtx.useNearest &&
      patCtx.spreadMode == SpreadMode::Repeat && !mask_.has_value() && patCtx.opacity >= 1.0f &&
      patCtx.sx == 1.0f && patCtx.kx == 0.0f && patCtx.ky == 0.0f) {
    const std::uint32_t tw = patCtx.width;
    const std::uint32_t th = patCtx.height;
    const float fw = static_cast<float>(tw);
    const float fh = static_cast<float>(th);

    // Compute tile Y (constant for entire scanline).
    float cyRaw = (static_cast<float>(y) + 0.5f) * patCtx.sy + patCtx.ty;
    cyRaw = cyRaw - std::floor(cyRaw * patCtx.invHeight) * fh;
    if (cyRaw < 0.0f) cyRaw += fh;
    const std::uint32_t tileY = std::min(static_cast<std::uint32_t>(static_cast<std::int32_t>(cyRaw)),
                                         th - 1);
    const std::uint8_t* tileRow = patCtx.pixels + static_cast<std::size_t>(tileY) * tw * 4;
    auto* data = pixmap_->data;
    const auto rw = pixmap_->realWidth;

    auto div255 = [](std::uint32_t v) -> std::uint8_t {
      return static_cast<std::uint8_t>((v + 128 + ((v + 128) >> 8)) >> 8);
    };

    std::size_t aaOffset = 0;
    std::size_t runOffset = 0;
    while (runOffset < runs.size() && runs[runOffset].has_value()) {
      const auto run = static_cast<LengthU32>(runs[runOffset].value());
      if (run == 0u || aaOffset >= alpha.size()) break;
      const auto cov = alpha[aaOffset];
      if (cov == 255u) {
        blitH(x, y, run);
      } else if (cov != 0u) {
        for (std::uint32_t i = 0; i < run; ++i) {
          const auto px = x + i;
          // Compute tile X.
          float cxRaw = static_cast<float>(px) + 0.5f + patCtx.tx;
          cxRaw = cxRaw - std::floor(cxRaw * patCtx.invWidth) * fw;
          if (cxRaw < 0.0f) cxRaw += fw;
          const std::uint32_t tileX = std::min(
              static_cast<std::uint32_t>(static_cast<std::int32_t>(cxRaw)), tw - 1);
          const std::uint8_t* sp = tileRow + tileX * 4;
          // Scale source by coverage, then SourceOver blend.
          std::uint32_t sr = div255(static_cast<std::uint32_t>(sp[0]) * cov);
          std::uint32_t sg = div255(static_cast<std::uint32_t>(sp[1]) * cov);
          std::uint32_t sb = div255(static_cast<std::uint32_t>(sp[2]) * cov);
          std::uint32_t sa = div255(static_cast<std::uint32_t>(sp[3]) * cov);
          std::uint32_t invSa = 255 - sa;
          const auto offset = (static_cast<std::size_t>(y) * rw + px) * kBytesPerPixel;
          data[offset + 0] = static_cast<std::uint8_t>(
              sr + div255(static_cast<std::uint32_t>(data[offset + 0]) * invSa));
          data[offset + 1] = static_cast<std::uint8_t>(
              sg + div255(static_cast<std::uint32_t>(data[offset + 1]) * invSa));
          data[offset + 2] = static_cast<std::uint8_t>(
              sb + div255(static_cast<std::uint32_t>(data[offset + 2]) * invSa));
          data[offset + 3] = static_cast<std::uint8_t>(
              sa + div255(static_cast<std::uint32_t>(data[offset + 3]) * invSa));
        }
      }
      x += run;
      runOffset += static_cast<std::size_t>(run);
      aaOffset += static_cast<std::size_t>(run);
    }
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

  // Fast path for opaque Source: simple lerp.
  if (memsetColor_.has_value() && !isMaskOnly_ && pixmap_ != nullptr && x < pixmap_->width()) {
    const auto c = *memsetColor_;
    auto* data = pixmap_->data;
    const auto rw = pixmap_->realWidth;
    const auto maxY = std::min(static_cast<std::size_t>(y) + height, pixmap_->height());
    if (alpha == 255) {
      for (auto yy = static_cast<std::size_t>(y); yy < maxY; ++yy) {
        const auto offset = (yy * rw + x) * kBytesPerPixel;
        data[offset + 0] = c.red();
        data[offset + 1] = c.green();
        data[offset + 2] = c.blue();
        data[offset + 3] = c.alpha();
      }
    } else {
      const uint32_t inv = 255 - alpha;
      auto blend = [](uint32_t src, uint32_t dst, uint32_t a, uint32_t invA) {
        uint32_t t = src * a + dst * invA + 128;
        return static_cast<std::uint8_t>((t + (t >> 8)) >> 8);
      };
      for (auto yy = static_cast<std::size_t>(y); yy < maxY; ++yy) {
        const auto offset = (yy * rw + x) * kBytesPerPixel;
        data[offset + 0] = blend(c.red(), data[offset + 0], alpha, inv);
        data[offset + 1] = blend(c.green(), data[offset + 1], alpha, inv);
        data[offset + 2] = blend(c.blue(), data[offset + 2], alpha, inv);
        data[offset + 3] = blend(c.alpha(), data[offset + 3], alpha, inv);
      }
    }
    return;
  }
  // Fast path for semi-transparent SourceOver.
  if (solidSrcOverColor_.has_value() && pixmap_ != nullptr && x < pixmap_->width()) {
    const auto c = *solidSrcOverColor_;
    auto* data = pixmap_->data;
    const auto rw = pixmap_->realWidth;
    const auto maxY = std::min(static_cast<std::size_t>(y) + height, pixmap_->height());
    auto div255 = [](uint32_t v) -> std::uint8_t {
      return static_cast<std::uint8_t>((v + 128 + ((v + 128) >> 8)) >> 8);
    };
    uint32_t sa = div255(static_cast<uint32_t>(c.alpha()) * alpha);
    uint32_t inv_sa = 255 - sa;
    uint32_t sr = div255(static_cast<uint32_t>(c.red()) * alpha);
    uint32_t sg = div255(static_cast<uint32_t>(c.green()) * alpha);
    uint32_t sb = div255(static_cast<uint32_t>(c.blue()) * alpha);
    for (auto yy = static_cast<std::size_t>(y); yy < maxY; ++yy) {
      const auto offset = (yy * rw + x) * kBytesPerPixel;
      data[offset + 0] =
          static_cast<std::uint8_t>(sr + div255(static_cast<uint32_t>(data[offset + 0]) * inv_sa));
      data[offset + 1] =
          static_cast<std::uint8_t>(sg + div255(static_cast<uint32_t>(data[offset + 1]) * inv_sa));
      data[offset + 2] =
          static_cast<std::uint8_t>(sb + div255(static_cast<uint32_t>(data[offset + 2]) * inv_sa));
      data[offset + 3] =
          static_cast<std::uint8_t>(sa + div255(static_cast<uint32_t>(data[offset + 3]) * inv_sa));
    }
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
  // Inline pattern blend fast path: bypass pipeline for identity+repeat+nearest patterns.
  {
    const auto& patCtx = blitMaskRp_.ctx().fusedBilinearPattern;
    if (patCtx.pixels != nullptr && patCtx.useNearest && patCtx.width > 0 && patCtx.height > 0 &&
        patCtx.spreadMode == SpreadMode::Repeat && !mask_.has_value() && patCtx.opacity >= 1.0f &&
        patCtx.sx == 1.0f && patCtx.kx == 0.0f && patCtx.ky == 0.0f && pixmap_ != nullptr) {
      const std::uint32_t tw = patCtx.width;
      const std::uint32_t th = patCtx.height;
      const float fw = static_cast<float>(tw);
      const float fh = static_cast<float>(th);

      float cyRaw = (static_cast<float>(y) + 0.5f) * patCtx.sy + patCtx.ty;
      cyRaw = cyRaw - std::floor(cyRaw * patCtx.invHeight) * fh;
      if (cyRaw < 0.0f) cyRaw += fh;
      const std::uint32_t tileY =
          std::min(static_cast<std::uint32_t>(static_cast<std::int32_t>(cyRaw)), th - 1);
      const std::uint8_t* tileRow = patCtx.pixels + static_cast<std::size_t>(tileY) * tw * 4;
      auto* data = pixmap_->data;
      const auto rw = pixmap_->realWidth;

      auto div255 = [](std::uint32_t v) -> std::uint8_t {
        return static_cast<std::uint8_t>((v + 128 + ((v + 128) >> 8)) >> 8);
      };

      const AlphaU8 alphas[2] = {alpha0, alpha1};
      for (int i = 0; i < 2; ++i) {
        const auto px = x + static_cast<std::uint32_t>(i);
        const auto cov = alphas[i];
        if (cov == 0 || px >= pixmap_->width() || y >= pixmap_->height()) continue;

        float cxRaw = static_cast<float>(px) + 0.5f + patCtx.tx;
        cxRaw = cxRaw - std::floor(cxRaw * patCtx.invWidth) * fw;
        if (cxRaw < 0.0f) cxRaw += fw;
        const std::uint32_t tileX =
            std::min(static_cast<std::uint32_t>(static_cast<std::int32_t>(cxRaw)), tw - 1);
        const std::uint8_t* sp = tileRow + tileX * 4;

        const auto offset = (static_cast<std::size_t>(y) * rw + px) * kBytesPerPixel;
        if (cov == 255) {
          // SourceOver without coverage scaling.
          std::uint32_t sa = sp[3];
          std::uint32_t invSa = 255 - sa;
          data[offset + 0] = static_cast<std::uint8_t>(
              sp[0] + div255(static_cast<std::uint32_t>(data[offset + 0]) * invSa));
          data[offset + 1] = static_cast<std::uint8_t>(
              sp[1] + div255(static_cast<std::uint32_t>(data[offset + 1]) * invSa));
          data[offset + 2] = static_cast<std::uint8_t>(
              sp[2] + div255(static_cast<std::uint32_t>(data[offset + 2]) * invSa));
          data[offset + 3] = static_cast<std::uint8_t>(
              sa + div255(static_cast<std::uint32_t>(data[offset + 3]) * invSa));
        } else {
          // Scale source by coverage, then SourceOver.
          std::uint32_t sr = div255(static_cast<std::uint32_t>(sp[0]) * cov);
          std::uint32_t sg = div255(static_cast<std::uint32_t>(sp[1]) * cov);
          std::uint32_t sb = div255(static_cast<std::uint32_t>(sp[2]) * cov);
          std::uint32_t sa = div255(static_cast<std::uint32_t>(sp[3]) * cov);
          std::uint32_t invSa = 255 - sa;
          data[offset + 0] = static_cast<std::uint8_t>(
              sr + div255(static_cast<std::uint32_t>(data[offset + 0]) * invSa));
          data[offset + 1] = static_cast<std::uint8_t>(
              sg + div255(static_cast<std::uint32_t>(data[offset + 1]) * invSa));
          data[offset + 2] = static_cast<std::uint8_t>(
              sb + div255(static_cast<std::uint32_t>(data[offset + 2]) * invSa));
          data[offset + 3] = static_cast<std::uint8_t>(
              sa + div255(static_cast<std::uint32_t>(data[offset + 3]) * invSa));
        }
      }
      return;
    }
  }

  // Fast path for opaque Source (strength-reduced from SourceOver): simple lerp.
  if (memsetColor_.has_value() && !isMaskOnly_ && pixmap_ != nullptr) {
    const auto c = *memsetColor_;
    auto* data = pixmap_->data;
    const auto rw = pixmap_->realWidth;
    const AlphaU8 alphas[2] = {alpha0, alpha1};
    for (int i = 0; i < 2; ++i) {
      const auto xx = x + static_cast<std::uint32_t>(i);
      if (xx >= pixmap_->width() || y >= pixmap_->height()) continue;
      const auto a = alphas[i];
      if (a == 0) continue;
      const auto offset = (static_cast<std::size_t>(y) * rw + xx) * kBytesPerPixel;
      if (a == 255) {
        data[offset + 0] = c.red();
        data[offset + 1] = c.green();
        data[offset + 2] = c.blue();
        data[offset + 3] = c.alpha();
      } else {
        const uint32_t inv = 255 - a;
        auto blend = [](uint32_t src, uint32_t dst, uint32_t alpha, uint32_t invAlpha) {
          uint32_t t = src * alpha + dst * invAlpha + 128;
          return static_cast<std::uint8_t>((t + (t >> 8)) >> 8);
        };
        data[offset + 0] = blend(c.red(), data[offset + 0], a, inv);
        data[offset + 1] = blend(c.green(), data[offset + 1], a, inv);
        data[offset + 2] = blend(c.blue(), data[offset + 2], a, inv);
        data[offset + 3] = blend(c.alpha(), data[offset + 3], a, inv);
      }
    }
    return;
  }
  // Fast path for semi-transparent SourceOver: ScaleU8 + SourceOver inline.
  if (solidSrcOverColor_.has_value() && pixmap_ != nullptr) {
    const auto c = *solidSrcOverColor_;
    auto* data = pixmap_->data;
    const auto rw = pixmap_->realWidth;
    auto div255 = [](uint32_t v) -> std::uint8_t {
      return static_cast<std::uint8_t>((v + 128 + ((v + 128) >> 8)) >> 8);
    };
    const AlphaU8 alphas[2] = {alpha0, alpha1};
    for (int i = 0; i < 2; ++i) {
      const auto xx = x + static_cast<std::uint32_t>(i);
      if (xx >= pixmap_->width() || y >= pixmap_->height()) continue;
      const auto cov = alphas[i];
      if (cov == 0) continue;
      const auto offset = (static_cast<std::size_t>(y) * rw + xx) * kBytesPerPixel;
      uint32_t sa = div255(static_cast<uint32_t>(c.alpha()) * cov);
      uint32_t inv_sa = 255 - sa;
      data[offset + 0] = div255(static_cast<uint32_t>(c.red()) * cov) +
                         div255(static_cast<uint32_t>(data[offset + 0]) * inv_sa);
      data[offset + 1] = div255(static_cast<uint32_t>(c.green()) * cov) +
                         div255(static_cast<uint32_t>(data[offset + 1]) * inv_sa);
      data[offset + 2] = div255(static_cast<uint32_t>(c.blue()) * cov) +
                         div255(static_cast<uint32_t>(data[offset + 2]) * inv_sa);
      data[offset + 3] =
          static_cast<std::uint8_t>(sa + div255(static_cast<uint32_t>(data[offset + 3]) * inv_sa));
    }
    return;
  }

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

void RasterPipelineBlitter::blitAntiRect(std::int32_t x, std::int32_t y, std::int32_t width,
                                         std::int32_t height, AlphaU8 leftAlpha,
                                         AlphaU8 rightAlpha) {
  if (height <= 0) return;
  auto uy = static_cast<std::uint32_t>(y);
  auto uh = static_cast<LengthU32>(height);
  // Left edge column: one blitV call for full height.
  if (leftAlpha > 0) {
    blitV(static_cast<std::uint32_t>(x), uy, uh, leftAlpha);
  }
  // Interior: one blitRect call (uses memset fast path for opaque).
  if (width > 0) {
    blitRect(ScreenIntRect::fromXYWHSafe(static_cast<std::uint32_t>(x + 1), uy,
                                         static_cast<LengthU32>(width), uh));
  }
  // Right edge column: one blitV call for full height.
  if (rightAlpha > 0) {
    blitV(static_cast<std::uint32_t>(x + 1 + width), uy, uh, rightAlpha);
  }
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
