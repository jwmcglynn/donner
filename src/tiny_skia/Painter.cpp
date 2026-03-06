#include "tiny_skia/Painter.h"

#include <cmath>
#include <limits>

#include "tiny_skia/Math.h"
#include "tiny_skia/Stroker.h"
#include "tiny_skia/pipeline/Blitter.h"
#include "tiny_skia/scan/Hairline.h"
#include "tiny_skia/scan/HairlineAa.h"
#include "tiny_skia/scan/Scan.h"
#include "tiny_skia/scan/Path.h"
#include "tiny_skia/scan/PathAa.h"

namespace tiny_skia {

namespace {

/// Maximum float scalar value.
constexpr float kScalarMax = std::numeric_limits<float>::max();

}  // namespace

bool detail::isTooBigForMath(const Path& path) {
  constexpr float kScaleDownToAllowForSmallMultiplies = 0.25f;
  constexpr float kMax = kScalarMax * kScaleDownToAllowForSmallMultiplies;

  const auto b = path.bounds();

  // Use ! expression so we return true if bounds contains NaN.
  return !(b.left() >= -kMax && b.top() >= -kMax && b.right() <= kMax && b.bottom() <= kMax);
}

std::optional<float> detail::treatAsHairline(const Paint& paint, float strokeWidth, Transform ts) {
  if (strokeWidth == 0.0f) {
    return 1.0f;
  }

  if (!paint.antiAlias) {
    return std::nullopt;
  }

  // We don't care about translate.
  ts.tx = 0.0f;
  ts.ty = 0.0f;

  // Map the stroke width through the transform to check actual pixel width.
  Point points[2] = {Point::fromXY(strokeWidth, 0.0f), Point::fromXY(0.0f, strokeWidth)};
  ts.mapPoints(points);

  // fastLen: approximation of vector length.
  auto fastLen = [](Point p) -> float {
    float ax = std::abs(p.x);
    float ay = std::abs(p.y);
    if (ax < ay) {
      std::swap(ax, ay);
    }
    return ax + ay * 0.5f;
  };

  const float len0 = fastLen(points[0]);
  const float len1 = fastLen(points[1]);

  if (len0 <= 1.0f && len1 <= 1.0f) {
    return (len0 + len1) * 0.5f;
  }

  return std::nullopt;
}

void Painter::fillRect(MutablePixmapView& pixmap, const Rect& rect, const Paint& paint,
                       Transform transform, const Mask* mask) {
  if (transform.isIdentity() && !detail::DrawTiler::required(pixmap.width(), pixmap.height())) {
    const auto clip = pixmap.size().toScreenIntRect(0, 0);

    auto submaskOpt = mask ? std::optional<SubMaskView>(mask->submask()) : std::nullopt;
    auto subpix = pixmap.subpixmap();
    auto blitter = pipeline::RasterPipelineBlitter::create(paint, submaskOpt, &subpix);
    if (!blitter.has_value()) {
      return;
    }

    if (paint.antiAlias) {
      scan::fillRectAa(rect, clip, *blitter);
    } else {
      scan::fillRect(rect, clip, *blitter);
    }
  } else {
    const auto path = Path::fromRect(rect);
    Painter::fillPath(pixmap, path, paint, FillRule::Winding, transform, mask);
  }
}

void Painter::fillPath(MutablePixmapView& pixmap, const Path& path, const Paint& paint,
                       FillRule fillRule, Transform transform, const Mask* mask) {
  if (transform.isIdentity()) {
    // Skip empty paths and horizontal/vertical lines.
    const auto pathBounds = path.bounds();
    if (isNearlyZero(pathBounds.width()) || isNearlyZero(pathBounds.height())) {
      return;
    }

    if (detail::isTooBigForMath(path)) {
      return;
    }

    if (auto tiler = detail::DrawTiler::create(pixmap.width(), pixmap.height())) {
      auto pathCopy = path;
      auto paintCopy = paint;

      while (auto tile = tiler->next()) {
        const auto ts = Transform::fromTranslate(-static_cast<float>(tile->x()),
                                                 -static_cast<float>(tile->y()));
        auto transformed = pathCopy.transform(ts);
        if (!transformed.has_value()) {
          return;
        }
        pathCopy = std::move(*transformed);
        transformShader(paintCopy.shader, ts);

        const auto clipRect = tile->size().toScreenIntRect(0, 0);
        auto subpix = pixmap.subpixmap(tile->toIntRect());
        if (!subpix.has_value()) {
          continue;
        }

        auto submaskOpt = mask ? mask->submask(tile->toIntRect()) : std::nullopt;
        auto blitter = pipeline::RasterPipelineBlitter::create(paintCopy, submaskOpt, &(*subpix));
        if (!blitter.has_value()) {
          continue;
        }

        if (paintCopy.antiAlias) {
          scan::path_aa::fillPath(pathCopy, fillRule, clipRect, *blitter);
        } else {
          scan::fillPath(pathCopy, fillRule, clipRect, *blitter);
        }

        const auto tsBack =
            Transform::fromTranslate(static_cast<float>(tile->x()), static_cast<float>(tile->y()));
        auto untransformed = pathCopy.transform(tsBack);
        if (!untransformed.has_value()) {
          return;
        }
        pathCopy = std::move(*untransformed);
        transformShader(paintCopy.shader, tsBack);
      }
    } else {
      const auto clipRect = pixmap.size().toScreenIntRect(0, 0);
      auto submaskOpt = mask ? std::optional<SubMaskView>(mask->submask()) : std::nullopt;
      auto subpix = pixmap.subpixmap();
      auto blitter = pipeline::RasterPipelineBlitter::create(paint, submaskOpt, &subpix);
      if (!blitter.has_value()) {
        return;
      }

      if (paint.antiAlias) {
        scan::path_aa::fillPath(path, fillRule, clipRect, *blitter);
      } else {
        scan::fillPath(path, fillRule, clipRect, *blitter);
      }
    }
  } else {
    auto transformed = path.transform(transform);
    if (!transformed.has_value()) {
      return;
    }

    auto paintCopy = paint;
    transformShader(paintCopy.shader, transform);

    Painter::fillPath(pixmap, *transformed, paintCopy, fillRule, Transform::identity(), mask);
  }
}

void Painter::drawPixmap(MutablePixmapView& pixmap, std::int32_t x, std::int32_t y, PixmapView src,
                         const PixmapPaint& ppaint, Transform transform, const Mask* mask) {
  const auto intRect = src.size().toIntRect(x, y);
  if (!intRect.has_value()) {
    return;
  }
  const auto screenRect = intRect->toScreenIntRect();
  if (!screenRect.has_value()) {
    return;
  }
  const auto rect = screenRect->toRect();

  // Translate pattern as well as bounds.
  const auto pattTransform = Transform::fromTranslate(static_cast<float>(x), static_cast<float>(y));

  Paint paint;
  paint.shader = Pattern(src, SpreadMode::Pad, ppaint.quality, ppaint.opacity, pattTransform);
  paint.blendMode = ppaint.blendMode;
  paint.antiAlias = false;
  paint.forceHqPipeline = false;
  paint.colorspace = ColorSpace::Linear;

  Painter::fillRect(pixmap, rect, paint, transform, mask);
}

void Painter::applyMask(MutablePixmapView& pixmap, const Mask& mask) {
  if (pixmap.size() != mask.size()) {
    return;
  }

  // Dummy source pixmap.
  auto pixmapSrc = PixmapView::fromBytes(std::span<const std::uint8_t>({0, 0, 0, 0}), 1, 1);
  if (!pixmapSrc.has_value()) {
    return;
  }

  pipeline::RasterPipelineBuilder p;
  p.push(pipeline::Stage::LoadMaskU8);
  p.push(pipeline::Stage::LoadDestination);
  p.push(pipeline::Stage::DestinationIn);
  p.push(pipeline::Stage::Store);
  auto rp = p.compile();

  const auto rect = pixmap.size().toScreenIntRect(0, 0);
  auto subpix = pixmap.subpixmap();
  const auto submask = mask.submask();
  const auto maskCtx = pipeline::MaskCtx{submask.data, submask.realWidth};
  rp.run(rect, pipeline::AAMaskCtx{}, maskCtx, *pixmapSrc, &subpix);
}

void Painter::strokePath(MutablePixmapView& pixmap, const Path& path, const Paint& paint,
                         const Stroke& stroke, Transform transform, const Mask* mask) {
  if (stroke.width < 0.0f) {
    return;
  }

  float resScale = PathStroker::computeResolutionScale(transform);

  // Apply dashing if needed.
  std::optional<Path> dashPath;
  const Path* pathPtr = &path;
  if (stroke.dash.has_value()) {
    dashPath = path.dash(*stroke.dash, resScale);
    if (!dashPath.has_value()) {
      return;
    }
    pathPtr = &(*dashPath);
  }

  auto coverage = detail::treatAsHairline(paint, stroke.width, transform);
  if (coverage.has_value()) {
    // Hairline path.
    auto paintCopy = paint;
    if (*coverage == 1.0f) {
      // No changes to paint.
    } else if (shouldPreScaleCoverage(paintCopy.blendMode)) {
      auto scale = static_cast<std::int32_t>(*coverage * 256.0f);
      auto newAlpha = (255 * scale) >> 8;
      applyShaderOpacity(paintCopy.shader, static_cast<float>(newAlpha) / 255.0f);
    }

    if (auto tiler = detail::DrawTiler::create(pixmap.width(), pixmap.height())) {
      auto pathCopy = *pathPtr;

      if (!transform.isIdentity()) {
        transformShader(paintCopy.shader, transform);
        auto transformed = pathCopy.transform(transform);
        if (!transformed.has_value()) {
          return;
        }
        pathCopy = std::move(*transformed);
      }

      while (auto tile = tiler->next()) {
        const auto ts = Transform::fromTranslate(-static_cast<float>(tile->x()),
                                                 -static_cast<float>(tile->y()));
        auto transformed = pathCopy.transform(ts);
        if (!transformed.has_value()) {
          return;
        }
        pathCopy = std::move(*transformed);
        transformShader(paintCopy.shader, ts);

        auto subpix = pixmap.subpixmap(tile->toIntRect());
        if (!subpix.has_value()) {
          continue;
        }
        auto submaskOpt = mask ? mask->submask(tile->toIntRect()) : std::nullopt;

        strokeHairline(pathCopy, paintCopy, stroke.lineCap, submaskOpt, *subpix);

        const auto tsBack =
            Transform::fromTranslate(static_cast<float>(tile->x()), static_cast<float>(tile->y()));
        auto untransformed = pathCopy.transform(tsBack);
        if (!untransformed.has_value()) {
          return;
        }
        pathCopy = std::move(*untransformed);
        transformShader(paintCopy.shader, tsBack);
      }
    } else {
      auto subpix = pixmap.subpixmap();
      auto submaskOpt = mask ? std::optional<SubMaskView>(mask->submask()) : std::nullopt;
      if (!transform.isIdentity()) {
        transformShader(paintCopy.shader, transform);
        auto transformed = pathPtr->transform(transform);
        if (!transformed.has_value()) {
          return;
        }
        strokeHairline(*transformed, paintCopy, stroke.lineCap, submaskOpt, subpix);
      } else {
        strokeHairline(*pathPtr, paintCopy, stroke.lineCap, submaskOpt, subpix);
      }
    }
  } else {
    // Thick stroke: stroke the path into a filled outline, then fill it.
    auto strokedPath = pathPtr->stroke(stroke, resScale);
    if (!strokedPath.has_value()) {
      return;
    }
    Painter::fillPath(pixmap, *strokedPath, paint, FillRule::Winding, transform, mask);
  }
}

void Painter::strokeHairline(const Path& path, const Paint& paint, LineCap lineCap,
                             std::optional<SubMaskView> mask, MutableSubPixmapView& subpix) {
  const auto clip = subpix.size.toScreenIntRect(0, 0);
  auto blitter = pipeline::RasterPipelineBlitter::create(paint, mask, &subpix);
  if (!blitter.has_value()) {
    return;
  }

  if (paint.antiAlias) {
    scan::hairline_aa::strokePath(path, lineCap, clip, *blitter);
  } else {
    scan::strokePath(path, lineCap, clip, *blitter);
  }
}

}  // namespace tiny_skia
