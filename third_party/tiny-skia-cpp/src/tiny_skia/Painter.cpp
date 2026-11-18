#include "tiny_skia/Painter.h"

#include <algorithm>
#include <array>
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

/// Returns the blitter the scan converter should be driven with: the wrapper's substitute when
/// one was supplied, otherwise the pipeline blitter itself.
Blitter& selectBlitter(BlitterWrapper* wrapper, pipeline::RasterPipelineBlitter& blitter,
                       const Paint& paint) {
  return wrapper != nullptr ? wrapper->wrap(blitter, paint) : static_cast<Blitter&>(blitter);
}

/// A single closed rectangular contour, in either traversal direction.
std::optional<Rect> closedRectangle(const Path& path) {
  constexpr std::array kRectangleVerbs{PathVerb::Move, PathVerb::Line, PathVerb::Line,
                                       PathVerb::Line, PathVerb::Close};
  const auto verbs = path.verbs();
  const auto points = path.points();
  if (points.size() != 4 ||
      !std::equal(verbs.begin(), verbs.end(), kRectangleVerbs.begin(), kRectangleVerbs.end())) {
    return std::nullopt;
  }

  const Point a = points[0];
  const Point b = points[1];
  const Point c = points[2];
  const Point d = points[3];
  const bool horizontalFirst = a.y == b.y && b.x == c.x && c.y == d.y && d.x == a.x;
  const bool verticalFirst = a.x == b.x && b.y == c.y && c.x == d.x && d.y == a.y;
  if (!(horizontalFirst || verticalFirst)) {
    return std::nullopt;
  }
  return Rect::fromLTRB(std::min(a.x, c.x), std::min(a.y, c.y), std::max(a.x, c.x),
                        std::max(a.y, c.y));
}

std::optional<Rect> mapRectangle(float left, float top, float right, float bottom,
                                 Transform transform) {
  if (!(right > left && bottom > top)) {
    return std::nullopt;
  }
  std::array points{Point::fromXY(left, top), Point::fromXY(right, bottom)};
  transform.mapPoints(points);
  return Rect::fromLTRB(points[0].x, points[0].y, points[1].x, points[1].y);
}

/// Length of a one-dimensional pixel intersected with an axis-aligned interval.
double pixelOverlap(std::uint32_t pixel, double first, double last) {
  const double edge = static_cast<double>(pixel);
  return std::max(0.0, std::min(edge + 1.0, last) - std::max(edge, first));
}

std::array<std::uint32_t, 8> axisCoverageEdges(float outerFirst, float outerLast, float innerFirst,
                                               float innerLast, std::uint32_t extent) {
  const auto clip = [extent](double edge) {
    return static_cast<std::uint32_t>(std::clamp(edge, 0.0, static_cast<double>(extent)));
  };
  std::array<std::uint32_t, 8> edges{};
  std::size_t count = 0;
  for (float edge : {outerFirst, outerLast, innerFirst, innerLast}) {
    edges[count++] = clip(std::floor(edge));
    edges[count++] = clip(std::ceil(edge));
  }
  std::sort(edges.begin(), edges.end());
  return edges;
}

void blitRectangleRing(const Rect& outer, const Rect& inner, std::uint32_t width,
                       std::uint32_t height, Blitter& blitter) {
  const auto xEdges =
      axisCoverageEdges(outer.left(), outer.right(), inner.left(), inner.right(), width);
  const auto yEdges =
      axisCoverageEdges(outer.top(), outer.bottom(), inner.top(), inner.bottom(), height);
  if (xEdges.front() == xEdges.back() || yEdges.front() == yEdges.back()) {
    return;
  }

  // The area is constant inside a block because the fractional edge cells make their own bands.
  // Blit each nonempty block once. This avoids scanning either the filled interior or the hole.
  std::optional<AlphaRuns> row;
  for (std::size_t iy = 1; iy < yEdges.size(); ++iy) {
    const auto y = yEdges[iy - 1];
    const auto rows = yEdges[iy] - y;
    if (rows == 0) {
      continue;
    }
    const double outerY = pixelOverlap(y, outer.top(), outer.bottom());
    const double innerY = pixelOverlap(y, inner.top(), inner.bottom());
    for (std::size_t ix = 1; ix < xEdges.size(); ++ix) {
      const auto x = xEdges[ix - 1];
      const auto cols = xEdges[ix] - x;
      if (cols == 0) {
        continue;
      }
      const double outsideArea = outerY * pixelOverlap(x, outer.left(), outer.right());
      const double insideArea = innerY * pixelOverlap(x, inner.left(), inner.right());
      const auto coverage =
          static_cast<AlphaU8>(std::lround(255.0 * std::clamp(outsideArea - insideArea, 0.0, 1.0)));
      if (coverage == 0) {
        continue;
      }
      if (coverage == 255) {
        blitter.blitRect(ScreenIntRect::fromXYWHSafe(x, y, cols, rows));
      } else if (cols == 1) {
        blitter.blitV(x, y, rows, coverage);
      } else {
        if (row) {
          row->reset(cols);
        } else {
          row.emplace(cols);
        }
        row->alpha[0] = coverage;
        for (std::uint32_t dy = 0; dy < rows; ++dy) {
          blitter.blitAntiH(x, y + dy, row->alpha, row->runs);
        }
      }
    }
  }
}

bool tryStrokeRectangleRing(MutablePixmapView& pixmap, const Path& path, const Paint& paint,
                            const Stroke& stroke, Transform transform, const Mask* mask,
                            BlitterWrapper* wrapper) {
  constexpr float kRightAngleMiterRatio = 1.4142135623730951f;
  if (!paint.antiAlias || stroke.dash || stroke.lineJoin != LineJoin::Miter ||
      !std::isfinite(stroke.miterLimit) || stroke.miterLimit < kRightAngleMiterRatio ||
      !transform.isFinite() || transform.kx != 0.0f || transform.ky != 0.0f ||
      transform.sx <= 0.0f || transform.sy <= 0.0f ||
      detail::DrawTiler::required(pixmap.width(), pixmap.height())) {
    return false;
  }

  const auto rect = closedRectangle(path);
  if (!rect) {
    return false;
  }
  const float half = stroke.width * 0.5f;
  const auto outer = mapRectangle(rect->left() - half, rect->top() - half, rect->right() + half,
                                  rect->bottom() + half, transform);
  const auto inner = mapRectangle(rect->left() + half, rect->top() + half, rect->right() - half,
                                  rect->bottom() - half, transform);
  if (!outer || !inner) {
    return false;
  }

  auto paintCopy = paint;
  if (!transform.isIdentity()) {
    transformShader(paintCopy.shader, transform);
  }
  auto subpix = pixmap.subpixmap();
  auto submask = mask ? std::optional<SubMaskView>(mask->submask()) : std::nullopt;
  auto blitter = pipeline::RasterPipelineBlitter::create(paintCopy, submask, &subpix);
  if (blitter) {
    blitRectangleRing(*outer, *inner, pixmap.width(), pixmap.height(),
                      selectBlitter(wrapper, *blitter, paintCopy));
  }
  return true;
}

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
                       Transform transform, const Mask* mask, BlitterWrapper* wrapper) {
  if (transform.isIdentity() && !detail::DrawTiler::required(pixmap.width(), pixmap.height())) {
    const auto clip = pixmap.size().toScreenIntRect(0, 0);

    auto submaskOpt = mask ? std::optional<SubMaskView>(mask->submask()) : std::nullopt;
    auto subpix = pixmap.subpixmap();
    auto blitter = pipeline::RasterPipelineBlitter::create(paint, submaskOpt, &subpix);
    if (!blitter.has_value()) {
      return;
    }

    Blitter& target = selectBlitter(wrapper, *blitter, paint);
    if (paint.antiAlias) {
      scan::fillRectAa(rect, clip, target);
    } else {
      scan::fillRect(rect, clip, target);
    }
  } else {
    const auto path = Path::fromRect(rect);
    Painter::fillPath(pixmap, path, paint, FillRule::Winding, transform, mask, wrapper);
  }
}

void Painter::fillPath(MutablePixmapView& pixmap, const Path& path, const Paint& paint,
                       FillRule fillRule, Transform transform, const Mask* mask,
                       BlitterWrapper* wrapper) {
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

        Blitter& target = selectBlitter(wrapper, *blitter, paintCopy);
        if (paintCopy.antiAlias) {
          scan::path_aa::fillPath(pathCopy, fillRule, clipRect, target);
        } else {
          scan::fillPath(pathCopy, fillRule, clipRect, target);
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

      Blitter& target = selectBlitter(wrapper, *blitter, paint);
      if (paint.antiAlias) {
        scan::path_aa::fillPath(path, fillRule, clipRect, target);
      } else {
        scan::fillPath(path, fillRule, clipRect, target);
      }
    }
  } else {
    auto transformed = path.transform(transform);
    if (!transformed.has_value()) {
      return;
    }

    auto paintCopy = paint;
    transformShader(paintCopy.shader, transform);

    Painter::fillPath(pixmap, *transformed, paintCopy, fillRule, Transform::identity(), mask,
                      wrapper);
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
  paint.forceHqPipeline = ppaint.forceHqPipeline;
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
                         const Stroke& stroke, Transform transform, const Mask* mask,
                         BlitterWrapper* wrapper, StrokeOutputLimit* limit) {
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

        strokeHairline(pathCopy, paintCopy, stroke.lineCap, submaskOpt, *subpix, wrapper);

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
        strokeHairline(*transformed, paintCopy, stroke.lineCap, submaskOpt, subpix, wrapper);
      } else {
        strokeHairline(*pathPtr, paintCopy, stroke.lineCap, submaskOpt, subpix, wrapper);
      }
    }
  } else {
    // Thick stroke: stroke the path into a filled outline, then fill it.
    auto strokedPath = pathPtr->stroke(stroke, resScale, limit);
    if (!strokedPath.has_value()) {
      return;
    }
    if (tryStrokeRectangleRing(pixmap, path, paint, stroke, transform, mask, wrapper)) {
      return;
    }
    Painter::fillPath(pixmap, *strokedPath, paint, FillRule::Winding, transform, mask, wrapper);
  }
}

void Painter::strokeHairline(const Path& path, const Paint& paint, LineCap lineCap,
                             std::optional<SubMaskView> mask, MutableSubPixmapView& subpix,
                             BlitterWrapper* wrapper) {
  const auto clip = subpix.size.toScreenIntRect(0, 0);
  auto blitter = pipeline::RasterPipelineBlitter::create(paint, mask, &subpix);
  if (!blitter.has_value()) {
    return;
  }

  Blitter& target = selectBlitter(wrapper, *blitter, paint);
  if (paint.antiAlias) {
    scan::hairline_aa::strokePath(path, lineCap, clip, target);
  } else {
    scan::strokePath(path, lineCap, clip, target);
  }
}

}  // namespace tiny_skia
