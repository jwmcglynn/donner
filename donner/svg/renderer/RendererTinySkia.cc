#include "donner/svg/renderer/RendererTinySkia.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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
#ifdef DONNER_FILTERS_ENABLED
#include "donner/svg/renderer/FilterGraphExecutor.h"
#endif
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererImageIO.h"
#ifdef DONNER_TEXT_ENABLED
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"
#include "donner/svg/text/TextLayoutParams.h"
#endif
#include "tiny_skia/Painter.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/shaders/Shaders.h"

namespace donner::svg {

namespace {

#ifdef DONNER_TEXT_ENABLED
TextLayoutParams toTextLayoutParams(const TextParams& params) {
  TextLayoutParams layoutParams;
  layoutParams.fontFamilies = params.fontFamilies;
  layoutParams.fontSize = params.fontSize;
  layoutParams.viewBox = params.viewBox;
  layoutParams.fontMetrics = params.fontMetrics;
  layoutParams.textAnchor = params.textAnchor;
  layoutParams.dominantBaseline = params.dominantBaseline;
  layoutParams.writingMode = params.writingMode;
  layoutParams.letterSpacingPx = params.letterSpacingPx;
  layoutParams.wordSpacingPx = params.wordSpacingPx;
  layoutParams.textLength = params.textLength;
  layoutParams.lengthAdjust = params.lengthAdjust;
  return layoutParams;
}
#endif

const Box2d kUnitPathBounds(Vector2d::Zero(), Vector2d(1, 1));

tiny_skia::Color toTinyColor(const css::RGBA& rgba) {
  return tiny_skia::Color::fromRgba8(rgba.r, rgba.g, rgba.b, rgba.a);
}

tiny_skia::Point toTinyPoint(const Vector2d& value) {
  return tiny_skia::Point::fromXY(NarrowToFloat(value.x), NarrowToFloat(value.y));
}

tiny_skia::Transform toTinyTransform(const Transform2d& transform) {
  return tiny_skia::Transform::fromRow(
      NarrowToFloat(transform.data[0]), NarrowToFloat(transform.data[1]),
      NarrowToFloat(transform.data[2]), NarrowToFloat(transform.data[3]),
      NarrowToFloat(transform.data[4]), NarrowToFloat(transform.data[5]));
}

std::optional<tiny_skia::Rect> toTinyRect(const Box2d& box) {
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

tiny_skia::Path toTinyPath(const Path& spline) {
  tiny_skia::PathBuilder builder(spline.commands().size(), spline.points().size());
  const auto points = spline.points();

  for (const Path::Command& command : spline.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo: {
        const Vector2d& point = points[command.pointIndex];
        builder.moveTo(NarrowToFloat(point.x), NarrowToFloat(point.y));
        break;
      }
      case Path::Verb::LineTo: {
        const Vector2d& point = points[command.pointIndex];
        builder.lineTo(NarrowToFloat(point.x), NarrowToFloat(point.y));
        break;
      }
      case Path::Verb::QuadTo: {
        const Vector2d& control = points[command.pointIndex];
        const Vector2d& endPoint = points[command.pointIndex + 1];
        builder.quadTo(NarrowToFloat(control.x), NarrowToFloat(control.y),
                       NarrowToFloat(endPoint.x), NarrowToFloat(endPoint.y));
        break;
      }
      case Path::Verb::CurveTo: {
        const Vector2d& control1 = points[command.pointIndex];
        const Vector2d& control2 = points[command.pointIndex + 1];
        const Vector2d& endPoint = points[command.pointIndex + 2];
        builder.cubicTo(NarrowToFloat(control1.x), NarrowToFloat(control1.y),
                        NarrowToFloat(control2.x), NarrowToFloat(control2.y),
                        NarrowToFloat(endPoint.x), NarrowToFloat(endPoint.y));
        break;
      }
      case Path::Verb::ClosePath: {
        builder.close();
        break;
      }
    }
  }

  return builder.finish().value_or(tiny_skia::Path());
}

Path transformPath(const Path& spline, const Transform2d& transform) {
  PathBuilder builder;
  const auto points = spline.points();

  for (const Path::Command& command : spline.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo:
        builder.moveTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::LineTo:
        builder.lineTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::QuadTo:
        builder.quadTo(transform.transformPosition(points[command.pointIndex]),
                       transform.transformPosition(points[command.pointIndex + 1]));
        break;
      case Path::Verb::CurveTo:
        builder.curveTo(transform.transformPosition(points[command.pointIndex]),
                        transform.transformPosition(points[command.pointIndex + 1]),
                        transform.transformPosition(points[command.pointIndex + 2]));
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  }

  return builder.build();
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

inline double resolveGradientCoord(Lengthd value, const Box2d& viewBox, bool numbersArePercent) {
  return toPercent(value, numbersArePercent).toPixels(viewBox, FontMetrics());
}

Vector2d resolveGradientCoords(Lengthd x, Lengthd y, const Box2d& viewBox, bool numbersArePercent) {
  return Vector2d(
      toPercent(x, numbersArePercent).toPixels(viewBox, FontMetrics(), Lengthd::Extent::X),
      toPercent(y, numbersArePercent).toPixels(viewBox, FontMetrics(), Lengthd::Extent::Y));
}

Transform2d resolveGradientTransform(
    const components::ComputedLocalTransformComponent* maybeTransformComponent,
    const Box2d& viewBox) {
  if (maybeTransformComponent == nullptr) {
    return Transform2d();
  }

  const Vector2d origin = maybeTransformComponent->transformOrigin;
  const Transform2d parentFromEntity =
      maybeTransformComponent->rawCssTransform.compute(viewBox, FontMetrics());
  return Transform2d::Translate(origin) * parentFromEntity * Transform2d::Translate(-origin);
}

std::optional<tiny_skia::Shader> instantiateGradientShader(
    const components::PaintResolvedReference& ref, const Box2d& pathBounds, const Box2d& viewBox,
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

  Transform2d gradientFromGradientUnits;
  if (objectBoundingBox) {
    gradientFromGradientUnits = resolveGradientTransform(
        handle.try_get<components::ComputedLocalTransformComponent>(), kUnitPathBounds);

    const Transform2d objectBoundingBoxFromUnitBox =
        Transform2d::Scale(pathBounds.size()) * Transform2d::Translate(pathBounds.topLeft);
    gradientFromGradientUnits = gradientFromGradientUnits * objectBoundingBoxFromUnitBox;
  } else {
    gradientFromGradientUnits = resolveGradientTransform(
        handle.try_get<components::ComputedLocalTransformComponent>(), viewBox);
  }

  const Box2d& bounds = objectBoundingBox ? kUnitPathBounds : viewBox;

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

Vector2d patternRasterScaleForTransform(const Transform2d& deviceFromPattern) {
  const double scaleX =
      std::max(1.0, deviceFromPattern.transformVector(Vector2d(1.0, 0.0)).length());
  const double scaleY =
      std::max(1.0, deviceFromPattern.transformVector(Vector2d(0.0, 1.0)).length());
  constexpr double kPatternSupersampleScale = 2.0;
  return Vector2d(scaleX * kPatternSupersampleScale, scaleY * kPatternSupersampleScale);
}

Vector2d effectivePatternRasterScale(const Box2d& tileRect, int pixelWidth, int pixelHeight,
                                     const Vector2d& fallbackScale) {
  const double scaleX = NearZero(tileRect.width())
                            ? fallbackScale.x
                            : static_cast<double>(pixelWidth) / tileRect.width();
  const double scaleY = NearZero(tileRect.height())
                            ? fallbackScale.y
                            : static_cast<double>(pixelHeight) / tileRect.height();
  return Vector2d(scaleX, scaleY);
}

Transform2d scaleTransformOutput(const Transform2d& transform, const Vector2d& scale) {
  Transform2d result = transform;
  result.data[0] *= scale.x;
  result.data[2] *= scale.x;
  result.data[4] *= scale.x;
  result.data[1] *= scale.y;
  result.data[3] *= scale.y;
  result.data[5] *= scale.y;
  return result;
}

void drawRectIntoMask(tiny_skia::Mask& mask, const Box2d& rect, const Transform2d& transform,
                      bool antialias) {
  const std::optional<tiny_skia::Rect> tinyRect = toTinyRect(rect);
  if (!tinyRect.has_value()) {
    return;
  }

  const tiny_skia::Path path = tiny_skia::Path::fromRect(*tinyRect);
  mask.fillPath(path, tiny_skia::FillRule::Winding, antialias, toTinyTransform(transform));
}

#ifndef DONNER_FILTERS_ENABLED
// When filters are disabled, FilterGraphExecutor.h is not included so PremultiplyRgba is not
// available. Provide a local copy for drawImage().
std::vector<std::uint8_t> PremultiplyRgba(std::span<const std::uint8_t> rgbaPixels) {
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
#endif  // !DONNER_FILTERS_ENABLED

#ifdef DONNER_FILTERS_ENABLED
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

/// Returns true if the filter graph contains spatial primitives (feOffset, feDisplacementMap)
/// that can shift content from outside the viewport into view.
bool graphHasSpatialShift(const components::FilterGraph& filterGraph) {
  using namespace components::filter_primitive;
  for (const components::FilterNode& node : filterGraph.nodes) {
    if (std::holds_alternative<Offset>(node.primitive)) {
      return true;
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
                                  const Transform2d& deviceFromFilter) {
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
#endif  // DONNER_FILTERS_ENABLED

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
  deviceFromLocalTransform_ = Transform2d();
  deviceFromLocalTransformStack_.clear();
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
  deviceFromLocalTransform_ = Transform2d();
  deviceFromLocalTransformStack_.clear();
  currentClipMask_.reset();
  clipStack_.clear();
}

void RendererTinySkia::setTransform(const Transform2d& transform) {
  if (!surfaceStack_.empty() && surfaceStack_.back().kind == SurfaceKind::PatternTile) {
    const Transform2d& rasterFromTile = surfaceStack_.back().patternRasterFromTile;
    deviceFromLocalTransform_ =
        scaleTransformOutput(transform, Vector2d(rasterFromTile.data[0], rasterFromTile.data[3]));
  } else if (!surfaceStack_.empty() && surfaceStack_.back().kind == SurfaceKind::FilterLayer) {
    const auto& frame = surfaceStack_.back();
    if (frame.filterBufferOffsetX != 0 || frame.filterBufferOffsetY != 0) {
      // Offset the transform so content at negative device coordinates renders into the
      // expanded filter buffer. Same pattern as PatternTile's rasterFromTile adjustment.
      deviceFromLocalTransform_ =
          transform * Transform2d::Translate(frame.filterBufferOffsetX, frame.filterBufferOffsetY);
    } else {
      deviceFromLocalTransform_ = transform;
    }
  } else {
    deviceFromLocalTransform_ = transform;
  }
}

void RendererTinySkia::pushTransform(const Transform2d& transform) {
  deviceFromLocalTransformStack_.push_back(deviceFromLocalTransform_);
  deviceFromLocalTransform_ = transform * deviceFromLocalTransform_;
}

void RendererTinySkia::popTransform() {
  if (deviceFromLocalTransformStack_.empty()) {
    return;
  }

  deviceFromLocalTransform_ = deviceFromLocalTransformStack_.back();
  deviceFromLocalTransformStack_.pop_back();
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

void RendererTinySkia::pushIsolatedLayer(double opacity, MixBlendMode blendMode) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::IsolatedLayer;
  frame.opacity = opacity;
  frame.blendMode = blendMode;
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
  compositePixmap(frame.pixmap, frame.opacity, frame.blendMode);
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
                                       const std::optional<Box2d>& filterRegion) {
#ifdef DONNER_FILTERS_ENABLED
  SurfaceFrame frame;
  frame.kind = SurfaceKind::FilterLayer;
  frame.filterGraph = filterGraph;
  frame.filterRegion = filterRegion;
  frame.deviceFromFilter = deviceFromLocalTransform_;

  const int viewportWidth = static_cast<int>(currentPixmap().width());
  const int viewportHeight = static_cast<int>(currentPixmap().height());

  // Compute the filter buffer dimensions. When the filter region's AABB in device space extends
  // beyond the viewport (e.g., due to skew or rotation placing content at negative coordinates),
  // expand the buffer so the SourceGraphic captures all content. This is needed because feOffset
  // or other spatial primitives may shift content into the viewport later.
  int bufferX0 = 0;
  int bufferY0 = 0;
  int width = viewportWidth;
  int height = viewportHeight;

  if (filterRegion.has_value()) {
    const Box2d deviceRegion = deviceFromLocalTransform_.transformBox(*filterRegion);
    const int regionX0 = static_cast<int>(std::floor(deviceRegion.topLeft.x));
    const int regionY0 = static_cast<int>(std::floor(deviceRegion.topLeft.y));
    const int regionX1 = static_cast<int>(std::ceil(deviceRegion.bottomRight.x));
    const int regionY1 = static_cast<int>(std::ceil(deviceRegion.bottomRight.y));

    // Expand buffer to cover the union of viewport and filter region AABB.
    bufferX0 = std::min(0, regionX0);
    bufferY0 = std::min(0, regionY0);
    const int bufferX1 = std::max(viewportWidth, regionX1);
    const int bufferY1 = std::max(viewportHeight, regionY1);
    width = bufferX1 - bufferX0;
    height = bufferY1 - bufferY0;

    // Cap to a reasonable maximum to avoid huge allocations.
    constexpr int kMaxBufferDim = 4096;
    width = std::min(width, kMaxBufferDim);
    height = std::min(height, kMaxBufferDim);
  }

  // Expand the buffer into negative device coordinates only when the filter graph contains
  // spatial primitives (feOffset, feDisplacementMap) that can shift content into the viewport
  // from outside. Without such primitives, content at negative coordinates would never be
  // visible, so expansion is unnecessary.
  if ((bufferX0 < 0 || bufferY0 < 0) && graphHasSpatialShift(filterGraph)) {
    constexpr int kMaxExpansion = 4096;
    frame.filterBufferOffsetX = std::min(-bufferX0, std::max(0, kMaxExpansion - viewportWidth));
    frame.filterBufferOffsetY = std::min(-bufferY0, std::max(0, kMaxExpansion - viewportHeight));
    width = viewportWidth + frame.filterBufferOffsetX;
    height = viewportHeight + frame.filterBufferOffsetY;
  } else {
    // No expansion needed — use viewport dimensions.
    width = viewportWidth;
    height = viewportHeight;
  }
  frame.pixmap = createTransparentPixmap(width, height);
  if (graphUsesStandardInput(filterGraph, components::FilterStandardInput::FillPaint)) {
    frame.fillPaintPixmap = createTransparentPixmap(width, height);
  }
  if (graphUsesStandardInput(filterGraph, components::FilterStandardInput::StrokePaint)) {
    frame.strokePaintPixmap = createTransparentPixmap(width, height);
  }

  // Per SVG spec, the rendering order is: paint → filter → clip-path → mask → opacity.
  // The SourceGraphic for the filter should be the element's unclipped content. Save and clear
  // the current clip mask so that content renders unclipped into the filter's offscreen buffer.
  // The clip mask is restored in popFilterLayer and applied when compositing the filter output.
  frame.savedClipMask = std::move(currentClipMask_);
  frame.savedClipStack = std::move(clipStack_);
  currentClipMask_.reset();
  clipStack_.clear();

  surfaceStack_.push_back(std::move(frame));

  // Apply the buffer offset to the current transform. setTransform (called by RendererDriver)
  // ran BEFORE this filter layer was pushed, so deviceFromLocalTransform_ doesn't include the
  // offset yet. Subsequent setTransform calls will pick up the offset from surfaceStack_.back(),
  // but we need to fix the already-set transform for the element being filtered.
  const auto& pushedFrame = surfaceStack_.back();
  if (pushedFrame.filterBufferOffsetX != 0 || pushedFrame.filterBufferOffsetY != 0) {
    deviceFromLocalTransform_ =
        deviceFromLocalTransform_ *
        Transform2d::Translate(pushedFrame.filterBufferOffsetX, pushedFrame.filterBufferOffsetY);
  }
#else
  (void)filterGraph;
  (void)filterRegion;
#endif
}

void RendererTinySkia::popFilterLayer() {
#ifdef DONNER_FILTERS_ENABLED
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::FilterLayer) {
    return;
  }

  SurfaceFrame frame = std::move(surfaceStack_.back());
  surfaceStack_.pop_back();

  // Restore the clip mask that was saved in pushFilterLayer. This allows the clip to be applied
  // to the filter output during compositing, implementing the SVG rendering order:
  // paint → filter → clip-path → mask → opacity.
  currentClipMask_ = std::move(frame.savedClipMask);
  clipStack_ = std::move(frame.savedClipStack);

  // Transform is maintained by RendererDriver via setTransform. The filter buffer offset
  // (if any) was applied in setTransform and is automatically removed when the surface is
  // popped, since setTransform checks surfaceStack_.back().

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
    const Transform2d& deviceFromFilter = frame.deviceFromFilter;
    const Box2d& filterRegion = *frame.filterRegion;

    // Compute local raster density from the filter transform basis vectors.
    const double scaleX =
        std::max(1.0, deviceFromFilter.transformVector(Vector2d(1.0, 0.0)).length());
    const double scaleY =
        std::max(1.0, deviceFromFilter.transformVector(Vector2d(0.0, 1.0)).length());

    const double blurPadding = computeBlurPadding(frame.filterGraph);
    const Box2d paddedRegion(filterRegion.topLeft - Vector2d(blurPadding, blurPadding),
                             filterRegion.bottomRight + Vector2d(blurPadding, blurPadding));

    const int localWidth = std::max(1, static_cast<int>(std::ceil(paddedRegion.width() * scaleX)));
    const int localHeight =
        std::max(1, static_cast<int>(std::ceil(paddedRegion.height() * scaleY)));

    // Allocate local-raster pixmap.
    tiny_skia::Pixmap localPixmap = createTransparentPixmap(localWidth, localHeight);
    if (localPixmap.width() == 0 || localPixmap.height() == 0) {
      goto device_space_fallback;
    }

    {
      // Resample device pixels into local filter coordinates.
      // Transform chain: device → filter (inverse of deviceFromFilter) → padded origin
      // (translate) → local raster pixels (scale).
      // Operator* convention: (A * B)(p) = B(A(p)), so A is applied first.
      const Transform2d filterFromDevice = deviceFromFilter.inverse();
      const Transform2d localFromDevice =
          filterFromDevice *
          Transform2d::Translate(-paddedRegion.topLeft.x, -paddedRegion.topLeft.y) *
          Transform2d::Scale(scaleX, scaleY);

      {
        tiny_skia::PixmapPaint resamplePaint;
        resamplePaint.opacity = 1.0f;
        resamplePaint.blendMode = tiny_skia::BlendMode::Source;
        resamplePaint.quality = tiny_skia::FilterQuality::Bilinear;

        auto localView = localPixmap.mutableView();
        tiny_skia::Painter::drawPixmap(localView, 0, 0, frame.pixmap.view(), resamplePaint,
                                       toTinyTransform(localFromDevice));
      }

      // Execute the filter graph in local raster space.
      const Transform2d localTransform = Transform2d::Scale(scaleX, scaleY);
      const Box2d localFilterRegion(
          Vector2d(blurPadding, blurPadding),
          Vector2d(blurPadding + filterRegion.width(), blurPadding + filterRegion.height()));

      ApplyFilterGraphToPixmap(localPixmap, frame.filterGraph, localTransform, localFilterRegion,
                               false);

      // Clip to the original filter region within the padded raster.
      ClipFilterOutputToRegion(localPixmap, localFilterRegion, localTransform);

      // Composite the filtered local raster back to the parent device pixmap.
      // Transform chain: local raster → filter coords (inverse scale + translate back) → device.
      // Operator* convention: (A * B)(p) = B(A(p)), so A is applied first.
      const Transform2d deviceFromLocal =
          Transform2d::Scale(1.0 / scaleX, 1.0 / scaleY) *
          Transform2d::Translate(paddedRegion.topLeft.x, paddedRegion.topLeft.y) * deviceFromFilter;

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
  device_space_fallback: {
    // When the filter buffer is offset (expanded to capture content at negative device
    // coordinates), adjust the deviceFromFilter transform to include the offset.
    const Transform2d bufferDeviceFromFilter =
        (frame.filterBufferOffsetX != 0 || frame.filterBufferOffsetY != 0)
            ? frame.deviceFromFilter *
                  Transform2d::Translate(frame.filterBufferOffsetX, frame.filterBufferOffsetY)
            : frame.deviceFromFilter;

    ApplyFilterGraphToPixmap(
        frame.pixmap, frame.filterGraph, bufferDeviceFromFilter, frame.filterRegion, true,
        frame.fillPaintPixmap.has_value() ? &*frame.fillPaintPixmap : nullptr,
        frame.strokePaintPixmap.has_value() ? &*frame.strokePaintPixmap : nullptr);
    ClipFilterOutputToRegion(frame.pixmap, frame.filterRegion, bufferDeviceFromFilter);

    {
      // Composite the filter result with the restored clip mask applied, so the clip-path
      // clips the filter output rather than the input.
      tiny_skia::PixmapPaint paint;
      paint.opacity = 1.0f;
      paint.blendMode = tiny_skia::BlendMode::SourceOver;
      paint.quality = tiny_skia::FilterQuality::Nearest;
      paint.unpremulStore = surfaceStack_.empty();

      const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
      auto pixmapView = currentPixmapView();

      if (frame.filterBufferOffsetX != 0 || frame.filterBufferOffsetY != 0) {
        // The filter buffer is expanded beyond the viewport. Extract the viewport-sized region
        // at the buffer offset and composite that. drawPixmap doesn't support negative dest
        // coordinates, so we copy the relevant region into a viewport-sized pixmap first.
        const int vpW = static_cast<int>(pixmapView.width());
        const int vpH = static_cast<int>(pixmapView.height());
        tiny_skia::Pixmap viewportRegion = createTransparentPixmap(vpW, vpH);
        const auto srcData = frame.pixmap.data();
        auto dstData = viewportRegion.data();
        const int bufW = static_cast<int>(frame.pixmap.width());
        const int bufH = static_cast<int>(frame.pixmap.height());
        const int ox = frame.filterBufferOffsetX;
        const int oy = frame.filterBufferOffsetY;
        for (int y = 0; y < vpH; ++y) {
          const int srcY = y + oy;
          if (srcY < 0 || srcY >= bufH) {
            continue;
          }
          // Copy the overlapping row segment.
          const int srcXStart = std::max(0, ox);
          const int srcXEnd = std::min(bufW, ox + vpW);
          if (srcXStart >= srcXEnd) {
            continue;
          }
          const int dstX = srcXStart - ox;
          const auto srcOff = static_cast<std::size_t>((srcY * bufW + srcXStart) * 4);
          const auto dstOff = static_cast<std::size_t>((y * vpW + dstX) * 4);
          const auto count = static_cast<std::size_t>((srcXEnd - srcXStart) * 4);
          std::memcpy(&dstData[dstOff], &srcData[srcOff], count);
        }
        tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, viewportRegion.view(), paint,
                                       tiny_skia::Transform::identity(), mask);
      } else {
        tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, frame.pixmap.view(), paint,
                                       tiny_skia::Transform::identity(), mask);
      }
    }
  }
  }
#endif  // DONNER_FILTERS_ENABLED
}

void RendererTinySkia::pushMask(const std::optional<Box2d>& maskBounds) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::MaskCapture;
  frame.maskBounds = maskBounds;
  frame.maskBoundsTransform = deviceFromLocalTransform_;
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

void RendererTinySkia::beginPatternTile(const Box2d& tileRect,
                                        const Transform2d& targetFromPattern) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::PatternTile;
  frame.savedTransform = deviceFromLocalTransform_;
  frame.savedTransformStack = deviceFromLocalTransformStack_;
  frame.savedClipMask = currentClipMask_;
  frame.savedClipStack = clipStack_;
  const Transform2d deviceFromPattern = frame.savedTransform * targetFromPattern;
  const Vector2d requestedRasterScale = patternRasterScaleForTransform(deviceFromPattern);
  const int pixelWidth =
      std::max(1, static_cast<int>(std::ceil(tileRect.width() * requestedRasterScale.x)));
  const int pixelHeight =
      std::max(1, static_cast<int>(std::ceil(tileRect.height() * requestedRasterScale.y)));
  const Vector2d rasterScale =
      effectivePatternRasterScale(tileRect, pixelWidth, pixelHeight, requestedRasterScale);
  frame.patternRasterFromTile = Transform2d::Scale(rasterScale);
  frame.targetFromPattern = targetFromPattern;
  frame.targetFromPattern.data[4] *= rasterScale.x;
  frame.targetFromPattern.data[5] *= rasterScale.y;
  frame.targetFromPattern =
      frame.targetFromPattern * Transform2d::Scale(1.0 / rasterScale.x, 1.0 / rasterScale.y);
  frame.pixmap = createTransparentPixmap(pixelWidth, pixelHeight);

  surfaceStack_.push_back(std::move(frame));

  deviceFromLocalTransform_ = surfaceStack_.back().patternRasterFromTile;
  deviceFromLocalTransformStack_.clear();
  currentClipMask_.reset();
  clipStack_.clear();
}

void RendererTinySkia::endPatternTile(bool forStroke) {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::PatternTile) {
    return;
  }

  SurfaceFrame frame = std::move(surfaceStack_.back());
  surfaceStack_.pop_back();

  deviceFromLocalTransform_ = frame.savedTransform;
  deviceFromLocalTransformStack_ = std::move(frame.savedTransformStack);
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
                                 toTinyTransform(deviceFromLocalTransform_), mask);
    if (fillPaintPixmap != nullptr) {
      auto fillPaintView = fillPaintPixmap->mutableView();
      tiny_skia::Painter::fillPath(fillPaintView, tinyPath, *fillPaint,
                                   toTinyFillRule(path.fillRule),
                                   toTinyTransform(deviceFromLocalTransform_), mask);
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
                                   toTinyTransform(deviceFromLocalTransform_), mask);
    if (strokePaintPixmap != nullptr) {
      auto strokePaintView = strokePaintPixmap->mutableView();
      tiny_skia::Painter::strokePath(strokePaintView, tinyPath, *strokePaint, tinyStroke,
                                     toTinyTransform(deviceFromLocalTransform_), mask);
    }
    if (usedPatternStroke) {
      patternStrokePaint_.reset();
    }
  }
}

void RendererTinySkia::drawRect(const Box2d& rect, const StrokeParams& stroke) {
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
                                 toTinyTransform(deviceFromLocalTransform_), mask);
    if (fillPaintPixmap != nullptr) {
      auto fillPaintView = fillPaintPixmap->mutableView();
      tiny_skia::Painter::fillRect(fillPaintView, *tinyRect, *fillPaint,
                                   toTinyTransform(deviceFromLocalTransform_), mask);
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
                                   toTinyTransform(deviceFromLocalTransform_), mask);
    if (strokePaintPixmap != nullptr) {
      auto strokePaintView = strokePaintPixmap->mutableView();
      tiny_skia::Painter::strokePath(strokePaintView, path, *strokePaint, tinyStroke,
                                     toTinyTransform(deviceFromLocalTransform_), mask);
    }
    if (usedPatternStroke) {
      patternStrokePaint_.reset();
    }
  }
}

void RendererTinySkia::drawEllipse(const Box2d& bounds, const StrokeParams& stroke) {
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
                                 toTinyTransform(deviceFromLocalTransform_), mask);
    if (fillPaintPixmap != nullptr) {
      auto fillPaintView = fillPaintPixmap->mutableView();
      tiny_skia::Painter::fillPath(fillPaintView, path, *fillPaint, tiny_skia::FillRule::Winding,
                                   toTinyTransform(deviceFromLocalTransform_), mask);
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
                                   toTinyTransform(deviceFromLocalTransform_), mask);
    if (strokePaintPixmap != nullptr) {
      auto strokePaintView = strokePaintPixmap->mutableView();
      tiny_skia::Painter::strokePath(strokePaintView, path, *strokePaint, tinyStroke,
                                     toTinyTransform(deviceFromLocalTransform_), mask);
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
  const Transform2d imageFromLocal = Transform2d::Scale(scaleX, scaleY) *
                                     Transform2d::Translate(params.targetRect.topLeft) *
                                     deviceFromLocalTransform_;

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

void RendererTinySkia::drawText(Registry& registry, const components::ComputedTextComponent& text,
                                const TextParams& params) {
#ifdef DONNER_TEXT_ENABLED
  if (currentPixmap().width() == 0 || currentPixmap().height() == 0) {
    return;
  }

  if (!registry.ctx().contains<TextEngine>()) {
    maybeWarnUnsupportedText();
    return;
  }

  auto& textEngine = registry.ctx().get<TextEngine>();

  // Use cached layout runs from ComputedTextGeometryComponent when available.
  std::vector<TextRun> runs;
  if (params.textRootEntity != entt::null) {
    if (const auto* cache =
            registry.try_get<components::ComputedTextGeometryComponent>(params.textRootEntity)) {
      runs = cache->runs;
    }
  }
  if (runs.empty()) {
    const TextLayoutParams layoutParams = toTextLayoutParams(params);
    runs = textEngine.layout(text, layoutParams);
  }

  float scale = 0.0f;
  const float fontSizePx = static_cast<float>(
      params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));

  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;

  // Compute text bounding box from glyph positions for objectBoundingBox gradient mapping.
  // Per the SVG spec, the objectBoundingBox for text uses em-box cells defined by font metrics
  // (ascent above baseline, |descent| below baseline), not the raw font size.
  Box2d textBounds;
  {
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for (size_t runIdx = 0; runIdx < runs.size(); ++runIdx) {
      const auto& run = runs[runIdx];

      // Resolve per-run font size (spans may override the text element's font size).
      float runFontSizePx = fontSizePx;
      if (runIdx < text.spans.size() && text.spans[runIdx].fontSize.value != 0.0) {
        runFontSizePx = static_cast<float>(text.spans[runIdx].fontSize.toPixels(
            params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));
      }

      // Get font metrics for this run to compute proper em-box vertical extent.
      float runScale = run.font ? textEngine.scaleForPixelHeight(run.font, runFontSizePx) : 0.0f;
      double emTop = static_cast<double>(runFontSizePx);  // fallback: full font size above baseline
      double emBottom = 0.0;                              // fallback: baseline
      if (run.font && runScale > 0.0f) {
        const FontVMetrics metrics = textEngine.fontVMetrics(run.font);
        // ascent is positive (above baseline), descent is negative (below baseline).
        // In SVG's y-down space: top = baseline - ascent*scale, bottom = baseline - descent*scale.
        emTop = static_cast<double>(metrics.ascent) * runScale;
        emBottom = -static_cast<double>(metrics.descent) * runScale;
      }

      for (const auto& glyph : run.glyphs) {
        if (glyph.glyphIndex == 0) {
          continue;
        }
        minX = std::min(minX, glyph.xPosition);
        maxX = std::max(maxX, glyph.xPosition + glyph.xAdvance);
        minY = std::min(minY, glyph.yPosition - emTop);
        maxY = std::max(maxY, glyph.yPosition + emBottom);
      }
    }
    if (minX < maxX && minY < maxY) {
      textBounds = Box2d({minX, minY}, {maxX, maxY});
    }
  }

  // Use makeFillPaint/makeStrokePaint to support gradients, patterns, and solid colors.
  // These read from paint_ (set by setPaint()) which the driver already populated.
  std::optional<tiny_skia::Paint> fillPaint = makeFillPaint(textBounds);
  const auto makeSolidPaint = [&](const css::Color& color, double opacityScale = 1.0) {
    tiny_skia::Paint paint = makeBasePaint(antialias_);
    paint.unpremulStore = surfaceStack_.empty();

    css::RGBA rgba = color.rgba();
    rgba.a = static_cast<uint8_t>(
        std::round(static_cast<double>(rgba.a) * params.opacity * paintOpacity_ * opacityScale));
    paint.shader = toTinyColor(rgba);
    return paint;
  };

  // Check if we have a stroke.
  const bool hasStroke = params.strokeParams.strokeWidth > 0.0;
  std::optional<tiny_skia::Paint> strokePaint;
  tiny_skia::Stroke tinyStroke;
  if (hasStroke) {
    strokePaint = makeStrokePaint(textBounds, params.strokeParams);
    tinyStroke.width = NarrowToFloat(params.strokeParams.strokeWidth);
    tinyStroke.miterLimit = NarrowToFloat(params.strokeParams.miterLimit);
    tinyStroke.lineCap = toTinyLineCap(params.strokeParams.lineCap);
    tinyStroke.lineJoin = toTinyLineJoin(params.strokeParams.lineJoin);
  }

  for (size_t runIndex = 0; runIndex < runs.size(); ++runIndex) {
    const auto& run = runs[runIndex];

    // Per-span font size: use the span's fontSize if set, otherwise the text element's.
    float spanFontSizePx = fontSizePx;
    if (runIndex < text.spans.size() && text.spans[runIndex].fontSize.value != 0.0) {
      spanFontSizePx = static_cast<float>(text.spans[runIndex].fontSize.toPixels(
          params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));
    }

    if (run.font != FontHandle()) {
      scale = textEngine.scaleForPixelHeight(run.font, spanFontSizePx);
    }

    const bool isBitmapFont = run.font && textEngine.isBitmapOnly(run.font);
    if (!isBitmapFont && scale == 0.0f) {
      continue;
    }

    std::optional<tiny_skia::Paint> spanFillPaint = fillPaint;
    std::optional<tiny_skia::Paint> spanStrokePaint = strokePaint;
    tiny_skia::Stroke spanTinyStroke = tinyStroke;
    if (runIndex < text.spans.size()) {
      const auto& span = text.spans[runIndex];
      const css::RGBA spanCurrentColor = paint_.currentColor.rgba();
      const float spanFillOpacity = NarrowToFloat(span.fillOpacity);
      const float spanStrokeOpacity = NarrowToFloat(span.strokeOpacity);

      if (const auto* solid = std::get_if<PaintServer::Solid>(&span.resolvedFill)) {
        // Per-span solid fill color.
        spanFillPaint = makeSolidPaint(
            css::Color(solid->color.resolve(spanCurrentColor, spanFillOpacity)), span.opacity);
      } else if (const auto* ref =
                     std::get_if<components::PaintResolvedReference>(&span.resolvedFill)) {
        // Per-span gradient/pattern fill. Uses the text element's bbox (textBounds)
        // for objectBoundingBox mapping, per SVG spec ("tspan doesn't have a bbox").
        const float combinedOpacity = spanFillOpacity * static_cast<float>(span.opacity);
        if (auto shader = instantiateGradientShader(*ref, textBounds, paint_.viewBox,
                                                    spanCurrentColor, combinedOpacity)) {
          tiny_skia::Paint paint = makeBasePaint(antialias_);
          paint.unpremulStore = surfaceStack_.empty();
          paint.shader = std::move(*shader);
          spanFillPaint = paint;
        } else if (patternFillPaint_.has_value()) {
          tiny_skia::Paint paint = makeBasePaint(antialias_);
          paint.unpremulStore = surfaceStack_.empty();
          paint.shader =
              tiny_skia::Pattern(patternFillPaint_->pixmap.view(), tiny_skia::SpreadMode::Repeat,
                                 tiny_skia::FilterQuality::Bilinear,
                                 NarrowToFloat(spanFillOpacity * static_cast<float>(span.opacity)),
                                 toTinyTransform(patternFillPaint_->targetFromPattern));
          spanFillPaint = paint;
        } else if (ref->fallback.has_value()) {
          spanFillPaint = makeSolidPaint(
              css::Color(ref->fallback->resolve(spanCurrentColor, spanFillOpacity)), span.opacity);
        } else {
          // Keep the inherited paint for non-gradient refs such as patterns.
        }
      } else if (span.opacity < 1.0 && spanFillPaint.has_value()) {
        // No explicit fill but has per-span opacity — re-apply with opacity.
        spanFillPaint = makeSolidPaint(params.fillColor, span.opacity);
      }

      spanTinyStroke.width = NarrowToFloat(span.strokeWidth);
      spanTinyStroke.miterLimit = NarrowToFloat(span.strokeMiterLimit);
      spanTinyStroke.lineCap = toTinyLineCap(span.strokeLinecap);
      spanTinyStroke.lineJoin = toTinyLineJoin(span.strokeLinejoin);

      if (span.strokeWidth > 0.0) {
        if (const auto* solid = std::get_if<PaintServer::Solid>(&span.resolvedStroke)) {
          spanStrokePaint = makeSolidPaint(
              css::Color(solid->color.resolve(spanCurrentColor, spanStrokeOpacity)), span.opacity);
        } else if (const auto* ref =
                       std::get_if<components::PaintResolvedReference>(&span.resolvedStroke)) {
          const float combinedOpacity = spanStrokeOpacity * static_cast<float>(span.opacity);
          if (auto shader = instantiateGradientShader(*ref, textBounds, paint_.viewBox,
                                                      spanCurrentColor, combinedOpacity)) {
            tiny_skia::Paint paint = makeBasePaint(antialias_);
            paint.unpremulStore = surfaceStack_.empty();
            paint.shader = std::move(*shader);
            spanStrokePaint = paint;
          } else if (patternStrokePaint_.has_value()) {
            tiny_skia::Paint paint = makeBasePaint(antialias_);
            paint.unpremulStore = surfaceStack_.empty();
            paint.shader = tiny_skia::Pattern(
                patternStrokePaint_->pixmap.view(), tiny_skia::SpreadMode::Repeat,
                tiny_skia::FilterQuality::Bilinear,
                NarrowToFloat(spanStrokeOpacity * static_cast<float>(span.opacity)),
                toTinyTransform(patternStrokePaint_->targetFromPattern));
            spanStrokePaint = paint;
          } else if (ref->fallback.has_value()) {
            spanStrokePaint = makeSolidPaint(
                css::Color(ref->fallback->resolve(spanCurrentColor, spanStrokeOpacity)),
                span.opacity);
          } else {
            // Keep the inherited paint for non-gradient refs such as patterns.
          }
        } else {
          spanStrokePaint.reset();
        }
      } else {
        spanStrokePaint.reset();
      }
    }

    for (const auto& glyph : run.glyphs) {
      if (glyph.glyphIndex == 0) {
        continue;  // .notdef glyph, skip.
      }

      Path glyphPath;
      if (!isBitmapFont) {
        glyphPath =
            textEngine.glyphOutline(run.font, glyph.glyphIndex, scale * glyph.fontSizeScale);
        if (glyph.stretchScaleX != 1.0f || glyph.stretchScaleY != 1.0f) {
          glyphPath = transformPath(glyphPath,
                                    Transform2d::Scale(glyph.stretchScaleX, glyph.stretchScaleY));
        }
      }

      // For bitmap fonts (color emoji), extract and draw the bitmap directly.
      if (glyphPath.empty()) {
        auto bitmap = textEngine.bitmapGlyph(run.font, glyph.glyphIndex, scale);
        // DEBUG
        if (bitmap) {
          // Premultiply alpha for correct blending.
          std::vector<uint8_t> premul = PremultiplyRgba(bitmap->rgbaPixels);
          auto maybePixmap = tiny_skia::Pixmap::fromVec(
              std::move(premul), tiny_skia::IntSize(static_cast<uint32_t>(bitmap->width),
                                                    static_cast<uint32_t>(bitmap->height)));
          if (!maybePixmap.has_value()) {
            continue;
          }

          // Compute target rect in document space: position with bearing, scaled size.
          const double targetX = glyph.xPosition + bitmap->bearingX;
          const double targetY = glyph.yPosition - bitmap->bearingY;
          const double targetW =
              static_cast<double>(bitmap->width) * bitmap->scale * glyph.stretchScaleX;
          const double targetH =
              static_cast<double>(bitmap->height) * bitmap->scale * glyph.stretchScaleY;

          // Use the same transform pattern as drawImage: Scale * Translate *
          // deviceFromLocalTransform_.
          const double imgScaleX = targetW / static_cast<double>(bitmap->width);
          const double imgScaleY = targetH / static_cast<double>(bitmap->height);
          const Transform2d imageFromLocal = Transform2d::Scale(imgScaleX, imgScaleY) *
                                             Transform2d::Translate(Vector2d(targetX, targetY)) *
                                             deviceFromLocalTransform_;

          tiny_skia::PixmapPaint paint;
          paint.opacity = NarrowToFloat(paintOpacity_);
          paint.blendMode = tiny_skia::BlendMode::SourceOver;
          paint.quality = tiny_skia::FilterQuality::Bilinear;
          paint.unpremulStore = surfaceStack_.empty();

          const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
          auto pixmapView = currentPixmapView();
          tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, maybePixmap->view(), paint,
                                         toTinyTransform(imageFromLocal), mask);
          continue;
        }
      }

      if (glyphPath.empty()) {
        continue;
      }

      // Place glyph geometry in document space, then let the renderer's current transform map it
      // to device space. This avoids relying on composed affine semantics that differ from
      // TinySkia's.
      Transform2d glyphFromLocal = Transform2d::Translate(glyph.xPosition, glyph.yPosition);
      if (glyph.rotateDegrees != 0.0) {
        glyphFromLocal =
            Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) *
            glyphFromLocal;
      }

      const tiny_skia::Path tinyPath = toTinyPath(transformPath(glyphPath, glyphFromLocal));
      auto pixmapView = currentPixmapView();

      // Fill.
      if (spanFillPaint) {
        tiny_skia::Painter::fillPath(pixmapView, tinyPath, *spanFillPaint,
                                     tiny_skia::FillRule::Winding,
                                     toTinyTransform(deviceFromLocalTransform_), mask);
      }

      // Stroke.
      if (spanStrokePaint) {
        tiny_skia::Painter::strokePath(pixmapView, tinyPath, *spanStrokePaint, spanTinyStroke,
                                       toTinyTransform(deviceFromLocalTransform_), mask);
      }
    }

    // Draw text-decoration lines. Per CSS Text Decoration §3, decoration uses the paint and
    // font metrics of the element that declared text-decoration, not the current span's.
    const bool hasSpan = runIndex < text.spans.size();
    const TextDecoration spanDecoration =
        hasSpan ? text.spans[runIndex].textDecoration : params.textDecoration;

    if (spanDecoration != TextDecoration::None && !run.glyphs.empty() && run.font) {
      const auto& span = text.spans[runIndex];

      // Use the declaring element's font-size for metrics (Category C fix).
      const float decoFontSizePx =
          span.decorationFontSizePx > 0.0f ? span.decorationFontSizePx : spanFontSizePx;
      const float decoScale = textEngine.scaleForPixelHeight(run.font, decoFontSizePx);
      const float decoEmScale = textEngine.scaleForEmToPixels(run.font, decoFontSizePx);

      const FontVMetrics vmetrics = textEngine.fontVMetrics(run.font);
      const int ascent = vmetrics.ascent;
      const int descent = vmetrics.descent;

      double fontUnderlinePos = 0.0;
      double fontUnderlineThick = 0.0;
      if (auto ul = textEngine.underlineMetrics(run.font)) {
        fontUnderlinePos = ul->position;
        fontUnderlineThick = ul->thickness;
      }
      double fontStrikePos = 0.0;
      double fontStrikeThick = 0.0;
      if (auto strike = textEngine.strikeoutMetrics(run.font)) {
        fontStrikePos = strike->position;
        fontStrikeThick = strike->thickness;
      }

      const double thickness = fontUnderlineThick > 0.0
                                   ? fontUnderlineThick * decoEmScale
                                   : static_cast<double>(ascent - descent) * decoScale / 18.0;

      // Resolve decoration fill paint from the declaring element (Category B fix).
      std::optional<tiny_skia::Paint> decoFillPaint;
      if (const auto* solid = std::get_if<PaintServer::Solid>(&span.resolvedDecorationFill)) {
        const css::RGBA spanCurrentColor = paint_.currentColor.rgba();
        const float fillOpacity = NarrowToFloat(span.decorationFillOpacity);
        decoFillPaint = makeSolidPaint(
            css::Color(solid->color.resolve(spanCurrentColor, fillOpacity)), span.opacity);
      } else if (const auto* ref = std::get_if<components::PaintResolvedReference>(
                     &span.resolvedDecorationFill)) {
        const css::RGBA spanCurrentColor = paint_.currentColor.rgba();
        const float combinedOpacity =
            NarrowToFloat(span.decorationFillOpacity) * static_cast<float>(span.opacity);
        if (auto shader = instantiateGradientShader(*ref, textBounds, paint_.viewBox,
                                                    spanCurrentColor, combinedOpacity)) {
          tiny_skia::Paint paint = makeBasePaint(antialias_);
          paint.unpremulStore = surfaceStack_.empty();
          paint.shader = std::move(*shader);
          decoFillPaint = paint;
        }
      }
      if (!decoFillPaint) {
        decoFillPaint = spanFillPaint;  // Fallback to span fill if no declaring element.
      }

      // Resolve decoration stroke paint (Category A fix).
      std::optional<tiny_skia::Paint> decoStrokePaint;
      if (const auto* solid = std::get_if<PaintServer::Solid>(&span.resolvedDecorationStroke)) {
        const css::RGBA spanCurrentColor = paint_.currentColor.rgba();
        const float strokeOpacity = NarrowToFloat(span.decorationStrokeOpacity);
        decoStrokePaint = makeSolidPaint(
            css::Color(solid->color.resolve(spanCurrentColor, strokeOpacity)), span.opacity);
      } else if (const auto* ref = std::get_if<components::PaintResolvedReference>(
                     &span.resolvedDecorationStroke)) {
        const css::RGBA spanCurrentColor = paint_.currentColor.rgba();
        const float combinedOpacity =
            NarrowToFloat(span.decorationStrokeOpacity) * static_cast<float>(span.opacity);
        if (auto shader = instantiateGradientShader(*ref, textBounds, paint_.viewBox,
                                                    spanCurrentColor, combinedOpacity)) {
          tiny_skia::Paint paint = makeBasePaint(antialias_);
          paint.unpremulStore = surfaceStack_.empty();
          paint.shader = std::move(*shader);
          decoStrokePaint = paint;
        }
      }

      const bool hasRotation = std::any_of(run.glyphs.begin(), run.glyphs.end(),
                                           [](const auto& g) { return g.rotateDegrees != 0.0; });

      for (TextDecoration decoType :
           {TextDecoration::Underline, TextDecoration::Overline, TextDecoration::LineThrough}) {
        if (!hasFlag(spanDecoration, decoType)) {
          continue;
        }

        double decoThickness = thickness;
        if (decoType == TextDecoration::LineThrough && fontStrikeThick > 0.0) {
          decoThickness = fontStrikeThick * decoEmScale;
        }

        double decoOffsetY = 0.0;
        if (decoType == TextDecoration::Underline) {
          decoOffsetY = fontUnderlinePos != 0.0 ? -fontUnderlinePos * decoEmScale
                                                : -static_cast<double>(descent) * decoScale * 0.4;
        } else if (decoType == TextDecoration::Overline) {
          decoOffsetY = -static_cast<double>(ascent) * decoScale;
        } else if (decoType == TextDecoration::LineThrough) {
          decoOffsetY = fontStrikePos != 0.0 ? -fontStrikePos * decoEmScale
                                             : -static_cast<double>(ascent) * decoScale * 0.35;
        }

        double decoTopY = decoOffsetY - decoThickness / 2.0;

        const bool hasMultipleDecorationLines =
            (hasFlag(spanDecoration, TextDecoration::Underline) ? 1 : 0) +
                (hasFlag(spanDecoration, TextDecoration::Overline) ? 1 : 0) +
                (hasFlag(spanDecoration, TextDecoration::LineThrough) ? 1 : 0) >
            1;
        if (span.decorationDeclarationCount == 1 && hasMultipleDecorationLines) {
          if (decoType == TextDecoration::Overline) {
            decoTopY += decoThickness * 1.5;
          } else if (decoType == TextDecoration::LineThrough) {
            decoTopY -= decoThickness;
          }
        }

        // Helper lambda to fill+stroke a decoration path.
        auto drawDecoPath = [&](const tiny_skia::Path& tinyPath) {
          auto pixmapView = currentPixmapView();
          if (decoFillPaint) {
            tiny_skia::Painter::fillPath(pixmapView, tinyPath, *decoFillPaint,
                                         tiny_skia::FillRule::Winding,
                                         toTinyTransform(deviceFromLocalTransform_), mask);
          }
          if (decoStrokePaint && span.decorationStrokeWidth > 0.0) {
            tiny_skia::Stroke stroke;
            stroke.width = NarrowToFloat(span.decorationStrokeWidth);
            tiny_skia::Painter::strokePath(pixmapView, tinyPath, *decoStrokePaint, stroke,
                                           toTinyTransform(deviceFromLocalTransform_), mask);
          }
        };

        if (hasRotation) {
          const auto isRenderedGlyph = [](const auto& glyph) {
            return glyph.glyphIndex != 0 && glyph.xAdvance > 0.0;
          };

          for (size_t glyphIndex = 0; glyphIndex < run.glyphs.size(); ++glyphIndex) {
            const auto& glyph = run.glyphs[glyphIndex];
            if (glyph.glyphIndex == 0 || glyph.xAdvance <= 0.0) {
              continue;
            }

            double segmentWidth = glyph.xAdvance;
            for (size_t nextIndex = glyphIndex + 1; nextIndex < run.glyphs.size(); ++nextIndex) {
              const auto& nextGlyph = run.glyphs[nextIndex];
              if (!isRenderedGlyph(nextGlyph)) {
                continue;
              }

              segmentWidth = std::min(segmentWidth, nextGlyph.xPosition - glyph.xPosition);
              break;
            }

            if (segmentWidth <= 0.0) {
              continue;
            }

            Path segPath = PathBuilder()
                               .moveTo(Vector2d(0.0, decoTopY))
                               .lineTo(Vector2d(segmentWidth, decoTopY))
                               .lineTo(Vector2d(segmentWidth, decoTopY + decoThickness))
                               .lineTo(Vector2d(0.0, decoTopY + decoThickness))
                               .closePath()
                               .build();

            Transform2d segTransform = Transform2d::Translate(glyph.xPosition, glyph.yPosition);
            if (glyph.rotateDegrees != 0.0) {
              segTransform =
                  Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) *
                  segTransform;
            }

            drawDecoPath(toTinyPath(transformPath(segPath, segTransform)));
          }
        } else {
          const auto isRenderedGlyph = [](const auto& glyph) {
            return glyph.glyphIndex != 0 && glyph.xAdvance > 0.0;
          };

          const auto firstGlyph =
              std::find_if(run.glyphs.begin(), run.glyphs.end(), isRenderedGlyph);
          const auto lastGlyph =
              std::find_if(run.glyphs.rbegin(), run.glyphs.rend(), isRenderedGlyph);
          if (firstGlyph == run.glyphs.end() || lastGlyph == run.glyphs.rend()) {
            continue;
          }

          const double baselineY = firstGlyph->yPosition;
          const bool sameBaseline =
              std::all_of(run.glyphs.begin(), run.glyphs.end(), [&](const auto& glyph) {
                return !isRenderedGlyph(glyph) || std::abs(glyph.yPosition - baselineY) < 1e-6;
              });

          if (sameBaseline) {
            const double x0 = firstGlyph->xPosition;
            const double x1 = lastGlyph->xPosition + lastGlyph->xAdvance;
            const double y = baselineY + decoTopY;
            Path decoPath = PathBuilder()
                                .moveTo(Vector2d(x0, y))
                                .lineTo(Vector2d(x1, y))
                                .lineTo(Vector2d(x1, y + decoThickness))
                                .lineTo(Vector2d(x0, y + decoThickness))
                                .closePath()
                                .build();
            drawDecoPath(toTinyPath(decoPath));
          } else {
            PathBuilder decoBuilder;
            for (size_t glyphIndex = 0; glyphIndex < run.glyphs.size(); ++glyphIndex) {
              const auto& glyph = run.glyphs[glyphIndex];
              if (!isRenderedGlyph(glyph)) {
                continue;
              }

              const double x0 = glyph.xPosition;
              double x1 = glyph.xPosition + glyph.xAdvance;
              for (size_t nextIndex = glyphIndex + 1; nextIndex < run.glyphs.size(); ++nextIndex) {
                const auto& nextGlyph = run.glyphs[nextIndex];
                if (!isRenderedGlyph(nextGlyph)) {
                  continue;
                }

                x1 = std::min(x1, nextGlyph.xPosition);
                break;
              }

              if (x1 <= x0) {
                continue;
              }

              const double y = glyph.yPosition + decoTopY;
              decoBuilder.moveTo(Vector2d(x0, y));
              decoBuilder.lineTo(Vector2d(x1, y));
              decoBuilder.lineTo(Vector2d(x1, y + decoThickness));
              decoBuilder.lineTo(Vector2d(x0, y + decoThickness));
              decoBuilder.closePath();
            }

            if (!decoBuilder.empty()) {
              drawDecoPath(toTinyPath(decoBuilder.build()));
            }
          }
        }
      }
    }
  }

  // Consume pattern paints after use, matching drawPath/drawRect/drawEllipse behavior.
  if (patternFillPaint_.has_value()) {
    patternFillPaint_.reset();
  }
  if (patternStrokePaint_.has_value()) {
    patternStrokePaint_.reset();
  }
#else
  (void)text;
  (void)params;
  maybeWarnUnsupportedText();
#endif
}

RendererBitmap RendererTinySkia::takeSnapshot() const {
  RendererBitmap snapshot;
  snapshot.dimensions = Vector2i(width(), height());
  snapshot.rowBytes = static_cast<std::size_t>(width()) * 4u;
  // Tiny-skia's compose paths to the top-level frame buffer use
  // `unpremulStore = surfaceStack_.empty()` — every semi-transparent pixel that
  // lands in `frame_` is stored unpremultiplied in float space, preserving
  // precision at low alpha values. The frame buffer is therefore consistently
  // unpremultiplied (alpha=255 pixels are identical under either convention).
  // Report that truthfully so downstream callers (e.g.
  // `compositor::BuildImageResource`) don't re-unpremultiply.
  snapshot.alphaType = AlphaType::Unpremultiplied;
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

std::unique_ptr<RendererInterface> RendererTinySkia::createOffscreenInstance() const {
  return std::make_unique<RendererTinySkia>(verbose_);
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
              << "\n  currentTransform=" << deviceFromLocalTransform_
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
      drawRectIntoMask(*rectMask, *clip.clipRect, deviceFromLocalTransform_, antialias_);
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

    const Transform2d clipPathTransform =
        clip.clipPathUnitsTransform * shape.parentFromEntity * deviceFromLocalTransform_;
    if (verbose_) {
      const Box2d pathBounds = shape.path.bounds();
      std::cout << "\n  shape layer=" << shape.layer << " bounds=" << pathBounds
                << "\n    parentFromEntity=" << shape.parentFromEntity
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

std::optional<tiny_skia::Paint> RendererTinySkia::makeFillPaint(const Box2d& bounds) {
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

std::optional<tiny_skia::Paint> RendererTinySkia::makeStrokePaint(const Box2d& bounds,
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

void RendererTinySkia::compositePixmap(const tiny_skia::Pixmap& pixmap, double opacity,
                                       MixBlendMode blendMode) {
  compositePixmapInto(currentPixmap(), pixmap, opacity, blendMode);
}

/// Map donner MixBlendMode to tiny_skia::BlendMode.
static tiny_skia::BlendMode toTinyBlendMode(MixBlendMode mode) {
  switch (mode) {
    case MixBlendMode::Normal: return tiny_skia::BlendMode::SourceOver;
    case MixBlendMode::Multiply: return tiny_skia::BlendMode::Multiply;
    case MixBlendMode::Screen: return tiny_skia::BlendMode::Screen;
    case MixBlendMode::Overlay: return tiny_skia::BlendMode::Overlay;
    case MixBlendMode::Darken: return tiny_skia::BlendMode::Darken;
    case MixBlendMode::Lighten: return tiny_skia::BlendMode::Lighten;
    case MixBlendMode::ColorDodge: return tiny_skia::BlendMode::ColorDodge;
    case MixBlendMode::ColorBurn: return tiny_skia::BlendMode::ColorBurn;
    case MixBlendMode::HardLight: return tiny_skia::BlendMode::HardLight;
    case MixBlendMode::SoftLight: return tiny_skia::BlendMode::SoftLight;
    case MixBlendMode::Difference: return tiny_skia::BlendMode::Difference;
    case MixBlendMode::Exclusion: return tiny_skia::BlendMode::Exclusion;
    case MixBlendMode::Hue: return tiny_skia::BlendMode::Hue;
    case MixBlendMode::Saturation: return tiny_skia::BlendMode::Saturation;
    case MixBlendMode::Color: return tiny_skia::BlendMode::Color;
    case MixBlendMode::Luminosity: return tiny_skia::BlendMode::Luminosity;
  }
  return tiny_skia::BlendMode::SourceOver;
}

void RendererTinySkia::compositePixmapInto(tiny_skia::Pixmap& destination,
                                           const tiny_skia::Pixmap& pixmap, double opacity,
                                           MixBlendMode blendMode) {
  if (opacity <= 0.0 || pixmap.width() == 0 || pixmap.height() == 0) {
    return;
  }

  tiny_skia::PixmapPaint paint;
  paint.opacity = NarrowToFloat(opacity);
  paint.blendMode = toTinyBlendMode(blendMode);
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
