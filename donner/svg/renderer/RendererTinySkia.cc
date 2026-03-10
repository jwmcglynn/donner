#include "donner/svg/renderer/RendererTinySkia.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
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
#include "donner/svg/renderer/FilterGraphExecutor.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/PathBuilder.h"
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
  const double scaleX = NearZero(tileRect.width())
                            ? fallbackScale.x
                            : static_cast<double>(pixelWidth) / tileRect.width();
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

/**
 * Returns true if the filter graph is a linear chain of single-input blur-family primitives
 * eligible for the transformed local-raster path.
 *
 * Eligible graphs have:
 * - Only GaussianBlur, Offset, or DropShadow primitives
 * - Each node has at most one input, and that input is Previous or implicit SourceGraphic
 * - No named `result` attributes
 * - No primitive subregions (x/y/width/height unset on every node)
 * - primitiveUnits is not objectBoundingBox
 */
bool isEligibleForTransformedBlurPath(const components::FilterGraph& filterGraph) {
  if (filterGraph.nodes.empty()) {
    return false;
  }

  if (filterGraph.primitiveUnits == PrimitiveUnits::ObjectBoundingBox) {
    return false;
  }

  bool hasBlur = false;

  for (std::size_t i = 0; i < filterGraph.nodes.size(); ++i) {
    const components::FilterNode& node = filterGraph.nodes[i];

    // Check primitive type: only GaussianBlur, Offset, DropShadow allowed.
    const bool isBlur =
        std::holds_alternative<components::filter_primitive::GaussianBlur>(node.primitive) ||
        std::holds_alternative<components::filter_primitive::DropShadow>(node.primitive);
    const bool isOffset =
        std::holds_alternative<components::filter_primitive::Offset>(node.primitive);
    if (!isBlur && !isOffset) {
      return false;
    }
    hasBlur |= isBlur;

    // No named result reuse.
    if (node.result.has_value()) {
      return false;
    }

    // No primitive subregions.
    if (node.x.has_value() || node.y.has_value() || node.width.has_value() ||
        node.height.has_value()) {
      return false;
    }

    // Linear chain: each node has 0 or 1 inputs, and that input is Previous or SourceGraphic
    // (for the first node).
    if (node.inputs.size() > 1) {
      return false;
    }

    if (node.inputs.size() == 1) {
      const auto& input = node.inputs[0];
      if (std::holds_alternative<components::FilterInput::Previous>(input.value)) {
        // OK
      } else if (const auto* stdInput =
                     std::get_if<components::FilterStandardInput>(&input.value)) {
        if (*stdInput != components::FilterStandardInput::SourceGraphic) {
          return false;
        }
      } else {
        // Named input - not eligible.
        return false;
      }
    }
  }

  // Must have at least one blur primitive to benefit from the transformed path.
  return hasBlur;
}

bool graphUsesStandardInput(const components::FilterGraph& filterGraph,
                            components::FilterStandardInput input) {
  for (const components::FilterNode& node : filterGraph.nodes) {
    for (const components::FilterInput& nodeInput : node.inputs) {
      const auto* standardInput = std::get_if<components::FilterStandardInput>(&nodeInput.value);
      if (standardInput != nullptr && *standardInput == input) {
        return true;
      }
    }
  }

  return false;
}

bool graphHasAnisotropicBlur(const components::FilterGraph& filterGraph) {
  for (const auto& node : filterGraph.nodes) {
    bool anisotropic = false;
    std::visit(
        [&](const auto& prim) {
          using T = std::decay_t<decltype(prim)>;
          if constexpr (std::is_same_v<T, components::filter_primitive::GaussianBlur>) {
            anisotropic = !NearEquals(prim.stdDeviationX, prim.stdDeviationY, 1e-6);
          } else if constexpr (std::is_same_v<T, components::filter_primitive::DropShadow>) {
            anisotropic = !NearEquals(prim.stdDeviationX, prim.stdDeviationY, 1e-6);
          }
        },
        node.primitive);
    if (anisotropic) {
      return true;
    }
  }

  return false;
}

bool shouldUseTransformedBlurPath(const components::FilterGraph& filterGraph,
                                  const Transformd& deviceFromFilter) {
  const Vector2d xAxis = deviceFromFilter.transformVector(Vector2d(1.0, 0.0));
  const Vector2d yAxis = deviceFromFilter.transformVector(Vector2d(0.0, 1.0));
  const double dot = xAxis.x * yAxis.x + xAxis.y * yAxis.y;
  const bool hasSkew = !NearZero(dot, 1e-6);
  if (hasSkew) {
    return true;
  }

  const bool hasRotation =
      !NearZero(deviceFromFilter.data[1], 1e-6) || !NearZero(deviceFromFilter.data[2], 1e-6);
  if (!hasRotation) {
    return false;
  }

  return graphHasAnisotropicBlur(filterGraph);
}

/**
 * Computes the required blur padding in user-space units for the transformed local-raster path.
 * Returns 3σ + 1 for the maximum stdDeviation across all blur primitives.
 */
double computeBlurPadding(const components::FilterGraph& filterGraph) {
  double maxSigma = 0.0;
  for (const auto& node : filterGraph.nodes) {
    std::visit(
        [&](const auto& prim) {
          using T = std::decay_t<decltype(prim)>;
          if constexpr (std::is_same_v<T, components::filter_primitive::GaussianBlur>) {
            maxSigma = std::max({maxSigma, prim.stdDeviationX, prim.stdDeviationY});
          } else if constexpr (std::is_same_v<T, components::filter_primitive::DropShadow>) {
            maxSigma = std::max({maxSigma, prim.stdDeviationX, prim.stdDeviationY});
          }
        },
        node.primitive);
  }
  return maxSigma * 3.0 + 1.0;
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
  const int width = static_cast<int>(currentPixmap().width());
  const int height = static_cast<int>(currentPixmap().height());
  frame.pixmap = createTransparentPixmap(width, height);
  if (!surfaceStack_.empty()) {
    const SurfaceFrame& parent = surfaceStack_.back();
    if (parent.fillPaintPixmap.has_value()) {
      frame.fillPaintPixmap = createTransparentPixmap(width, height);
    }
    if (parent.strokePaintPixmap.has_value()) {
      frame.strokePaintPixmap = createTransparentPixmap(width, height);
    }
  }
  surfaceStack_.push_back(std::move(frame));
}

void RendererTinySkia::popIsolatedLayer() {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::IsolatedLayer) {
    return;
  }

  SurfaceFrame frame = std::move(surfaceStack_.back());
  surfaceStack_.pop_back();
  compositePixmap(frame.pixmap, frame.opacity);
  if (!surfaceStack_.empty()) {
    SurfaceFrame& parent = surfaceStack_.back();
    if (frame.fillPaintPixmap.has_value() && parent.fillPaintPixmap.has_value()) {
      compositePixmapInto(*parent.fillPaintPixmap, *frame.fillPaintPixmap, frame.opacity);
    }
    if (frame.strokePaintPixmap.has_value() && parent.strokePaintPixmap.has_value()) {
      compositePixmapInto(*parent.strokePaintPixmap, *frame.strokePaintPixmap, frame.opacity);
    }
  }
}

void RendererTinySkia::pushFilterLayer(const components::FilterGraph& filterGraph,
                                       const std::optional<Boxd>& filterRegion) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::FilterLayer;
  frame.filterGraph = filterGraph;
  frame.filterRegion = filterRegion;
  frame.deviceFromFilter = currentTransform_;
  const int width = static_cast<int>(currentPixmap().width());
  const int height = static_cast<int>(currentPixmap().height());
  frame.pixmap = createTransparentPixmap(width, height);
  if (graphUsesStandardInput(filterGraph, components::FilterStandardInput::FillPaint)) {
    frame.fillPaintPixmap = createTransparentPixmap(width, height);
  }
  if (graphUsesStandardInput(filterGraph, components::FilterStandardInput::StrokePaint)) {
    frame.strokePaintPixmap = createTransparentPixmap(width, height);
  }
  surfaceStack_.push_back(std::move(frame));
}

void RendererTinySkia::popFilterLayer() {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::FilterLayer) {
    return;
  }

  SurfaceFrame frame = std::move(surfaceStack_.back());
  surfaceStack_.pop_back();

  // Use the transformed local-raster path for eligible simple blur chains with
  // rotation or skew transforms. This resamples device content into an axis-aligned local
  // raster, applies the blur in filter-local coordinates, and composites back. This produces
  // correct blur orientation for rotated/skewed elements.
  // Only activate when the transform has rotation/skew (off-diagonal elements non-zero).
  // For pure scale+translate, the device-space blur is already correct.
  const bool useTransformedBlurPath =
      frame.filterRegion.has_value() && frame.filterRegion->width() > 0 &&
      frame.filterRegion->height() > 0 && !NearZero(frame.deviceFromFilter.determinant()) &&
      isEligibleForTransformedBlurPath(frame.filterGraph) &&
      shouldUseTransformedBlurPath(frame.filterGraph, frame.deviceFromFilter);

  if (useTransformedBlurPath) {
    const Transformd& deviceFromFilter = frame.deviceFromFilter;
    const Boxd& filterRegion = *frame.filterRegion;

    // Compute local raster density from the filter transform basis vectors.
    const double scaleX =
        std::max(1.0, deviceFromFilter.transformVector(Vector2d(1.0, 0.0)).length());
    const double scaleY =
        std::max(1.0, deviceFromFilter.transformVector(Vector2d(0.0, 1.0)).length());

    const double blurPadding = computeBlurPadding(frame.filterGraph);
    const Boxd paddedRegion(filterRegion.topLeft - Vector2d(blurPadding, blurPadding),
                            filterRegion.bottomRight + Vector2d(blurPadding, blurPadding));

    const int localWidth = std::max(1, static_cast<int>(std::ceil(paddedRegion.width() * scaleX)));
    const int localHeight =
        std::max(1, static_cast<int>(std::ceil(paddedRegion.height() * scaleY)));

    if (verbose_) {
      std::cerr << "[TinySkia::popFilterLayer] TRANSFORMED PATH\n"
                << "  filterRegion=" << filterRegion << " paddedRegion=" << paddedRegion << "\n"
                << "  scaleX=" << scaleX << " scaleY=" << scaleY << " localSize=" << localWidth
                << "x" << localHeight << "\n";
    }

    // Allocate local-raster pixmap.
    tiny_skia::Pixmap localPixmap = createTransparentPixmap(localWidth, localHeight);
    if (localPixmap.width() == 0 || localPixmap.height() == 0) {
      goto device_space_fallback;
    }

    {
      // Resample device pixels into local filter coordinates.
      const Transformd filterFromDevice = deviceFromFilter.inverse();
      const Transformd deviceToLocal =
          Transformd::Scale(scaleX, scaleY) *
          Transformd::Translate(-paddedRegion.topLeft.x, -paddedRegion.topLeft.y) *
          filterFromDevice;

      {
        tiny_skia::PixmapPaint resamplePaint;
        resamplePaint.opacity = 1.0f;
        resamplePaint.blendMode = tiny_skia::BlendMode::Source;
        resamplePaint.quality = tiny_skia::FilterQuality::Bilinear;

        auto localView = localPixmap.mutableView();
        tiny_skia::Painter::drawPixmap(localView, 0, 0, frame.pixmap.view(), resamplePaint,
                                       toTinyTransform(deviceToLocal));
      }

      // Execute the filter graph in local raster space.
      const Transformd localTransform = Transformd::Scale(scaleX, scaleY);
      const Boxd localFilterRegion(
          Vector2d(blurPadding, blurPadding),
          Vector2d(blurPadding + filterRegion.width(), blurPadding + filterRegion.height()));

      ApplyFilterGraphToPixmap(localPixmap, frame.filterGraph, localTransform, localFilterRegion,
                               false);

      // Clip to the original filter region within the padded raster.
      ClipFilterOutputToRegion(localPixmap, localFilterRegion, localTransform);

      // Composite the filtered local raster back to the parent device pixmap.
      const Transformd deviceFromLocal =
          deviceFromFilter * Transformd::Translate(paddedRegion.topLeft.x, paddedRegion.topLeft.y) *
          Transformd::Scale(1.0 / scaleX, 1.0 / scaleY);

      tiny_skia::PixmapPaint compositePaint;
      compositePaint.opacity = 1.0f;
      compositePaint.blendMode = tiny_skia::BlendMode::SourceOver;
      compositePaint.quality = tiny_skia::FilterQuality::Bilinear;
      compositePaint.unpremulStore = surfaceStack_.empty();

      const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
      auto pixmapView = currentPixmapView();
      tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, localPixmap.view(), compositePaint,
                                     toTinyTransform(deviceFromLocal), mask);
    }
  } else {
  device_space_fallback:
    ApplyFilterGraphToPixmap(
        frame.pixmap, frame.filterGraph, frame.deviceFromFilter, frame.filterRegion, true,
        frame.fillPaintPixmap.has_value() ? &*frame.fillPaintPixmap : nullptr,
        frame.strokePaintPixmap.has_value() ? &*frame.strokePaintPixmap : nullptr);
    ClipFilterOutputToRegion(frame.pixmap, frame.filterRegion, frame.deviceFromFilter);

    compositePixmap(frame.pixmap, 1.0);
  }
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
  const int width = static_cast<int>(frame.pixmap.width());
  const int height = static_cast<int>(frame.pixmap.height());
  frame.pixmap = createTransparentPixmap(width, height);
  frame.fillPaintPixmap.reset();
  frame.strokePaintPixmap.reset();
  if (surfaceStack_.size() >= 2u) {
    const SurfaceFrame& parent = surfaceStack_[surfaceStack_.size() - 2u];
    if (parent.fillPaintPixmap.has_value()) {
      frame.fillPaintPixmap = createTransparentPixmap(width, height);
    }
    if (parent.strokePaintPixmap.has_value()) {
      frame.strokePaintPixmap = createTransparentPixmap(width, height);
    }
  }
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
    if (frame.fillPaintPixmap.has_value()) {
      auto fillView = frame.fillPaintPixmap->mutableView();
      tiny_skia::Painter::applyMask(fillView, *frame.maskAlpha);
    }
    if (frame.strokePaintPixmap.has_value()) {
      auto strokeView = frame.strokePaintPixmap->mutableView();
      tiny_skia::Painter::applyMask(strokeView, *frame.maskAlpha);
    }
  }
  compositePixmap(frame.pixmap, 1.0);
  if (!surfaceStack_.empty()) {
    SurfaceFrame& parent = surfaceStack_.back();
    if (frame.fillPaintPixmap.has_value() && parent.fillPaintPixmap.has_value()) {
      compositePixmapInto(*parent.fillPaintPixmap, *frame.fillPaintPixmap, 1.0);
    }
    if (frame.strokePaintPixmap.has_value() && parent.strokePaintPixmap.has_value()) {
      compositePixmapInto(*parent.strokePaintPixmap, *frame.strokePaintPixmap, 1.0);
    }
  }
}

void RendererTinySkia::beginPatternTile(const Boxd& tileRect, const Transformd& targetFromPattern) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::PatternTile;
  frame.savedTransform = currentTransform_;
  frame.savedTransformStack = transformStack_;
  frame.savedClipMask = currentClipMask_;
  frame.savedClipStack = clipStack_;
  const Transformd deviceFromPattern = frame.savedTransform * targetFromPattern;
  const Vector2d requestedRasterScale = patternRasterScaleForTransform(deviceFromPattern);
  const int pixelWidth =
      std::max(1, static_cast<int>(std::ceil(tileRect.width() * requestedRasterScale.x)));
  const int pixelHeight =
      std::max(1, static_cast<int>(std::ceil(tileRect.height() * requestedRasterScale.y)));
  const Vector2d rasterScale =
      effectivePatternRasterScale(tileRect, pixelWidth, pixelHeight, requestedRasterScale);
  frame.patternRasterFromTile = Transformd::Scale(rasterScale);
  frame.targetFromPattern = targetFromPattern;
  frame.targetFromPattern.data[4] *= rasterScale.x;
  frame.targetFromPattern.data[5] *= rasterScale.y;
  frame.targetFromPattern =
      frame.targetFromPattern * Transformd::Scale(1.0 / rasterScale.x, 1.0 / rasterScale.y);
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
  PatternPaintState state{std::move(frame.pixmap), frame.targetFromPattern};
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
  tiny_skia::Pixmap* fillPaintPixmap =
      !surfaceStack_.empty() && surfaceStack_.back().fillPaintPixmap.has_value()
          ? &*surfaceStack_.back().fillPaintPixmap
          : nullptr;
  tiny_skia::Pixmap* strokePaintPixmap =
      !surfaceStack_.empty() && surfaceStack_.back().strokePaintPixmap.has_value()
          ? &*surfaceStack_.back().strokePaintPixmap
          : nullptr;

  const bool usedPatternFill = patternFillPaint_.has_value();
  if (std::optional<tiny_skia::Paint> fillPaint = makeFillPaint(path.path.bounds())) {
    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::fillPath(pixmapView, tinyPath, *fillPaint, toTinyFillRule(path.fillRule),
                                 toTinyTransform(currentTransform_), mask);
    if (fillPaintPixmap != nullptr) {
      auto fillPaintView = fillPaintPixmap->mutableView();
      tiny_skia::Painter::fillPath(fillPaintView, tinyPath, *fillPaint,
                                   toTinyFillRule(path.fillRule),
                                   toTinyTransform(currentTransform_), mask);
    }
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
    if (strokePaintPixmap != nullptr) {
      auto strokePaintView = strokePaintPixmap->mutableView();
      tiny_skia::Painter::strokePath(strokePaintView, tinyPath, *strokePaint, tinyStroke,
                                     toTinyTransform(currentTransform_), mask);
    }
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

  tiny_skia::Pixmap* fillPaintPixmap =
      !surfaceStack_.empty() && surfaceStack_.back().fillPaintPixmap.has_value()
          ? &*surfaceStack_.back().fillPaintPixmap
          : nullptr;
  tiny_skia::Pixmap* strokePaintPixmap =
      !surfaceStack_.empty() && surfaceStack_.back().strokePaintPixmap.has_value()
          ? &*surfaceStack_.back().strokePaintPixmap
          : nullptr;

  const bool usedPatternFill = patternFillPaint_.has_value();
  if (std::optional<tiny_skia::Paint> fillPaint = makeFillPaint(rect)) {
    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::fillRect(pixmapView, *tinyRect, *fillPaint,
                                 toTinyTransform(currentTransform_), mask);
    if (fillPaintPixmap != nullptr) {
      auto fillPaintView = fillPaintPixmap->mutableView();
      tiny_skia::Painter::fillRect(fillPaintView, *tinyRect, *fillPaint,
                                   toTinyTransform(currentTransform_), mask);
    }
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
    if (strokePaintPixmap != nullptr) {
      auto strokePaintView = strokePaintPixmap->mutableView();
      tiny_skia::Painter::strokePath(strokePaintView, path, *strokePaint, tinyStroke,
                                     toTinyTransform(currentTransform_), mask);
    }
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
  tiny_skia::Pixmap* fillPaintPixmap =
      !surfaceStack_.empty() && surfaceStack_.back().fillPaintPixmap.has_value()
          ? &*surfaceStack_.back().fillPaintPixmap
          : nullptr;
  tiny_skia::Pixmap* strokePaintPixmap =
      !surfaceStack_.empty() && surfaceStack_.back().strokePaintPixmap.has_value()
          ? &*surfaceStack_.back().strokePaintPixmap
          : nullptr;

  const bool usedPatternFill = patternFillPaint_.has_value();
  if (std::optional<tiny_skia::Paint> fillPaint = makeFillPaint(bounds)) {
    auto pixmapView = currentPixmapView();
    tiny_skia::Painter::fillPath(pixmapView, path, *fillPaint, tiny_skia::FillRule::Winding,
                                 toTinyTransform(currentTransform_), mask);
    if (fillPaintPixmap != nullptr) {
      auto fillPaintView = fillPaintPixmap->mutableView();
      tiny_skia::Painter::fillPath(fillPaintView, path, *fillPaint, tiny_skia::FillRule::Winding,
                                   toTinyTransform(currentTransform_), mask);
    }
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
    if (strokePaintPixmap != nullptr) {
      auto strokePaintView = strokePaintPixmap->mutableView();
      tiny_skia::Painter::strokePath(strokePaintView, path, *strokePaint, tinyStroke,
                                     toTinyTransform(currentTransform_), mask);
    }
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

  std::vector<std::uint8_t> premultiplied = PremultiplyRgba(image.data);
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
                           toTinyTransform(patternFillPaint_->targetFromPattern));
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
                           toTinyTransform(patternStrokePaint_->targetFromPattern));
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
  compositePixmapInto(currentPixmap(), pixmap, opacity);
}

void RendererTinySkia::compositePixmapInto(tiny_skia::Pixmap& destination,
                                           const tiny_skia::Pixmap& pixmap, double opacity) {
  if (opacity <= 0.0 || pixmap.width() == 0 || pixmap.height() == 0) {
    return;
  }

  tiny_skia::PixmapPaint paint;
  paint.opacity = NarrowToFloat(opacity);
  paint.blendMode = tiny_skia::BlendMode::SourceOver;
  paint.quality = tiny_skia::FilterQuality::Nearest;
  paint.unpremulStore = &destination == &frame_;

  auto destinationView = destination.mutableView();
  tiny_skia::Painter::drawPixmap(destinationView, 0, 0, pixmap.view(), paint);
}

void RendererTinySkia::maybeWarnUnsupportedText() {
  if (!verbose_ || warnedUnsupportedText_) {
    return;
  }

  warnedUnsupportedText_ = true;
  std::cerr << "RendererTinySkia: text rendering is not implemented\n";
}

}  // namespace donner::svg
