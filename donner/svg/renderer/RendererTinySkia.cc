#include "donner/svg/renderer/RendererTinySkia.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/Length.h"
#include "donner/base/MathUtils.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/filter/Blend.h"
#include "tiny_skia/filter/ColorMatrix.h"
#include "tiny_skia/filter/ComponentTransfer.h"
#include "tiny_skia/filter/ColorSpace.h"
#include "tiny_skia/filter/Composite.h"
#include "tiny_skia/filter/ConvolveMatrix.h"
#include "tiny_skia/filter/DisplacementMap.h"
#include "tiny_skia/filter/Flood.h"
#include "tiny_skia/filter/GaussianBlur.h"
#include "tiny_skia/filter/Merge.h"
#include "tiny_skia/filter/Morphology.h"
#include "tiny_skia/filter/Tile.h"
#include "tiny_skia/filter/Turbulence.h"
#include "tiny_skia/filter/Offset.h"
#include "tiny_skia/shaders/Shaders.h"

namespace donner::svg {

namespace {

const Boxd kUnitPathBounds(Vector2d::Zero(), Vector2d(1, 1));

tiny_skia::Color toTinyColor(const css::RGBA& rgba) {
  return tiny_skia::Color::fromRgba8(rgba.r, rgba.g, rgba.b, rgba.a);
}

tiny_skia::Point toTinyPoint(const Vector2d& value) {
  return tiny_skia::Point::fromXY(NarrowToFloat(value.x), NarrowToFloat(value.y));
}

tiny_skia::Transform toTinyTransform(const Transformd& transform) {
  return tiny_skia::Transform::fromRow(
      NarrowToFloat(transform.data[0]), NarrowToFloat(transform.data[1]),
      NarrowToFloat(transform.data[2]), NarrowToFloat(transform.data[3]),
      NarrowToFloat(transform.data[4]), NarrowToFloat(transform.data[5]));
}

std::optional<tiny_skia::Rect> toTinyRect(const Boxd& box) {
  return tiny_skia::Rect::fromLTRB(NarrowToFloat(box.topLeft.x), NarrowToFloat(box.topLeft.y),
                                   NarrowToFloat(box.bottomRight.x),
                                   NarrowToFloat(box.bottomRight.y));
}

tiny_skia::FillRule toTinyFillRule(FillRule fillRule) {
  switch (fillRule) {
    case FillRule::NonZero: return tiny_skia::FillRule::Winding;
    case FillRule::EvenOdd: return tiny_skia::FillRule::EvenOdd;
  }

  UTILS_UNREACHABLE();
}

tiny_skia::LineCap toTinyLineCap(StrokeLinecap lineCap) {
  switch (lineCap) {
    case StrokeLinecap::Butt: return tiny_skia::LineCap::Butt;
    case StrokeLinecap::Round: return tiny_skia::LineCap::Round;
    case StrokeLinecap::Square: return tiny_skia::LineCap::Square;
  }

  UTILS_UNREACHABLE();
}

tiny_skia::LineJoin toTinyLineJoin(StrokeLinejoin lineJoin) {
  switch (lineJoin) {
    case StrokeLinejoin::Miter: return tiny_skia::LineJoin::Miter;
    case StrokeLinejoin::MiterClip: return tiny_skia::LineJoin::MiterClip;
    case StrokeLinejoin::Round: return tiny_skia::LineJoin::Round;
    case StrokeLinejoin::Bevel: return tiny_skia::LineJoin::Bevel;
    case StrokeLinejoin::Arcs: return tiny_skia::LineJoin::Miter;
  }

  UTILS_UNREACHABLE();
}

tiny_skia::SpreadMode toTinySpreadMode(GradientSpreadMethod spreadMethod) {
  switch (spreadMethod) {
    case GradientSpreadMethod::Pad: return tiny_skia::SpreadMode::Pad;
    case GradientSpreadMethod::Reflect: return tiny_skia::SpreadMode::Reflect;
    case GradientSpreadMethod::Repeat: return tiny_skia::SpreadMode::Repeat;
  }

  UTILS_UNREACHABLE();
}

tiny_skia::Path toTinyPath(const PathSpline& spline) {
  tiny_skia::PathBuilder builder(spline.commands().size(), spline.points().size());
  const std::vector<Vector2d>& points = spline.points();

  for (const PathSpline::Command& command : spline.commands()) {
    switch (command.type) {
      case PathSpline::CommandType::MoveTo: {
        const Vector2d& point = points[command.pointIndex];
        builder.moveTo(NarrowToFloat(point.x), NarrowToFloat(point.y));
        break;
      }
      case PathSpline::CommandType::LineTo: {
        const Vector2d& point = points[command.pointIndex];
        builder.lineTo(NarrowToFloat(point.x), NarrowToFloat(point.y));
        break;
      }
      case PathSpline::CommandType::CurveTo: {
        const Vector2d& control1 = points[command.pointIndex];
        const Vector2d& control2 = points[command.pointIndex + 1];
        const Vector2d& endPoint = points[command.pointIndex + 2];
        builder.cubicTo(NarrowToFloat(control1.x), NarrowToFloat(control1.y),
                        NarrowToFloat(control2.x), NarrowToFloat(control2.y),
                        NarrowToFloat(endPoint.x), NarrowToFloat(endPoint.y));
        break;
      }
      case PathSpline::CommandType::ClosePath: {
        builder.close();
        break;
      }
    }
  }

  return builder.finish().value_or(tiny_skia::Path());
}

inline Lengthd toPercent(Lengthd value, bool numbersArePercent) {
  if (!numbersArePercent) {
    return value;
  }

  if (value.unit == Lengthd::Unit::None) {
    value.value *= 100.0;
    value.unit = Lengthd::Unit::Percent;
  }

  return value;
}

inline double resolveGradientCoord(Lengthd value, const Boxd& viewBox, bool numbersArePercent) {
  return toPercent(value, numbersArePercent).toPixels(viewBox, FontMetrics());
}

Vector2d resolveGradientCoords(Lengthd x, Lengthd y, const Boxd& viewBox, bool numbersArePercent) {
  return Vector2d(
      toPercent(x, numbersArePercent).toPixels(viewBox, FontMetrics(), Lengthd::Extent::X),
      toPercent(y, numbersArePercent).toPixels(viewBox, FontMetrics(), Lengthd::Extent::Y));
}

Transformd resolveGradientTransform(
    const components::ComputedLocalTransformComponent* maybeTransformComponent,
    const Boxd& viewBox) {
  if (maybeTransformComponent == nullptr) {
    return Transformd();
  }

  const Vector2d origin = maybeTransformComponent->transformOrigin;
  const Transformd entityFromParent =
      maybeTransformComponent->rawCssTransform.compute(viewBox, FontMetrics());
  return Transformd::Translate(origin) * entityFromParent * Transformd::Translate(-origin);
}

std::optional<tiny_skia::Shader> instantiateGradientShader(
    const components::PaintResolvedReference& ref, const Boxd& pathBounds, const Boxd& viewBox,
    const css::RGBA& currentColor, float opacity) {
  const EntityHandle handle = ref.reference.handle;
  if (!handle) {
    return std::nullopt;
  }

  const auto* computedGradient = handle.try_get<components::ComputedGradientComponent>();
  if (computedGradient == nullptr || !computedGradient->initialized) {
    return std::nullopt;
  }

  const bool objectBoundingBox =
      computedGradient->gradientUnits == GradientUnits::ObjectBoundingBox;
  const bool numbersArePercent = objectBoundingBox;

  constexpr double kDegenerateBBoxTolerance = 1e-6;
  if (objectBoundingBox && (NearZero(pathBounds.width(), kDegenerateBBoxTolerance) ||
                            NearZero(pathBounds.height(), kDegenerateBBoxTolerance))) {
    return std::nullopt;
  }

  Transformd gradientFromGradientUnits;
  if (objectBoundingBox) {
    gradientFromGradientUnits = resolveGradientTransform(
        handle.try_get<components::ComputedLocalTransformComponent>(), kUnitPathBounds);

    const Transformd objectBoundingBoxFromUnitBox =
        Transformd::Scale(pathBounds.size()) * Transformd::Translate(pathBounds.topLeft);
    gradientFromGradientUnits = gradientFromGradientUnits * objectBoundingBoxFromUnitBox;
  } else {
    gradientFromGradientUnits = resolveGradientTransform(
        handle.try_get<components::ComputedLocalTransformComponent>(), viewBox);
  }

  const Boxd& bounds = objectBoundingBox ? kUnitPathBounds : viewBox;

  std::vector<tiny_skia::GradientStop> stops;
  stops.reserve(computedGradient->stops.size());
  for (const GradientStop& stop : computedGradient->stops) {
    stops.push_back(tiny_skia::GradientStop::create(
        stop.offset, toTinyColor(stop.color.resolve(currentColor, stop.opacity * opacity))));
  }

  if (stops.empty()) {
    return std::nullopt;
  }

  if (stops.size() == 1) {
    return tiny_skia::Shader(stops.front().color);
  }

  const tiny_skia::Transform shaderTransform = toTinyTransform(gradientFromGradientUnits);

  if (const auto* linear = handle.try_get<components::ComputedLinearGradientComponent>()) {
    const Vector2d start = resolveGradientCoords(linear->x1, linear->y1, bounds, numbersArePercent);
    const Vector2d end = resolveGradientCoords(linear->x2, linear->y2, bounds, numbersArePercent);
    const auto shader = tiny_skia::LinearGradient::create(
        toTinyPoint(start), toTinyPoint(end), std::move(stops),
        toTinySpreadMode(computedGradient->spreadMethod), shaderTransform);
    if (!shader.has_value()) {
      return std::nullopt;
    }

    return std::visit(
        [](auto&& value) -> tiny_skia::Shader {
          return tiny_skia::Shader(std::forward<decltype(value)>(value));
        },
        std::move(*shader));
  }

  if (const auto* radial = handle.try_get<components::ComputedRadialGradientComponent>()) {
    const double radius = resolveGradientCoord(radial->r, bounds, numbersArePercent);
    const Vector2d center =
        resolveGradientCoords(radial->cx, radial->cy, bounds, numbersArePercent);
    const double focalRadius = resolveGradientCoord(radial->fr, bounds, numbersArePercent);
    const Vector2d focalCenter =
        resolveGradientCoords(radial->fx.value_or(radial->cx), radial->fy.value_or(radial->cy),
                              bounds, numbersArePercent);

    if (NearZero(radius)) {
      return tiny_skia::Shader(stops.back().color);
    }

    const double distanceBetweenCenters = (center - focalCenter).length();
    if (distanceBetweenCenters + radius <= focalRadius) {
      return std::nullopt;
    }

    const auto shader = tiny_skia::RadialGradient::create(
        toTinyPoint(focalCenter), NarrowToFloat(focalRadius), toTinyPoint(center),
        NarrowToFloat(radius), std::move(stops), toTinySpreadMode(computedGradient->spreadMethod),
        shaderTransform);
    if (!shader.has_value()) {
      return std::nullopt;
    }

    return std::visit(
        [](auto&& value) -> tiny_skia::Shader {
          return tiny_skia::Shader(std::forward<decltype(value)>(value));
        },
        std::move(*shader));
  }

  return std::nullopt;
}

tiny_skia::Paint makeBasePaint(bool antialias) {
  tiny_skia::Paint paint;
  paint.antiAlias = antialias;
  return paint;
}

std::optional<tiny_skia::Mask> createMaskForSize(std::uint32_t width, std::uint32_t height) {
  return tiny_skia::Mask::fromSize(width, height);
}

void intersectMaskInPlace(tiny_skia::Mask& dst, const tiny_skia::Mask& src) {
  if (dst.size() != src.size()) {
    return;
  }

  std::span<std::uint8_t> dstData = dst.data();
  std::span<const std::uint8_t> srcData = src.data();
  for (std::size_t i = 0; i < dstData.size(); ++i) {
    dstData[i] = std::min(dstData[i], srcData[i]);
  }
}

void unionMaskInPlace(tiny_skia::Mask& dst, const tiny_skia::Mask& src) {
  if (dst.size() != src.size()) {
    return;
  }

  std::span<std::uint8_t> dstData = dst.data();
  std::span<const std::uint8_t> srcData = src.data();
  for (std::size_t i = 0; i < dstData.size(); ++i) {
    dstData[i] = std::max(dstData[i], srcData[i]);
  }
}

Vector2d patternRasterScaleForTransform(const Transformd& deviceFromPattern) {
  const double scaleX =
      std::max(1.0, deviceFromPattern.transformVector(Vector2d(1.0, 0.0)).length());
  const double scaleY =
      std::max(1.0, deviceFromPattern.transformVector(Vector2d(0.0, 1.0)).length());
  constexpr double kPatternSupersampleScale = 2.0;
  return Vector2d(scaleX * kPatternSupersampleScale, scaleY * kPatternSupersampleScale);
}

Vector2d effectivePatternRasterScale(const Boxd& tileRect, int pixelWidth, int pixelHeight,
                                     const Vector2d& fallbackScale) {
  const double scaleX =
      NearZero(tileRect.width()) ? fallbackScale.x : static_cast<double>(pixelWidth) / tileRect.width();
  const double scaleY = NearZero(tileRect.height())
                            ? fallbackScale.y
                            : static_cast<double>(pixelHeight) / tileRect.height();
  return Vector2d(scaleX, scaleY);
}

Transformd scaleTransformOutput(const Transformd& transform, const Vector2d& scale) {
  Transformd result = transform;
  result.data[0] *= scale.x;
  result.data[2] *= scale.x;
  result.data[4] *= scale.x;
  result.data[1] *= scale.y;
  result.data[3] *= scale.y;
  result.data[5] *= scale.y;
  return result;
}

void drawRectIntoMask(tiny_skia::Mask& mask, const Boxd& rect, const Transformd& transform,
                      bool antialias) {
  const std::optional<tiny_skia::Rect> tinyRect = toTinyRect(rect);
  if (!tinyRect.has_value()) {
    return;
  }

  const tiny_skia::Path path = tiny_skia::Path::fromRect(*tinyRect);
  mask.fillPath(path, tiny_skia::FillRule::Winding, antialias, toTinyTransform(transform));
}

// Blur implementation moved to tiny_skia::filter::gaussianBlur (GaussianBlur.h).

std::vector<std::uint8_t> premultiplyRgba(std::span<const std::uint8_t> rgbaPixels) {
  std::vector<std::uint8_t> result(rgbaPixels.begin(), rgbaPixels.end());
  for (std::size_t i = 0; i + 3 < result.size(); i += 4) {
    const unsigned alpha = result[i + 3];
    result[i + 0] =
        static_cast<std::uint8_t>((static_cast<unsigned>(result[i + 0]) * alpha + 127u) / 255u);
    result[i + 1] =
        static_cast<std::uint8_t>((static_cast<unsigned>(result[i + 1]) * alpha + 127u) / 255u);
    result[i + 2] =
        static_cast<std::uint8_t>((static_cast<unsigned>(result[i + 2]) * alpha + 127u) / 255u);
  }

  return result;
}

}  // namespace

RendererTinySkia::RendererTinySkia(bool verbose) : verbose_(verbose) {}

RendererTinySkia::~RendererTinySkia() = default;
RendererTinySkia::RendererTinySkia(RendererTinySkia&&) noexcept = default;
RendererTinySkia& RendererTinySkia::operator=(RendererTinySkia&&) noexcept = default;

void RendererTinySkia::draw(SVGDocument& document) {
  RendererDriver driver(*this, verbose_);
  driver.draw(document);
}

void RendererTinySkia::beginFrame(const RenderViewport& viewport) {
  viewport_ = viewport;
  const int pixelWidth = static_cast<int>(viewport.size.x * viewport.devicePixelRatio);
  const int pixelHeight = static_cast<int>(viewport.size.y * viewport.devicePixelRatio);

  frame_ = createTransparentPixmap(pixelWidth, pixelHeight);
  currentTransform_ = Transformd();
  transformStack_.clear();
  currentClipMask_.reset();
  clipStack_.clear();
  surfaceStack_.clear();
  patternFillPaint_.reset();
  patternStrokePaint_.reset();
}

void RendererTinySkia::endFrame() {
  if (!surfaceStack_.empty() && verbose_) {
    std::cerr << "RendererTinySkia: unbalanced surface stack at endFrame\n";
  }

  surfaceStack_.clear();
  currentTransform_ = Transformd();
  transformStack_.clear();
  currentClipMask_.reset();
  clipStack_.clear();
}

void RendererTinySkia::setTransform(const Transformd& transform) {
  if (!surfaceStack_.empty() && surfaceStack_.back().kind == SurfaceKind::PatternTile) {
    const Transformd& rasterFromTile = surfaceStack_.back().patternRasterFromTile;
    currentTransform_ =
        scaleTransformOutput(transform, Vector2d(rasterFromTile.data[0], rasterFromTile.data[3]));
  } else {
    currentTransform_ = transform;
  }
}

void RendererTinySkia::pushTransform(const Transformd& transform) {
  transformStack_.push_back(currentTransform_);
  currentTransform_ = transform * currentTransform_;
}

void RendererTinySkia::popTransform() {
  if (transformStack_.empty()) {
    return;
  }

  currentTransform_ = transformStack_.back();
  transformStack_.pop_back();
}

void RendererTinySkia::pushClip(const ResolvedClip& clip) {
  clipStack_.push_back(currentClipMask_);

  std::optional<tiny_skia::Mask> clipMask = buildClipMask(clip);
  if (!clipMask.has_value()) {
    return;
  }

  if (currentClipMask_.has_value()) {
    intersectMaskInPlace(*clipMask, *currentClipMask_);
  }

  currentClipMask_ = std::move(clipMask);
}

void RendererTinySkia::popClip() {
  if (clipStack_.empty()) {
    currentClipMask_.reset();
    return;
  }

  currentClipMask_ = std::move(clipStack_.back());
  clipStack_.pop_back();
}

void RendererTinySkia::pushIsolatedLayer(double opacity) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::IsolatedLayer;
  frame.opacity = opacity;
  frame.pixmap = createTransparentPixmap(static_cast<int>(currentPixmap().width()),
                                         static_cast<int>(currentPixmap().height()));
  surfaceStack_.push_back(std::move(frame));
}

void RendererTinySkia::popIsolatedLayer() {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::IsolatedLayer) {
    return;
  }

  SurfaceFrame frame = std::move(surfaceStack_.back());
  surfaceStack_.pop_back();
  compositePixmap(frame.pixmap, frame.opacity);
}

void RendererTinySkia::pushFilterLayer(const components::FilterGraph& filterGraph,
                                       const std::optional<Boxd>& filterRegion) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::FilterLayer;
  frame.filterGraph = filterGraph;
  frame.filterRegion = filterRegion;
  frame.pixmap = createTransparentPixmap(static_cast<int>(currentPixmap().width()),
                                         static_cast<int>(currentPixmap().height()));
  surfaceStack_.push_back(std::move(frame));
}

void RendererTinySkia::popFilterLayer() {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::FilterLayer) {
    return;
  }

  SurfaceFrame frame = std::move(surfaceStack_.back());
  surfaceStack_.pop_back();
  applyFilterGraph(frame.pixmap, frame.filterGraph, frame.filterRegion);

  // Clip pixels outside the filter region to transparent.
  if (frame.filterRegion.has_value()) {
    const Boxd& region = *frame.filterRegion;
    const int pw = static_cast<int>(frame.pixmap.width());
    const int ph = static_cast<int>(frame.pixmap.height());

    // Transform filter region bounds from local coordinates to pixel coordinates.
    const Boxd pixelRegion = currentTransform_.transformBox(region);
    const int rx0 = std::max(0, static_cast<int>(std::floor(pixelRegion.topLeft.x)));
    const int ry0 = std::max(0, static_cast<int>(std::floor(pixelRegion.topLeft.y)));
    const int rx1 = std::min(pw, static_cast<int>(std::ceil(pixelRegion.bottomRight.x)));
    const int ry1 = std::min(ph, static_cast<int>(std::ceil(pixelRegion.bottomRight.y)));

    auto data = frame.pixmap.data();
    // Clear rows above the region.
    for (int y = 0; y < ry0; ++y) {
      std::fill_n(data.data() + y * pw * 4, pw * 4, std::uint8_t{0});
    }
    // Clear left/right margins within the region.
    for (int y = ry0; y < ry1; ++y) {
      if (rx0 > 0) {
        std::fill_n(data.data() + y * pw * 4, rx0 * 4, std::uint8_t{0});
      }
      if (rx1 < pw) {
        std::fill_n(data.data() + (y * pw + rx1) * 4, (pw - rx1) * 4, std::uint8_t{0});
      }
    }
    // Clear rows below the region.
    for (int y = ry1; y < ph; ++y) {
      std::fill_n(data.data() + y * pw * 4, pw * 4, std::uint8_t{0});
    }
  }

  compositePixmap(frame.pixmap, 1.0);
}

void RendererTinySkia::pushMask(const std::optional<Boxd>& maskBounds) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::MaskCapture;
  frame.maskBounds = maskBounds;
  frame.maskBoundsTransform = currentTransform_;
  frame.pixmap = createTransparentPixmap(static_cast<int>(currentPixmap().width()),
                                         static_cast<int>(currentPixmap().height()));
  surfaceStack_.push_back(std::move(frame));
}

void RendererTinySkia::transitionMaskToContent() {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::MaskCapture) {
    return;
  }

  SurfaceFrame& frame = surfaceStack_.back();
  frame.maskAlpha =
      tiny_skia::Mask::fromPixmap(frame.pixmap.view(), tiny_skia::MaskType::Luminance);

  if (frame.maskAlpha.has_value() && frame.maskBounds.has_value()) {
    std::optional<tiny_skia::Mask> boundsMask =
        createMaskForSize(frame.pixmap.width(), frame.pixmap.height());
    if (boundsMask.has_value()) {
      drawRectIntoMask(*boundsMask, *frame.maskBounds, frame.maskBoundsTransform, antialias_);
      intersectMaskInPlace(*frame.maskAlpha, *boundsMask);
    }
  }

  frame.kind = SurfaceKind::MaskContent;
  frame.pixmap = createTransparentPixmap(static_cast<int>(frame.pixmap.width()),
                                         static_cast<int>(frame.pixmap.height()));
}

void RendererTinySkia::popMask() {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::MaskContent) {
    return;
  }

  SurfaceFrame frame = std::move(surfaceStack_.back());
  surfaceStack_.pop_back();
  if (frame.maskAlpha.has_value()) {
    auto pixmapView = frame.pixmap.mutableView();
    tiny_skia::Painter::applyMask(pixmapView, *frame.maskAlpha);
  }
  compositePixmap(frame.pixmap, 1.0);
}

void RendererTinySkia::beginPatternTile(const Boxd& tileRect, const Transformd& patternToTarget) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::PatternTile;
  frame.savedTransform = currentTransform_;
  frame.savedTransformStack = transformStack_;
  frame.savedClipMask = currentClipMask_;
  frame.savedClipStack = clipStack_;
  const Transformd deviceFromPattern = frame.savedTransform * patternToTarget;
  const Vector2d requestedRasterScale = patternRasterScaleForTransform(deviceFromPattern);
  const int pixelWidth =
      std::max(1, static_cast<int>(std::ceil(tileRect.width() * requestedRasterScale.x)));
  const int pixelHeight =
      std::max(1, static_cast<int>(std::ceil(tileRect.height() * requestedRasterScale.y)));
  const Vector2d rasterScale =
      effectivePatternRasterScale(tileRect, pixelWidth, pixelHeight, requestedRasterScale);
  frame.patternRasterFromTile = Transformd::Scale(rasterScale);
  frame.patternToTarget = patternToTarget;
  frame.patternToTarget.data[4] *= rasterScale.x;
  frame.patternToTarget.data[5] *= rasterScale.y;
  frame.patternToTarget =
      frame.patternToTarget * Transformd::Scale(1.0 / rasterScale.x, 1.0 / rasterScale.y);
  frame.pixmap = createTransparentPixmap(pixelWidth, pixelHeight);

  surfaceStack_.push_back(std::move(frame));

  currentTransform_ = surfaceStack_.back().patternRasterFromTile;
  transformStack_.clear();
  currentClipMask_.reset();
  clipStack_.clear();
}

void RendererTinySkia::endPatternTile(bool forStroke) {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::PatternTile) {
    return;
  }

  SurfaceFrame frame = std::move(surfaceStack_.back());
  surfaceStack_.pop_back();

  currentTransform_ = frame.savedTransform;
  transformStack_ = std::move(frame.savedTransformStack);
  currentClipMask_ = std::move(frame.savedClipMask);
  clipStack_ = std::move(frame.savedClipStack);
  PatternPaintState state{std::move(frame.pixmap), frame.patternToTarget};
  if (forStroke) {
    patternStrokePaint_ = std::move(state);
  } else {
    patternFillPaint_ = std::move(state);
  }
}

void RendererTinySkia::setPaint(const PaintParams& paint) {
  paint_ = paint;
  paintOpacity_ = paint.opacity;
}

void RendererTinySkia::drawPath(const PathShape& path, const StrokeParams& stroke) {
  if (currentPixmap().width() == 0 || currentPixmap().height() == 0) {
    return;
  }

  const tiny_skia::Path tinyPath = toTinyPath(path.path);
  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;

  const bool usedPatternFill = patternFillPaint_.has_value();
  if (std::optional<tiny_skia::Paint> fillPaint = makeFillPaint(path.path.bounds())) {
    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::fillPath(pixmapView, tinyPath, *fillPaint, toTinyFillRule(path.fillRule),
                                 toTinyTransform(currentTransform_), mask);
    if (usedPatternFill) {
      patternFillPaint_.reset();
    }
  }

  StrokeParams adjustedStroke = stroke;
  if (!adjustedStroke.dashArray.empty() && adjustedStroke.pathLength > 0.0 &&
      !NearZero(adjustedStroke.pathLength)) {
    const double actualLength = path.path.pathLength();
    const double dashUnitsScale = actualLength / adjustedStroke.pathLength;
    for (double& dash : adjustedStroke.dashArray) {
      dash *= dashUnitsScale;
    }
    adjustedStroke.dashOffset *= dashUnitsScale;
  }

  const bool usedPatternStroke = patternStrokePaint_.has_value();
  if (std::optional<tiny_skia::Paint> strokePaint =
          makeStrokePaint(path.path.bounds(), adjustedStroke)) {
    tiny_skia::Stroke tinyStroke;
    tinyStroke.width = NarrowToFloat(adjustedStroke.strokeWidth);
    tinyStroke.miterLimit = NarrowToFloat(adjustedStroke.miterLimit);
    tinyStroke.lineCap = toTinyLineCap(adjustedStroke.lineCap);
    tinyStroke.lineJoin = toTinyLineJoin(adjustedStroke.lineJoin);

    if (!adjustedStroke.dashArray.empty()) {
      const int repeats = (adjustedStroke.dashArray.size() & 1u) != 0u ? 2 : 1;
      std::vector<float> dashArray;
      dashArray.reserve(adjustedStroke.dashArray.size() * repeats);
      for (int i = 0; i < repeats; ++i) {
        for (double dash : adjustedStroke.dashArray) {
          dashArray.push_back(NarrowToFloat(dash));
        }
      }

      tinyStroke.dash = tiny_skia::StrokeDash::create(std::move(dashArray),
                                                      NarrowToFloat(adjustedStroke.dashOffset));
    }

    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::strokePath(pixmapView, tinyPath, *strokePaint, tinyStroke,
                                   toTinyTransform(currentTransform_), mask);
    if (usedPatternStroke) {
      patternStrokePaint_.reset();
    }
  }
}

void RendererTinySkia::drawRect(const Boxd& rect, const StrokeParams& stroke) {
  if (currentPixmap().width() == 0 || currentPixmap().height() == 0) {
    return;
  }

  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
  const std::optional<tiny_skia::Rect> tinyRect = toTinyRect(rect);
  if (!tinyRect.has_value()) {
    return;
  }

  const bool usedPatternFill = patternFillPaint_.has_value();
  if (std::optional<tiny_skia::Paint> fillPaint = makeFillPaint(rect)) {
    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::fillRect(pixmapView, *tinyRect, *fillPaint,
                                 toTinyTransform(currentTransform_), mask);
    if (usedPatternFill) {
      patternFillPaint_.reset();
    }
  }

  const bool usedPatternStroke = patternStrokePaint_.has_value();
  if (std::optional<tiny_skia::Paint> strokePaint = makeStrokePaint(rect, stroke)) {
    const tiny_skia::Path path = tiny_skia::Path::fromRect(*tinyRect);
    tiny_skia::Stroke tinyStroke;
    tinyStroke.width = NarrowToFloat(stroke.strokeWidth);
    tinyStroke.miterLimit = NarrowToFloat(stroke.miterLimit);
    tinyStroke.lineCap = toTinyLineCap(stroke.lineCap);
    tinyStroke.lineJoin = toTinyLineJoin(stroke.lineJoin);

    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::strokePath(pixmapView, path, *strokePaint, tinyStroke,
                                   toTinyTransform(currentTransform_), mask);
    if (usedPatternStroke) {
      patternStrokePaint_.reset();
    }
  }
}

void RendererTinySkia::drawEllipse(const Boxd& bounds, const StrokeParams& stroke) {
  const std::optional<tiny_skia::Rect> oval = toTinyRect(bounds);
  if (!oval.has_value()) {
    return;
  }

  tiny_skia::PathBuilder builder;
  builder.pushOval(*oval);
  const tiny_skia::Path path = builder.finish().value_or(tiny_skia::Path());
  if (path.empty()) {
    return;
  }

  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
  const bool usedPatternFill = patternFillPaint_.has_value();
  if (std::optional<tiny_skia::Paint> fillPaint = makeFillPaint(bounds)) {
    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::fillPath(pixmapView, path, *fillPaint, tiny_skia::FillRule::Winding,
                                 toTinyTransform(currentTransform_), mask);
    if (usedPatternFill) {
      patternFillPaint_.reset();
    }
  }

  const bool usedPatternStroke = patternStrokePaint_.has_value();
  if (std::optional<tiny_skia::Paint> strokePaint = makeStrokePaint(bounds, stroke)) {
    tiny_skia::Stroke tinyStroke;
    tinyStroke.width = NarrowToFloat(stroke.strokeWidth);
    tinyStroke.miterLimit = NarrowToFloat(stroke.miterLimit);
    tinyStroke.lineCap = toTinyLineCap(stroke.lineCap);
    tinyStroke.lineJoin = toTinyLineJoin(stroke.lineJoin);

    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::strokePath(pixmapView, path, *strokePaint, tinyStroke,
                                   toTinyTransform(currentTransform_), mask);
    if (usedPatternStroke) {
      patternStrokePaint_.reset();
    }
  }
}

void RendererTinySkia::drawImage(const ImageResource& image, const ImageParams& params) {
  if (image.data.empty() || image.width <= 0 || image.height <= 0) {
    return;
  }

  const std::optional<tiny_skia::Rect> targetRect = toTinyRect(params.targetRect);
  if (!targetRect.has_value()) {
    return;
  }

  std::vector<std::uint8_t> premultiplied = premultiplyRgba(image.data);
  auto maybePixmap = tiny_skia::Pixmap::fromVec(
      std::move(premultiplied), tiny_skia::IntSize(static_cast<std::uint32_t>(image.width),
                                                   static_cast<std::uint32_t>(image.height)));
  if (!maybePixmap.has_value()) {
    return;
  }

  const double scaleX = params.targetRect.width() / static_cast<double>(image.width);
  const double scaleY = params.targetRect.height() / static_cast<double>(image.height);
  const Transformd imageFromLocal = Transformd::Scale(scaleX, scaleY) *
                                    Transformd::Translate(params.targetRect.topLeft) *
                                    currentTransform_;

  tiny_skia::PixmapPaint paint;
  paint.opacity = NarrowToFloat(params.opacity * paintOpacity_);
  paint.blendMode = tiny_skia::BlendMode::SourceOver;
  paint.quality = params.imageRenderingPixelated ? tiny_skia::FilterQuality::Nearest
                                                 : tiny_skia::FilterQuality::Bilinear;
  paint.unpremulStore = surfaceStack_.empty();

  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
  auto pixmapView = currentPixmapView();
  tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, maybePixmap->view(), paint,
                                 toTinyTransform(imageFromLocal), mask);
}

void RendererTinySkia::drawText(const components::ComputedTextComponent& text,
                                const TextParams& params) {
  (void)text;
  (void)params;
  maybeWarnUnsupportedText();
}

RendererBitmap RendererTinySkia::takeSnapshot() const {
  RendererBitmap snapshot;
  snapshot.dimensions = Vector2i(width(), height());
  snapshot.rowBytes = static_cast<std::size_t>(width()) * 4u;
  if (frame_.width() == 0 || frame_.height() == 0) {
    return snapshot;
  }

  auto maybeCopy = tiny_skia::Pixmap::fromVec(
      std::vector<std::uint8_t>(frame_.data().begin(), frame_.data().end()), frame_.size());
  if (!maybeCopy.has_value()) {
    snapshot.dimensions = Vector2i::Zero();
    snapshot.rowBytes = 0;
    return snapshot;
  }

  snapshot.pixels = maybeCopy->release();
  return snapshot;
}

bool RendererTinySkia::save(const char* filename) {
  const RendererBitmap snapshot = takeSnapshot();
  if (snapshot.empty()) {
    return false;
  }

  return RendererImageIO::writeRgbaPixelsToPngFile(filename, snapshot.pixels, snapshot.dimensions.x,
                                                   snapshot.dimensions.y);
}

int RendererTinySkia::width() const {
  return static_cast<int>(frame_.width());
}

int RendererTinySkia::height() const {
  return static_cast<int>(frame_.height());
}

tiny_skia::Pixmap& RendererTinySkia::currentPixmap() {
  return surfaceStack_.empty() ? frame_ : surfaceStack_.back().pixmap;
}

const tiny_skia::Pixmap& RendererTinySkia::currentPixmap() const {
  return surfaceStack_.empty() ? frame_ : surfaceStack_.back().pixmap;
}

tiny_skia::MutablePixmapView RendererTinySkia::currentPixmapView() {
  return currentPixmap().mutableView();
}

std::optional<tiny_skia::Mask> RendererTinySkia::buildClipMask(const ResolvedClip& clip) const {
  if (clip.empty()) {
    return std::nullopt;
  }

  if (verbose_) {
    std::cout << "[TinySkia::buildClipMask] clipRect=";
    if (clip.clipRect.has_value()) {
      std::cout << *clip.clipRect;
    } else {
      std::cout << "none";
    }
    std::cout << " clipPaths=" << clip.clipPaths.size()
              << "\n  currentTransform=" << currentTransform_
              << "  clipPathUnitsTransform=" << clip.clipPathUnitsTransform;
  }

  const auto createMask = [&]() {
    std::optional<tiny_skia::Mask> mask =
        createMaskForSize(currentPixmap().width(), currentPixmap().height());
    if (mask.has_value()) {
      std::fill(mask->data().begin(), mask->data().end(), 0);
    }
    return mask;
  };

  std::optional<tiny_skia::Mask> rectMask;
  if (clip.clipRect.has_value()) {
    rectMask = createMask();
    if (rectMask.has_value()) {
      drawRectIntoMask(*rectMask, *clip.clipRect, currentTransform_, antialias_);
    }
  }

  const auto renderShapeMask = [&](const PathShape& shape) -> std::optional<tiny_skia::Mask> {
    std::optional<tiny_skia::Mask> shapeMask = createMask();
    if (!shapeMask.has_value()) {
      return std::nullopt;
    }

    const tiny_skia::Path path = toTinyPath(shape.path);
    if (path.empty()) {
      if (verbose_) {
        std::cout << "\n  shape layer=" << shape.layer << " empty path";
      }
      return shapeMask;
    }

    const Transformd clipPathTransform =
        clip.clipPathUnitsTransform * shape.entityFromParent * currentTransform_;
    if (verbose_) {
      const Boxd pathBounds = shape.path.bounds();
      std::cout << "\n  shape layer=" << shape.layer << " bounds=" << pathBounds
                << "\n    entityFromParent=" << shape.entityFromParent
                << "    combinedTransform=" << clipPathTransform
                << "    transformedBounds=" << clipPathTransform.transformBox(pathBounds);
    }
    shapeMask->fillPath(path, toTinyFillRule(shape.fillRule), antialias_,
                        toTinyTransform(clipPathTransform));
    return shapeMask;
  };

  std::optional<tiny_skia::Mask> pathMask;
  if (!clip.clipPaths.empty()) {
    std::ptrdiff_t index = static_cast<std::ptrdiff_t>(clip.clipPaths.size()) - 1;

    std::function<std::optional<tiny_skia::Mask>(int)> buildLayerMask =
        [&](int layer) -> std::optional<tiny_skia::Mask> {
      std::optional<tiny_skia::Mask> layerMask;
      while (index >= 0 && clip.clipPaths[static_cast<std::size_t>(index)].layer == layer) {
        const PathShape& shape = clip.clipPaths[static_cast<std::size_t>(index)];
        std::optional<tiny_skia::Mask> shapeMask = renderShapeMask(shape);
        --index;

        if (shapeMask.has_value() && index >= 0 &&
            clip.clipPaths[static_cast<std::size_t>(index)].layer > layer) {
          std::optional<tiny_skia::Mask> nestedMask =
              buildLayerMask(clip.clipPaths[static_cast<std::size_t>(index)].layer);
          if (nestedMask.has_value()) {
            intersectMaskInPlace(*shapeMask, *nestedMask);
          }
        }

        if (!shapeMask.has_value()) {
          continue;
        }

        if (!layerMask.has_value()) {
          layerMask = std::move(shapeMask);
        } else {
          unionMaskInPlace(*layerMask, *shapeMask);
        }
      }
      return layerMask;
    };

    pathMask = buildLayerMask(clip.clipPaths.back().layer);
  }

  std::optional<tiny_skia::Mask> result;
  if (rectMask.has_value()) {
    result = std::move(rectMask);
  }
  if (pathMask.has_value()) {
    if (result.has_value()) {
      intersectMaskInPlace(*result, *pathMask);
    } else {
      result = std::move(pathMask);
    }
  }

  if (verbose_) {
    std::cout << "\n";
  }

  return result;
}

std::optional<tiny_skia::Paint> RendererTinySkia::makeFillPaint(const Boxd& bounds) {
  if (std::holds_alternative<PaintServer::None>(paint_.fill)) {
    return std::nullopt;
  }

  tiny_skia::Paint paint = makeBasePaint(antialias_);
  paint.unpremulStore = surfaceStack_.empty();

  if (patternFillPaint_.has_value()) {
    paint.shader =
        tiny_skia::Pattern(patternFillPaint_->pixmap.view(), tiny_skia::SpreadMode::Repeat,
                           tiny_skia::FilterQuality::Bilinear, NarrowToFloat(paint_.fillOpacity),
                           toTinyTransform(patternFillPaint_->patternToTarget));
    return paint;
  }

  const css::RGBA currentColor = paint_.currentColor.rgba();
  const float fillOpacity = NarrowToFloat(paint_.fillOpacity);

  if (const auto* solid = std::get_if<PaintServer::Solid>(&paint_.fill)) {
    paint.shader = toTinyColor(solid->color.resolve(currentColor, fillOpacity));
    return paint;
  }

  if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint_.fill)) {
    if (std::optional<tiny_skia::Shader> shader =
            instantiateGradientShader(*ref, bounds, paint_.viewBox, currentColor, fillOpacity)) {
      paint.shader = std::move(*shader);
      return paint;
    }

    if (ref->fallback.has_value()) {
      paint.shader = toTinyColor(ref->fallback->resolve(currentColor, fillOpacity));
      return paint;
    }
  }

  return std::nullopt;
}

std::optional<tiny_skia::Paint> RendererTinySkia::makeStrokePaint(const Boxd& bounds,
                                                                  const StrokeParams& stroke) {
  if (std::holds_alternative<PaintServer::None>(paint_.stroke) || stroke.strokeWidth <= 0.0) {
    return std::nullopt;
  }

  tiny_skia::Paint paint = makeBasePaint(antialias_);
  paint.unpremulStore = surfaceStack_.empty();

  if (patternStrokePaint_.has_value()) {
    paint.shader =
        tiny_skia::Pattern(patternStrokePaint_->pixmap.view(), tiny_skia::SpreadMode::Repeat,
                           tiny_skia::FilterQuality::Bilinear, NarrowToFloat(paint_.strokeOpacity),
                           toTinyTransform(patternStrokePaint_->patternToTarget));
    return paint;
  }

  const css::RGBA currentColor = paint_.currentColor.rgba();
  const float strokeOpacity = NarrowToFloat(paint_.strokeOpacity);

  if (const auto* solid = std::get_if<PaintServer::Solid>(&paint_.stroke)) {
    paint.shader = toTinyColor(solid->color.resolve(currentColor, strokeOpacity));
    return paint;
  }

  if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint_.stroke)) {
    if (std::optional<tiny_skia::Shader> shader =
            instantiateGradientShader(*ref, bounds, paint_.viewBox, currentColor, strokeOpacity)) {
      paint.shader = std::move(*shader);
      return paint;
    }

    if (ref->fallback.has_value()) {
      paint.shader = toTinyColor(ref->fallback->resolve(currentColor, strokeOpacity));
      return paint;
    }
  }

  return std::nullopt;
}

tiny_skia::Pixmap RendererTinySkia::createTransparentPixmap(int width, int height) const {
  if (width <= 0 || height <= 0) {
    return tiny_skia::Pixmap();
  }

  auto maybePixmap = tiny_skia::Pixmap::fromSize(static_cast<std::uint32_t>(width),
                                                 static_cast<std::uint32_t>(height));
  if (!maybePixmap.has_value()) {
    return tiny_skia::Pixmap();
  }

  maybePixmap->fill(tiny_skia::Color::transparent);
  return std::move(*maybePixmap);
}

void RendererTinySkia::compositePixmap(const tiny_skia::Pixmap& pixmap, double opacity) {
  if (opacity <= 0.0 || pixmap.width() == 0 || pixmap.height() == 0) {
    return;
  }

  tiny_skia::PixmapPaint paint;
  paint.opacity = NarrowToFloat(opacity);
  paint.blendMode = tiny_skia::BlendMode::SourceOver;
  paint.quality = tiny_skia::FilterQuality::Nearest;
  paint.unpremulStore = surfaceStack_.empty();

  auto pixmapView = currentPixmapView();
  tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, pixmap.view(), paint);
}

void RendererTinySkia::applyFilterGraph(tiny_skia::Pixmap& pixmap,
                                        const components::FilterGraph& filterGraph,
                                        const std::optional<Boxd>& filterRegion) {
  using namespace components;
  using namespace components::filter_primitive;

  const int w = static_cast<int>(pixmap.width());
  const int h = static_cast<int>(pixmap.height());

  // Convert SourceGraphic from sRGB to linearRGB for filter processing.
  // Per spec, color-interpolation-filters defaults to linearRGB.
  const bool useLinearRGB =
      filterGraph.colorInterpolationFilters != ColorInterpolationFilters::SRGB;
  if (useLinearRGB) {
    tiny_skia::filter::srgbToLinear(pixmap);
  }

  // Buffer management: sourceGraphic is the captured content, previousOutput tracks implicit
  // chaining, and namedBuffers stores explicitly named results.
  tiny_skia::Pixmap* sourceGraphic = &pixmap;
  std::optional<tiny_skia::Pixmap> sourceAlpha;
  std::optional<tiny_skia::Pixmap> previousOutput;
  std::map<RcString, tiny_skia::Pixmap> namedBuffers;

  // Per CSS Filter Effects spec, the default primitive subregion is the tightest fitting
  // bounding box of the referenced input. Track each node's effective pixel-space subregion
  // for feTile (which tiles based on the input's subregion).
  const Boxd fullRegion = Boxd::FromXYWH(0, 0, w, h);
  // Filter region in pixel space, used to clip primitive subregions.
  const Boxd filterRegionPixel =
      filterRegion.has_value() ? currentTransform_.transformBox(*filterRegion) : fullRegion;
  Boxd previousOutputSubregion = fullRegion;
  std::map<RcString, Boxd> namedSubregions;


  // Lazily create SourceAlpha: same as SourceGraphic but with RGB=0, keeping only alpha.
  auto getSourceAlpha = [&]() -> tiny_skia::Pixmap* {
    if (!sourceAlpha.has_value()) {
      sourceAlpha = *sourceGraphic;
      auto data = sourceAlpha->data();
      for (int i = 0; i < w * h; ++i) {
        // Premultiplied RGBA: set R, G, B to 0, keep A.
        data[i * 4 + 0] = 0;
        data[i * 4 + 1] = 0;
        data[i * 4 + 2] = 0;
      }
    }
    return &sourceAlpha.value();
  };

  auto resolveInput = [&](const FilterInput& input) -> tiny_skia::Pixmap* {
    return std::visit(
        [&](const auto& v) -> tiny_skia::Pixmap* {
          using V = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<V, FilterInput::Previous>) {
            return previousOutput.has_value() ? &previousOutput.value() : sourceGraphic;
          } else if constexpr (std::is_same_v<V, FilterStandardInput>) {
            if (v == FilterStandardInput::SourceGraphic) {
              return sourceGraphic;
            }
            if (v == FilterStandardInput::SourceAlpha) {
              return getSourceAlpha();
            }
            // TODO: FillPaint, StrokePaint
            return sourceGraphic;
          } else if constexpr (std::is_same_v<V, FilterInput::Named>) {
            auto it = namedBuffers.find(v.name);
            if (it != namedBuffers.end()) {
              return &it->second;
            }
            return sourceGraphic;
          } else {
            return sourceGraphic;
          }
        },
        input.value);
  };

  auto resolveInputSubregion = [&](const FilterInput& input) -> Boxd {
    return std::visit(
        [&](const auto& v) -> Boxd {
          using V = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<V, FilterInput::Previous>) {
            return previousOutputSubregion;
          } else if constexpr (std::is_same_v<V, FilterInput::Named>) {
            auto it = namedSubregions.find(v.name);
            if (it != namedSubregions.end()) {
              return it->second;
            }
            return fullRegion;
          } else {
            return fullRegion;
          }
        },
        input.value);
  };

  for (const FilterNode& node : filterGraph.nodes) {
    tiny_skia::Pixmap* input =
        node.inputs.empty() ? sourceGraphic : resolveInput(node.inputs[0]);

    std::optional<tiny_skia::Pixmap> output;

    std::visit(
        [&](const auto& primitive) {
          using T = std::decay_t<decltype(primitive)>;
          if constexpr (std::is_same_v<T, GaussianBlur>) {
            // Gaussian blur operates in-place on a copy of the input.
            // Scale stdDeviation from user space to pixel space.
            const Vector2d origin = currentTransform_.transformPosition(Vector2d(0, 0));
            const Vector2d scaleX =
                currentTransform_.transformPosition(Vector2d(primitive.stdDeviationX, 0)) - origin;
            const Vector2d scaleY =
                currentTransform_.transformPosition(Vector2d(0, primitive.stdDeviationY)) - origin;
            // Negative stdDeviation means no blur (per SVG spec).
            const double pixelSigmaX =
                primitive.stdDeviationX >= 0 ? std::abs(scaleX.x) : 0.0;
            const double pixelSigmaY =
                primitive.stdDeviationY >= 0 ? std::abs(scaleY.y) : 0.0;
            output = *input;
            tiny_skia::filter::gaussianBlur(*output, pixelSigmaX, pixelSigmaY);
          } else if constexpr (std::is_same_v<T, Flood>) {
            output = createTransparentPixmap(w, h);
            const css::RGBA rgba = primitive.floodColor.asRGBA();
            const double alpha = (rgba.a / 255.0) * primitive.floodOpacity;
            const uint8_t pa = static_cast<uint8_t>(std::round(alpha * 255.0));
            const uint8_t pr = static_cast<uint8_t>(std::round(rgba.r * alpha));
            const uint8_t pg = static_cast<uint8_t>(std::round(rgba.g * alpha));
            const uint8_t pb = static_cast<uint8_t>(std::round(rgba.b * alpha));
            tiny_skia::filter::flood(*output, pr, pg, pb, pa);
            // Convert flood color from sRGB to linearRGB to match the filter pipeline.
            if (useLinearRGB) {
              tiny_skia::filter::srgbToLinear(*output);
            }
          } else if constexpr (std::is_same_v<T, Offset>) {
            output = createTransparentPixmap(w, h);
            // Transform offset from user space to pixel space.
            const Vector2d origin = currentTransform_.transformPosition(Vector2d(0, 0));
            const Vector2d offsetPoint =
                currentTransform_.transformPosition(Vector2d(primitive.dx, primitive.dy));
            const Vector2d pixelOffset = offsetPoint - origin;
            tiny_skia::filter::offset(*input, *output, static_cast<int>(pixelOffset.x),
                                      static_cast<int>(pixelOffset.y));
          } else if constexpr (std::is_same_v<T, Composite>) {
            tiny_skia::Pixmap* input2 =
                node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : sourceGraphic;
            output = createTransparentPixmap(w, h);

            // Map FilterGraph Composite::Operator to tiny_skia::filter::CompositeOp.
            static constexpr tiny_skia::filter::CompositeOp kOpMap[] = {
                tiny_skia::filter::CompositeOp::Over,
                tiny_skia::filter::CompositeOp::In,
                tiny_skia::filter::CompositeOp::Out,
                tiny_skia::filter::CompositeOp::Atop,
                tiny_skia::filter::CompositeOp::Xor,
                tiny_skia::filter::CompositeOp::Lighter,
                tiny_skia::filter::CompositeOp::Arithmetic,
            };
            const auto op = kOpMap[static_cast<int>(primitive.op)];
            tiny_skia::filter::composite(*input, *input2, *output, op, primitive.k1, primitive.k2,
                                         primitive.k3, primitive.k4);
          } else if constexpr (std::is_same_v<T, Merge>) {
            // Collect all input layer pointers.
            std::vector<const tiny_skia::Pixmap*> layers;
            for (const auto& mergeInput : node.inputs) {
              layers.push_back(resolveInput(mergeInput));
            }
            output = createTransparentPixmap(w, h);
            tiny_skia::filter::merge(layers, *output);
          } else if constexpr (std::is_same_v<T, ColorMatrix>) {
            output = *input;
            std::array<double, 20> matrix;
            if (primitive.type == ColorMatrix::Type::Matrix) {
              if (primitive.values.size() == 20) {
                for (size_t j = 0; j < 20; ++j) {
                  matrix[j] = primitive.values[j];
                }
              } else {
                matrix = tiny_skia::filter::identityMatrix();
              }
            } else if (primitive.type == ColorMatrix::Type::Saturate) {
              const double s = primitive.values.empty() ? 1.0 : primitive.values[0];
              matrix = tiny_skia::filter::saturateMatrix(s);
            } else if (primitive.type == ColorMatrix::Type::HueRotate) {
              const double angle = primitive.values.empty() ? 0.0 : primitive.values[0];
              matrix = tiny_skia::filter::hueRotateMatrix(angle);
            } else if (primitive.type == ColorMatrix::Type::LuminanceToAlpha) {
              matrix = tiny_skia::filter::luminanceToAlphaMatrix();
            } else {
              matrix = tiny_skia::filter::identityMatrix();
            }
            tiny_skia::filter::colorMatrix(*output, matrix);
          } else if constexpr (std::is_same_v<T, Blend>) {
            tiny_skia::Pixmap* input2 =
                node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : sourceGraphic;
            output = createTransparentPixmap(w, h);
            // Map FilterGraph Blend::Mode to tiny_skia::filter::BlendMode.
            static constexpr tiny_skia::filter::BlendMode kModeMap[] = {
                tiny_skia::filter::BlendMode::Normal,
                tiny_skia::filter::BlendMode::Multiply,
                tiny_skia::filter::BlendMode::Screen,
                tiny_skia::filter::BlendMode::Darken,
                tiny_skia::filter::BlendMode::Lighten,
            };
            const auto blendMode = kModeMap[static_cast<int>(primitive.mode)];
            // SVG: in=foreground (Cs), in2=background (Cb).
            tiny_skia::filter::blend(*input2, *input, *output, blendMode);
          } else if constexpr (std::is_same_v<T, ComponentTransfer>) {
            output = *input;
            auto toFunc = [](const ComponentTransfer::Func& f) {
              tiny_skia::filter::TransferFunc tf;
              tf.type = static_cast<tiny_skia::filter::TransferFuncType>(f.type);
              tf.tableValues = f.tableValues;
              tf.slope = f.slope;
              tf.intercept = f.intercept;
              tf.amplitude = f.amplitude;
              tf.exponent = f.exponent;
              tf.offset = f.offset;
              return tf;
            };
            tiny_skia::filter::componentTransfer(*output, toFunc(primitive.funcR),
                                                  toFunc(primitive.funcG), toFunc(primitive.funcB),
                                                  toFunc(primitive.funcA));
          } else if constexpr (std::is_same_v<T, DropShadow>) {
            // feDropShadow decomposes into:
            //   flood(shadowColor) → composite(in, SourceAlpha) → offset(dx,dy)
            //   → gaussianBlur(stdDev) → merge(blurredShadow, input)
            const css::RGBA rgba = primitive.floodColor.asRGBA();
            const double alpha = (rgba.a / 255.0) * primitive.floodOpacity;
            const uint8_t pa = static_cast<uint8_t>(std::round(alpha * 255.0));
            const uint8_t pr = static_cast<uint8_t>(std::round(rgba.r * alpha));
            const uint8_t pg = static_cast<uint8_t>(std::round(rgba.g * alpha));
            const uint8_t pb = static_cast<uint8_t>(std::round(rgba.b * alpha));

            // 1. Create flood-filled buffer with the shadow color.
            auto floodBuf = createTransparentPixmap(w, h);
            tiny_skia::filter::flood(floodBuf, pr, pg, pb, pa);
            if (useLinearRGB) {
              tiny_skia::filter::srgbToLinear(floodBuf);
            }

            // 2. Composite flood with SourceAlpha (extract shape of the input).
            auto compositeBuf = createTransparentPixmap(w, h);
            tiny_skia::Pixmap* srcAlpha = getSourceAlpha();
            tiny_skia::filter::composite(floodBuf, *srcAlpha, compositeBuf,
                                         tiny_skia::filter::CompositeOp::In, 0, 0, 0, 0);

            // 3. Offset by (dx, dy).
            const Vector2d origin = currentTransform_.transformPosition(Vector2d(0, 0));
            const Vector2d offsetPoint =
                currentTransform_.transformPosition(Vector2d(primitive.dx, primitive.dy));
            const Vector2d pixelOffset = offsetPoint - origin;
            auto offsetBuf = createTransparentPixmap(w, h);
            tiny_skia::filter::offset(compositeBuf, offsetBuf, static_cast<int>(pixelOffset.x),
                                      static_cast<int>(pixelOffset.y));

            // 4. Gaussian blur.
            const Vector2d scaleX =
                currentTransform_.transformPosition(Vector2d(primitive.stdDeviationX, 0)) - origin;
            const Vector2d scaleY =
                currentTransform_.transformPosition(Vector2d(0, primitive.stdDeviationY)) - origin;
            const double pixelSigmaX =
                primitive.stdDeviationX >= 0 ? std::abs(scaleX.x) : 0.0;
            const double pixelSigmaY =
                primitive.stdDeviationY >= 0 ? std::abs(scaleY.y) : 0.0;
            tiny_skia::filter::gaussianBlur(offsetBuf, pixelSigmaX, pixelSigmaY);

            // 5. Merge blurred shadow behind the input.
            output = createTransparentPixmap(w, h);
            std::vector<const tiny_skia::Pixmap*> layers = {&offsetBuf, input};
            tiny_skia::filter::merge(layers, *output);
          } else if constexpr (std::is_same_v<T, Morphology>) {
            output = createTransparentPixmap(w, h);
            // Per SVG spec: negative radius → transparent black. Both zero → transparent.
            // One zero + one positive → apply in the positive direction (0 = 1px window).
            if (primitive.radiusX < 0 || primitive.radiusY < 0 ||
                (primitive.radiusX == 0 && primitive.radiusY == 0)) {
              // output stays transparent
            } else {
              const Vector2d origin = currentTransform_.transformPosition(Vector2d(0, 0));
              const Vector2d scaleX =
                  currentTransform_.transformPosition(Vector2d(primitive.radiusX, 0)) - origin;
              const Vector2d scaleY =
                  currentTransform_.transformPosition(Vector2d(0, primitive.radiusY)) - origin;
              const int pixelRadiusX = static_cast<int>(std::round(std::abs(scaleX.x)));
              const int pixelRadiusY = static_cast<int>(std::round(std::abs(scaleY.y)));
              const auto op = primitive.op == Morphology::Operator::Erode
                                  ? tiny_skia::filter::MorphologyOp::Erode
                                  : tiny_skia::filter::MorphologyOp::Dilate;
              tiny_skia::filter::morphology(*input, *output, op, pixelRadiusX, pixelRadiusY);
            }
          } else if constexpr (std::is_same_v<T, ConvolveMatrix>) {
            output = createTransparentPixmap(w, h);
            const int requiredKernelSize = primitive.orderX * primitive.orderY;
            // Validate: kernel must have exactly orderX * orderY values, and order must be
            // positive. Per spec, wrong kernel count disables the filter.
            if (primitive.orderX > 0 && primitive.orderY > 0 &&
                static_cast<int>(primitive.kernelMatrix.size()) == requiredKernelSize) {
              // Resolve defaults: unset targetX/Y defaults to floor(order/2).
              const int targetX =
                  primitive.targetX.value_or(primitive.orderX / 2);
              const int targetY =
                  primitive.targetY.value_or(primitive.orderY / 2);

              // Validate target is in range [0, order). Out-of-range disables the filter.
              if (targetX >= 0 && targetX < primitive.orderX && targetY >= 0 &&
                  targetY < primitive.orderY) {
                // Compute divisor. Per spec: if divisor attribute is explicitly 0, it's
                // an error (filter disabled). If unset, use sum of kernel (or 1 if sum=0).
                double divisor = 0.0;
                bool divisorValid = true;
                if (primitive.divisor.has_value()) {
                  divisor = *primitive.divisor;
                  if (divisor == 0.0) {
                    // Per spec: "It is an error if divisor is zero." Filter disabled.
                    divisorValid = false;
                  }
                } else {
                  for (int i = 0; i < requiredKernelSize; ++i) {
                    divisor += primitive.kernelMatrix[i];
                  }
                  if (divisor == 0.0) {
                    divisor = 1.0;
                  }
                }

                if (divisorValid) {
                  tiny_skia::filter::ConvolveParams params;
                  params.orderX = primitive.orderX;
                  params.orderY = primitive.orderY;
                  params.kernel = std::span<const double>(primitive.kernelMatrix.data(),
                                                          requiredKernelSize);
                  params.divisor = divisor;
                  params.bias = primitive.bias;
                  params.targetX = targetX;
                  params.targetY = targetY;
                  params.edgeMode =
                      static_cast<tiny_skia::filter::ConvolveEdgeMode>(primitive.edgeMode);
                  params.preserveAlpha = primitive.preserveAlpha;

                  tiny_skia::filter::convolveMatrix(*input, *output, params);
                }
              }
              // else: invalid targetX/targetY, output stays transparent (disabled filter)
            }
            // else: invalid order or kernel size, output stays transparent (disabled filter)
          } else if constexpr (std::is_same_v<T, Tile>) {
            output = createTransparentPixmap(w, h);
            // The tile rectangle is the input's primitive subregion.
            const Boxd inputSubregion =
                node.inputs.empty() ? previousOutputSubregion
                                    : resolveInputSubregion(node.inputs[0]);
            const int tileX = std::max(0, static_cast<int>(std::floor(inputSubregion.topLeft.x)));
            const int tileY = std::max(0, static_cast<int>(std::floor(inputSubregion.topLeft.y)));
            const int tileR =
                std::min(w, static_cast<int>(std::ceil(inputSubregion.bottomRight.x)));
            const int tileB =
                std::min(h, static_cast<int>(std::ceil(inputSubregion.bottomRight.y)));
            const int tileW = tileR - tileX;
            const int tileH = tileB - tileY;
            if (tileW > 0 && tileH > 0) {
              tiny_skia::filter::tile(*input, *output, tileX, tileY, tileW, tileH);
            }
          } else if constexpr (std::is_same_v<T, Turbulence>) {
            output = createTransparentPixmap(w, h);
            tiny_skia::filter::TurbulenceParams turbParams;
            turbParams.type =
                primitive.type == Turbulence::Type::FractalNoise
                    ? tiny_skia::filter::TurbulenceType::FractalNoise
                    : tiny_skia::filter::TurbulenceType::Turbulence;
            turbParams.baseFrequencyX = primitive.baseFrequencyX;
            turbParams.baseFrequencyY = primitive.baseFrequencyY;
            turbParams.numOctaves = primitive.numOctaves;
            turbParams.seed = primitive.seed;
            turbParams.stitchTiles = primitive.stitchTiles;
            turbParams.tileWidth = w;
            turbParams.tileHeight = h;
            // Pass the current transform's scale so noise is generated in user space.
            turbParams.scaleX = std::abs(currentTransform_.data[0]);
            turbParams.scaleY = std::abs(currentTransform_.data[3]);
            if (turbParams.scaleX < 1e-10) turbParams.scaleX = 1.0;
            if (turbParams.scaleY < 1e-10) turbParams.scaleY = 1.0;
            tiny_skia::filter::turbulence(*output, turbParams);
          } else if constexpr (std::is_same_v<T, DisplacementMap>) {
            tiny_skia::Pixmap* input2 =
                node.inputs.size() > 1 ? resolveInput(node.inputs[1]) : sourceGraphic;
            output = createTransparentPixmap(w, h);
            static constexpr tiny_skia::filter::DisplacementChannel kChMap[] = {
                tiny_skia::filter::DisplacementChannel::R,
                tiny_skia::filter::DisplacementChannel::G,
                tiny_skia::filter::DisplacementChannel::B,
                tiny_skia::filter::DisplacementChannel::A,
            };
            const auto xCh = kChMap[static_cast<int>(primitive.xChannelSelector)];
            const auto yCh = kChMap[static_cast<int>(primitive.yChannelSelector)];
            tiny_skia::filter::displacementMap(*input, *input2, *output, primitive.scale, xCh,
                                               yCh);
          } else {
            maybeWarnUnsupportedFilter();
          }
        },
        node.primitive);

    if (output.has_value()) {
      // Apply primitive subregion clipping if x/y/width/height are specified on the node.
      if (node.x.has_value() || node.y.has_value() || node.width.has_value() ||
          node.height.has_value()) {
        // Compute subregion in user space. Unspecified attributes default to full pixmap extent.
        const double ux = node.x.has_value() ? node.x->value : 0.0;
        const double uy = node.y.has_value() ? node.y->value : 0.0;
        const double uw = node.width.has_value() ? node.width->value : w;
        const double uh = node.height.has_value() ? node.height->value : h;
        const Boxd userRegion = Boxd::FromXYWH(ux, uy, uw, uh);

        // Transform from user space to pixel space.
        const Boxd pixelRegion = currentTransform_.transformBox(userRegion);
        const int rx0 = std::max(0, static_cast<int>(std::floor(pixelRegion.topLeft.x)));
        const int ry0 = std::max(0, static_cast<int>(std::floor(pixelRegion.topLeft.y)));
        const int rx1 = std::min(w, static_cast<int>(std::ceil(pixelRegion.bottomRight.x)));
        const int ry1 = std::min(h, static_cast<int>(std::ceil(pixelRegion.bottomRight.y)));

        // Clear pixels outside the primitive subregion.
        auto data = output->data();
        for (int y = 0; y < ry0; ++y) {
          std::fill_n(data.data() + y * w * 4, w * 4, std::uint8_t{0});
        }
        for (int y = ry0; y < ry1; ++y) {
          if (rx0 > 0) {
            std::fill_n(data.data() + y * w * 4, rx0 * 4, std::uint8_t{0});
          }
          if (rx1 < w) {
            std::fill_n(data.data() + (y * w + rx1) * 4, (w - rx1) * 4, std::uint8_t{0});
          }
        }
        for (int y = ry1; y < h; ++y) {
          std::fill_n(data.data() + y * w * 4, w * 4, std::uint8_t{0});
        }
      }

      // Compute this node's effective subregion. Per CSS Filter Effects spec:
      // - If explicit x/y/width/height, use those values.
      // - Otherwise, inherit from the input's subregion (tightest fitting bbox).
      Boxd nodeSubregion;
      if (node.x.has_value() || node.y.has_value() || node.width.has_value() ||
          node.height.has_value()) {
        const double ux = node.x.has_value() ? node.x->value : 0.0;
        const double uy = node.y.has_value() ? node.y->value : 0.0;
        const double uw = node.width.has_value() ? node.width->value : w;
        const double uh = node.height.has_value() ? node.height->value : h;
        nodeSubregion = currentTransform_.transformBox(Boxd::FromXYWH(ux, uy, uw, uh));
      } else {
        // Default: inherit from input's subregion (tightest fitting bbox per CSS Filter
        // Effects spec). The subregion represents the tile rectangle geometry, not the
        // content position — primitives like feOffset move content within the subregion
        // but don't shift the subregion itself.
        nodeSubregion = node.inputs.empty() ? previousOutputSubregion
                                            : resolveInputSubregion(node.inputs[0]);
      }
      // Intersect with the filter region — content outside it is invisible.
      nodeSubregion = Boxd(
          Vector2d(std::max(nodeSubregion.topLeft.x, filterRegionPixel.topLeft.x),
                   std::max(nodeSubregion.topLeft.y, filterRegionPixel.topLeft.y)),
          Vector2d(std::min(nodeSubregion.bottomRight.x, filterRegionPixel.bottomRight.x),
                   std::min(nodeSubregion.bottomRight.y, filterRegionPixel.bottomRight.y)));
      // Clamp to non-negative dimensions.
      if (nodeSubregion.bottomRight.x < nodeSubregion.topLeft.x) {
        nodeSubregion.bottomRight.x = nodeSubregion.topLeft.x;
      }
      if (nodeSubregion.bottomRight.y < nodeSubregion.topLeft.y) {
        nodeSubregion.bottomRight.y = nodeSubregion.topLeft.y;
      }

      if (node.result.has_value()) {
        namedBuffers[*node.result] = *output;
        namedSubregions[*node.result] = nodeSubregion;
      }
      previousOutput = std::move(output);
      previousOutputSubregion = nodeSubregion;
    }
  }

  // Convert final output from linearRGB back to sRGB and copy to source pixmap.
  if (previousOutput.has_value()) {
    if (useLinearRGB) {
      tiny_skia::filter::linearToSrgb(*previousOutput);
    }
    auto srcData = previousOutput->data();
    auto dstData = pixmap.data();
    std::copy(srcData.begin(), srcData.end(), dstData.begin());
  }
}

void RendererTinySkia::maybeWarnUnsupportedFilter() {
  if (!verbose_ || warnedUnsupportedFilter_) {
    return;
  }

  warnedUnsupportedFilter_ = true;
  std::cerr << "RendererTinySkia: some filter effects are not implemented\n";
}

void RendererTinySkia::maybeWarnUnsupportedText() {
  if (!verbose_ || warnedUnsupportedText_) {
    return;
  }

  warnedUnsupportedText_ = true;
  std::cerr << "RendererTinySkia: text rendering is not implemented\n";
}

}  // namespace donner::svg
