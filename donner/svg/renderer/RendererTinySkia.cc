#include "donner/svg/renderer/RendererTinySkia.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Length.h"
#include "donner/base/MathUtils.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#ifdef DONNER_FILTERS_ENABLED
#include "donner/svg/renderer/FilterGraphExecutor.h"
#endif
#include "donner/svg/renderer/ImageSampling.h"
#include "donner/svg/renderer/PatternTile.h"
#include "donner/svg/renderer/PixelFormatUtils.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererTinySkiaCache.h"
#ifdef DONNER_TEXT_ENABLED
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/renderer/PlacedTextGeometry.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"
#include "donner/svg/text/TextLayoutParams.h"
#endif
#include "tiny_skia/Painter.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/shaders/Shaders.h"

namespace donner::svg {

namespace {

std::optional<int> CheckedPixelDimension(double logicalDimension, double devicePixelRatio) {
  const double pixelDimension = logicalDimension * devicePixelRatio;
  if (!std::isfinite(logicalDimension) || !std::isfinite(devicePixelRatio) ||
      logicalDimension < 0.0 || devicePixelRatio <= 0.0 || !std::isfinite(pixelDimension) ||
      pixelDimension > static_cast<double>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  return static_cast<int>(pixelDimension);
}

#ifdef DONNER_TEXT_ENABLED
TextLayoutParams toTextLayoutParams(const TextParams& params) {
  TextLayoutParams layoutParams;
  layoutParams.fontFamilies = params.fontFamilies;
  layoutParams.fontSize = params.fontSize;
  layoutParams.viewBox = params.viewBox;
  layoutParams.fontMetrics = params.fontMetrics;
  layoutParams.textAnchor = params.textAnchor;
  layoutParams.writingMode = params.writingMode;
  layoutParams.letterSpacingPx = params.letterSpacingPx;
  layoutParams.wordSpacingPx = params.wordSpacingPx;
  layoutParams.textLength = params.textLength;
  layoutParams.lengthAdjust = params.lengthAdjust;
  layoutParams.inlineSizePx = params.inlineSizePx;
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

enum class TinyPathCloseBehavior : uint8_t {
  Preserve,
  EndWithLine,
};

tiny_skia::Path toTinyPath(const Path& spline,
                           TinyPathCloseBehavior closeBehavior = TinyPathCloseBehavior::Preserve) {
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
        if (closeBehavior == TinyPathCloseBehavior::Preserve) {
          builder.close();
        } else {
          const Vector2d& subpathStart = points[command.pointIndex];
          builder.lineTo(NarrowToFloat(subpathStart.x), NarrowToFloat(subpathStart.y));
        }
        break;
      }
    }
  }

  return builder.finish().value_or(tiny_skia::Path());
}

/// Marks a registry whose conversion-cache invalidation listeners are already connected.
///
/// The sentinel lives in the registry's context store, so it dies with the registry: a later
/// registry allocated at the same address correctly misses it and gets its own listeners.
/// Pointer identity on the registry alone could not tell those two cases apart.
struct CacheListenersInstalled {};

void OnComputedPathChanged(Registry& registry, Entity entity) {
  // entt allows `remove` on a component the entity does not hold; it is a cheap no-op there.
  registry.remove<components::TinySkiaPathCacheComponent>(entity);
}

void OnLoadedImageChanged(Registry& registry, Entity entity) {
  registry.remove<components::TinySkiaImageCacheComponent>(entity);
}

/// Connects the listeners that drop a cached conversion when its source changes or goes away.
/// Idempotent, and called before any cache entry is installed on \p registry.
///
/// Wiring on that first installation rather than at frame entry is sufficient, even though a
/// backend that wires later than the render tree's rebuild would normally risk a stale entry:
/// no cache entry on \p registry can predate the connection, because the same call path that
/// creates the first one connects the listeners first. Every source mutation that could
/// invalidate an entry therefore happens after the listeners are attached, including the ones
/// a later frame's render-tree rebuild fires. A signal missed before the first entry exists has
/// nothing to invalidate.
///
/// \p checkedThisFrame memoizes the last registry the caller verified, so the context-store
/// lookup runs once per frame per document instead of once per draw. The memo is reset at every
/// frame boundary, which is what makes it safe: a registry cannot be destroyed and a new one
/// allocated at the same address in the middle of a frame while the caller holds entity handles
/// into it, so unlike a persistent pointer cache the memo cannot confuse two registries. The
/// context-store sentinel remains the authority across frames.
void EnsureCacheInvalidationWired(Registry& registry, const Registry*& checkedThisFrame) {
  if (checkedThisFrame == &registry) {
    return;
  }

  checkedThisFrame = &registry;
  if (registry.ctx().contains<CacheListenersInstalled>()) {
    return;
  }

  registry.ctx().emplace<CacheListenersInstalled>();
  // Materialize both cache pools here, while the registry is quiescent. The listeners below
  // remove those components, and a removal on an entity that never held one still has to reach
  // the pool, which would otherwise be created for the first time from inside a destruction
  // signal. Creating it up front removes the need to reason about that at all.
  static_cast<void>(registry.storage<components::TinySkiaPathCacheComponent>());
  static_cast<void>(registry.storage<components::TinySkiaImageCacheComponent>());
  registry.on_update<components::ComputedPathComponent>().connect<&OnComputedPathChanged>();
  registry.on_destroy<components::ComputedPathComponent>().connect<&OnComputedPathChanged>();
  registry.on_update<components::LoadedImageComponent>().connect<&OnLoadedImageChanged>();
  registry.on_destroy<components::LoadedImageComponent>().connect<&OnLoadedImageChanged>();
  // Leaving the connections attached across renderer destruction is intentional: these are
  // free functions with no captured state, so they are safe to outlive any renderer, and the
  // connections die with the registry.
}

/// Selects the cache slot that holds \p closeBehavior's conversion.
///
/// Written as an exhaustive switch, like the other `toTiny*` mappings in this file, rather than
/// as a two-way ternary. A ternary silently folds any future close behavior into the `openedPath`
/// slot, which would serve one behavior's outline for another - a wrong-pixels bug that no test
/// of the existing two behaviors can see. Here `-Wswitch` names the missing case instead.
std::optional<tiny_skia::Path>& cacheSlotFor(components::TinySkiaPathCacheComponent& cache,
                                             TinyPathCloseBehavior closeBehavior) {
  switch (closeBehavior) {
    case TinyPathCloseBehavior::Preserve: return cache.closedPath;
    case TinyPathCloseBehavior::EndWithLine: return cache.openedPath;
  }

  UTILS_UNREACHABLE();
}

/// Returns `shape.path` in tiny-skia form, converting it on a cache miss.
///
/// When the shape carries a source entity the conversion is memoized on that entity and the
/// returned reference points into the cache component, which stays valid until the entity's
/// geometry changes. Without a source entity (overlay drawing, test harnesses) the conversion
/// lands in \p scratch, which the caller must keep alive for as long as it uses the result.
const tiny_skia::Path& ResolveTinyPath(const PathShape& shape, TinyPathCloseBehavior closeBehavior,
                                       tiny_skia::Path& scratch, const Registry*& checkedThisFrame,
                                       RendererTinySkiaFrameCounters& counters) {
  EntityHandle source = shape.sourceEntity;
  if (!source) {
    ++counters.pathConversions;
    scratch = toTinyPath(shape.pathOrEmpty(), closeBehavior);
    return scratch;
  }

  EnsureCacheInvalidationWired(*source.registry(), checkedThisFrame);
  auto& cache = source.get_or_emplace<components::TinySkiaPathCacheComponent>();
  std::optional<tiny_skia::Path>& slot = cacheSlotFor(cache, closeBehavior);
  if (!slot.has_value()) {
    ++counters.pathConversions;
    slot = toTinyPath(shape.pathOrEmpty(), closeBehavior);
  }
  return *slot;
}

/// Returns \p image's pixels premultiplied, converting them on a cache miss.
///
/// Mirrors \ref ResolveTinyPath: the payload is memoized on \p source when there is one, and
/// otherwise converted into \p scratch, which the caller must keep alive for as long as it uses
/// the result. A cached payload whose dimensions no longer match \p image is reconverted rather
/// than sampled at the wrong extent.
std::span<const std::uint8_t> ResolvePremultipliedImage(const ImageResource& image,
                                                        EntityHandle source,
                                                        std::vector<std::uint8_t>& scratch,
                                                        const Registry*& checkedThisFrame,
                                                        RendererTinySkiaFrameCounters& counters) {
  if (!source) {
    ++counters.imagePremultiplies;
    PremultiplyRgbaInto(image.data, scratch);
    return scratch;
  }

  EnsureCacheInvalidationWired(*source.registry(), checkedThisFrame);
  auto& cache = source.get_or_emplace<components::TinySkiaImageCacheComponent>();
  if (cache.premultiplied.size() != image.data.size() || cache.width != image.width ||
      cache.height != image.height) {
    ++counters.imagePremultiplies;
    PremultiplyRgbaInto(image.data, cache.premultiplied);
    cache.width = image.width;
    cache.height = image.height;
  }
  return cache.premultiplied;
}

ImageRendering ResolveImageRendering(const ImageParams& params) {
  if (params.imageRendering == ImageRendering::Auto && params.imageRenderingPixelated) {
    return ImageRendering::Pixelated;
  }

  return params.imageRendering;
}

tiny_skia::FilterQuality FilterQualityForImageRendering(ImageRendering imageRendering) {
  if (imageRendering == ImageRendering::CrispEdges ||
      imageRendering == ImageRendering::OptimizeSpeed) {
    return tiny_skia::FilterQuality::Nearest;
  }

  return tiny_skia::FilterQuality::Bilinear;
}

Transform2d MakeDestFromSourceTransform(const Box2d& targetRect, int sourceWidth, int sourceHeight,
                                        const Transform2d& destFromLocal) {
  return Transform2d::Scale(targetRect.width() / static_cast<double>(sourceWidth),
                            targetRect.height() / static_cast<double>(sourceHeight)) *
         Transform2d::Translate(targetRect.topLeft) * destFromLocal;
}

struct PixelatedAxisScale {
  double roundedScale = 0.0;
  std::int64_t integerScale = 1;
  bool finite = false;
};

PixelatedAxisScale ResolvePixelatedAxisScale(const Vector2d& transformedUnitAxis) {
  PixelatedAxisScale result;
  result.roundedScale = std::floor(transformedUnitAxis.length() + 0.5);
  result.finite = std::isfinite(result.roundedScale);
  if (result.finite) {
    result.integerScale =
        std::max<std::int64_t>(1, static_cast<std::int64_t>(std::min<double>(
                                      result.roundedScale, kMaxImageSamplingDimension)));
  }
  return result;
}

struct PixelatedSamplingPlan {
  Transform2d destFromSource;
  std::int64_t integerScaleX = 1;
  std::int64_t integerScaleY = 1;
  std::int64_t intermediateWidth = 0;
  std::int64_t intermediateHeight = 0;
  bool needsIntermediate = false;
  bool canMaterializeIntermediate = false;
};

PixelatedSamplingPlan MakePixelatedSamplingPlan(const Box2d& targetRect, int sourceWidth,
                                                int sourceHeight,
                                                const Transform2d& destFromLocal) {
  PixelatedSamplingPlan result;
  result.destFromSource =
      MakeDestFromSourceTransform(targetRect, sourceWidth, sourceHeight, destFromLocal);

  const PixelatedAxisScale scaleX =
      ResolvePixelatedAxisScale(result.destFromSource.transformVector(Vector2d(1.0, 0.0)));
  const PixelatedAxisScale scaleY =
      ResolvePixelatedAxisScale(result.destFromSource.transformVector(Vector2d(0.0, 1.0)));
  const bool finiteScale = scaleX.finite && scaleY.finite;
  result.integerScaleX = finiteScale ? scaleX.integerScale : 1;
  result.integerScaleY = finiteScale ? scaleY.integerScale : 1;
  result.intermediateWidth = static_cast<std::int64_t>(sourceWidth) * result.integerScaleX;
  result.intermediateHeight = static_cast<std::int64_t>(sourceHeight) * result.integerScaleY;
  result.needsIntermediate = result.integerScaleX > 1 || result.integerScaleY > 1;
  result.canMaterializeIntermediate =
      finiteScale && scaleX.roundedScale <= kMaxImageSamplingDimension &&
      scaleY.roundedScale <= kMaxImageSamplingDimension &&
      result.intermediateWidth <= kMaxImageSamplingDimension &&
      result.intermediateHeight <= kMaxImageSamplingDimension &&
      result.intermediateWidth * result.intermediateHeight <= kMaxImageSamplingSurfacePixels;
  return result;
}

std::optional<tiny_skia::Pixmap> CreatePixelatedIntermediate(const tiny_skia::PixmapView& source,
                                                             const PixelatedSamplingPlan& plan) {
  if (!plan.needsIntermediate || !plan.canMaterializeIntermediate) {
    return std::nullopt;
  }

  std::optional<tiny_skia::Pixmap> intermediate =
      tiny_skia::Pixmap::fromSize(static_cast<std::uint32_t>(plan.intermediateWidth),
                                  static_cast<std::uint32_t>(plan.intermediateHeight));
  if (!intermediate.has_value()) {
    return std::nullopt;
  }

  tiny_skia::PixmapPaint nearest;
  nearest.quality = tiny_skia::FilterQuality::Nearest;
  nearest.blendMode = tiny_skia::BlendMode::Source;
  tiny_skia::MutablePixmapView intermediateView = intermediate->mutableView();
  tiny_skia::Painter::drawPixmap(
      intermediateView, 0, 0, source, nearest,
      toTinyTransform(Transform2d::Scale(static_cast<double>(plan.integerScaleX),
                                         static_cast<double>(plan.integerScaleY))));
  return intermediate;
}

void DrawProceduralPixelatedImage(const tiny_skia::PixmapView& source, int sourceWidth,
                                  int sourceHeight, const Transform2d& destFromSource,
                                  tiny_skia::Pixmap& destination, tiny_skia::PixmapPaint paint,
                                  const tiny_skia::Mask* mask, bool verbose) {
  std::vector<std::uint8_t> proceduralPixels = RasterizeImagePremultiplied(
      source.data(), sourceWidth, sourceHeight, destFromSource,
      static_cast<int>(destination.width()), static_cast<int>(destination.height()),
      ImageRendering::Pixelated);
  std::optional<tiny_skia::Pixmap> procedural = tiny_skia::Pixmap::fromVec(
      std::move(proceduralPixels), tiny_skia::IntSize(destination.width(), destination.height()));
  if (!procedural.has_value()) {
    if (verbose) {
      std::cerr << "RendererTinySkia: pixelated image exceeds the raster surface budget\n";
    }
    return;
  }

  tiny_skia::MutablePixmapView destinationView = destination.mutableView();
  tiny_skia::Painter::drawPixmap(destinationView, 0, 0, procedural->view(), paint,
                                 toTinyTransform(Transform2d()), mask);
}

// `transformPath` now lives in the shared (text-gated) PlacedTextGeometry header
// so both backends share one definition.

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
  // Apply the `transform-origin` pivot in the same `Translate(-origin)·M·Translate(origin)` order
  // used for shapes (LayoutSystem::getEntityFromParentTransform, #609). Donner's `operator*` is
  // left-first (`A * B` applies A, then B), so the pivot-out `Translate(-origin)` must come first
  // and the pivot-back `Translate(origin)` last. The reversed order pushes the gradient center off
  // the shape, collapsing scaled radial gradients to a single stop color (#621).
  return Transform2d::Translate(-origin) * parentFromEntity * Transform2d::Translate(origin);
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

  // Paints inherited through `context-fill` / `context-stroke` are evaluated in the context
  // element's space: objectBoundingBox units resolve against the context element's bounding box,
  // and the gradient is remapped from the context element's user space into path-local space.
  const Box2d& referenceBounds = ref.contextRemap ? ref.contextRemap->contextBounds : pathBounds;

  constexpr double kDegenerateBBoxTolerance = 1e-6;
  if (objectBoundingBox && (NearZero(referenceBounds.width(), kDegenerateBBoxTolerance) ||
                            NearZero(referenceBounds.height(), kDegenerateBBoxTolerance))) {
    return std::nullopt;
  }

  Transform2d pathFromGradientUnits;
  if (objectBoundingBox) {
    pathFromGradientUnits = resolveGradientTransform(
        handle.try_get<components::ComputedLocalTransformComponent>(), kUnitPathBounds);

    const Transform2d objectBoundingBoxFromUnitBox =
        Transform2d::Scale(referenceBounds.size()) *
        Transform2d::Translate(referenceBounds.topLeft);
    pathFromGradientUnits = pathFromGradientUnits * objectBoundingBoxFromUnitBox;
  } else {
    pathFromGradientUnits = resolveGradientTransform(
        handle.try_get<components::ComputedLocalTransformComponent>(), viewBox);
  }

  if (ref.contextRemap) {
    // Gradient units -> context element user space, then context -> path local space.
    pathFromGradientUnits = pathFromGradientUnits * ref.contextRemap->entityFromContextTransform;
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

  const tiny_skia::Transform shaderTransform = toTinyTransform(pathFromGradientUnits);

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

bool masksEqual(const tiny_skia::Mask& lhs, const tiny_skia::Mask& rhs) {
  const std::span<const std::uint8_t> lhsData = lhs.data();
  const std::span<const std::uint8_t> rhsData = rhs.data();
  return lhs.size() == rhs.size() && lhsData.size() == rhsData.size() &&
         std::equal(lhsData.begin(), lhsData.end(), rhsData.begin());
}

/// True when a paint borrows its pixels from a pattern tile.
///
/// Those pixels are rebuilt per use and live outside the paint, so coverage recorded under one
/// cannot be shown to still describe the next draw. Such a draw is left to rasterize.
bool paintBorrowsPattern(const tiny_skia::Paint& paint) {
  return std::holds_alternative<tiny_skia::Pattern>(paint.shader);
}

/// Draws one fill or stroke pass, replaying retained coverage when it still applies.
///
/// @param slot Retained storage for this pass, or null to rasterize unconditionally.
/// @param key Inputs the recorded coverage depends on.
/// @param pixmap Surface the pass draws onto.
/// @param mask Clip mask in effect, applied per blit by the pipeline rather than baked into
///   coverage, so a replay must be handed the same one a fresh draw would get.
/// @param stats Counters describing what happened.
/// @param drawFresh Performs the ordinary draw.
/// @param drawCapturing Performs the same draw through a capture, returning false when the
///   draw could not be recorded. The pixels are painted either way.
template <typename DrawFresh, typename DrawCapturing>
void drawRetainablePass(RetainedSpanSlot* slot, const RetainedSpanKey& key,
                        tiny_skia::MutablePixmapView& pixmap, const tiny_skia::Mask* mask,
                        RetainedSpanStats& stats, DrawFresh&& drawFresh,
                        DrawCapturing&& drawCapturing) {
  if (slot == nullptr) {
    ++stats.bypassedDraws;
    drawFresh();
    return;
  }

  if (slot->valid && slot->key == key) {
    if (tiny_skia::SpanCapture::replay(pixmap, slot->capture.spans(), slot->capture.paint(),
                                       mask)) {
      ++stats.replayedDraws;
      return;
    }

    // The recording was refused, which the surface-size guard does for runs that were recorded
    // against a differently sized surface. Falling through to a real draw is what keeps a
    // refusal from turning into a missing shape.
    ++stats.refusedReplays;
    slot->invalidate();
  }

  if (slot->valid) {
    ++stats.invalidatedDraws;
    slot->invalidate();
  }

  const bool recorded = drawCapturing(slot->capture);
  if (recorded) {
    slot->valid = true;
    slot->key = key;
    ++stats.capturedDraws;
  } else {
    // Nothing to replay, and the draw will be just as unrecordable next frame, so give the
    // storage back rather than charge the document for it.
    slot->capture.release();
    ++stats.unrecordableDraws;
  }
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

int BoundedFloorToInt(double value, int minimum, int maximum) {
  if (std::isnan(value)) {
    return 0;
  }
  if (value <= static_cast<double>(minimum)) {
    return minimum;
  }
  if (value >= static_cast<double>(maximum)) {
    return maximum;
  }
  return static_cast<int>(std::floor(value));
}

std::optional<int> CheckedFilterRasterDimension(double value) {
  if (!std::isfinite(value) || value <= 0.0 ||
      value > static_cast<double>(components::kMaximumFilterSurfaceDimension)) {
    return std::nullopt;
  }
  return std::max(1, static_cast<int>(std::ceil(value)));
}

bool graphHasSpatialShift(const components::FilterGraph& filterGraph);
bool isEligibleForTransformedBlurPath(const components::FilterGraph& filterGraph);
bool shouldUseTransformedBlurPath(const components::FilterGraph& filterGraph,
                                  const Transform2d& deviceFromFilter);
double computeBlurPadding(const components::FilterGraph& filterGraph);

struct FilterBufferMetrics {
  int width = 0;
  int height = 0;
  int offsetX = 0;
  int offsetY = 0;
};

FilterBufferMetrics ComputeFilterBufferMetrics(const components::FilterGraph& filterGraph,
                                               const std::optional<Box2d>& filterRegion,
                                               const Transform2d& deviceFromFilter,
                                               int viewportWidth, int viewportHeight) {
  constexpr int kMaxExpansion = 4096;
  int bufferX0 = 0;
  int bufferY0 = 0;
  if (filterRegion.has_value()) {
    const Box2d deviceRegion = deviceFromFilter.transformBox(*filterRegion);
    bufferX0 = std::min(0, BoundedFloorToInt(deviceRegion.topLeft.x, -kMaxExpansion, 0));
    bufferY0 = std::min(0, BoundedFloorToInt(deviceRegion.topLeft.y, -kMaxExpansion, 0));
  }

  FilterBufferMetrics result{viewportWidth, viewportHeight, 0, 0};
  if ((bufferX0 >= 0 && bufferY0 >= 0) || !graphHasSpatialShift(filterGraph)) {
    return result;
  }
  result.offsetX = std::min(-bufferX0, std::max(0, kMaxExpansion - viewportWidth));
  result.offsetY = std::min(-bufferY0, std::max(0, kMaxExpansion - viewportHeight));
  result.width = viewportWidth + result.offsetX;
  result.height = viewportHeight + result.offsetY;
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(result.width) * static_cast<std::uint64_t>(result.height);
  if (pixels > components::kMaximumFilterSurfacePixels) {
    return {viewportWidth, viewportHeight, 0, 0};
  }
  return result;
}

bool CanUseLocalFilterRaster(const components::FilterGraph& filterGraph,
                             const std::optional<Box2d>& filterRegion,
                             const Transform2d& deviceFromFilter, bool fullExecutionFits) {
  if (!filterRegion.has_value() || filterRegion->width() <= 0.0 || filterRegion->height() <= 0.0 ||
      NearZero(deviceFromFilter.determinant())) {
    return false;
  }
  return isEligibleForTransformedBlurPath(filterGraph) &&
         (shouldUseTransformedBlurPath(filterGraph, deviceFromFilter) || !fullExecutionFits);
}

struct LocalFilterRasterGeometry {
  double scaleX = 1.0;
  double scaleY = 1.0;
  double blurPadding = 0.0;
  Box2d paddedRegion;
  int width = 0;
  int height = 0;
};

std::optional<LocalFilterRasterGeometry> ComputeLocalFilterRasterGeometry(
    const components::FilterGraph& filterGraph, const std::optional<Box2d>& filterRegion,
    const Transform2d& deviceFromFilter, bool fullExecutionFits) {
  if (!CanUseLocalFilterRaster(filterGraph, filterRegion, deviceFromFilter, fullExecutionFits)) {
    return std::nullopt;
  }
  const double scaleX =
      std::max(1.0, deviceFromFilter.transformVector(Vector2d(1.0, 0.0)).length());
  const double scaleY =
      std::max(1.0, deviceFromFilter.transformVector(Vector2d(0.0, 1.0)).length());
  const double blurPadding = computeBlurPadding(filterGraph);
  const Box2d paddedRegion(filterRegion->topLeft - Vector2d(blurPadding, blurPadding),
                           filterRegion->bottomRight + Vector2d(blurPadding, blurPadding));
  const std::optional<int> width = CheckedFilterRasterDimension(paddedRegion.width() * scaleX);
  const std::optional<int> height = CheckedFilterRasterDimension(paddedRegion.height() * scaleY);
  if (!width || !height) {
    return std::nullopt;
  }
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(*width) * static_cast<std::uint64_t>(*height);
  if (pixels > components::kMaximumFilterSurfacePixels) {
    return std::nullopt;
  }
  return LocalFilterRasterGeometry{scaleX, scaleY, blurPadding, paddedRegion, *width, *height};
}

std::optional<std::uint64_t> ComputeLocalFilterPixels(const components::FilterGraph& filterGraph,
                                                      const std::optional<Box2d>& filterRegion,
                                                      const Transform2d& deviceFromFilter,
                                                      bool fullExecutionFits) {
  const std::optional<LocalFilterRasterGeometry> geometry = ComputeLocalFilterRasterGeometry(
      filterGraph, filterRegion, deviceFromFilter, fullExecutionFits);
  if (!geometry.has_value()) {
    return std::nullopt;
  }
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(geometry->width) * static_cast<std::uint64_t>(geometry->height);
  if (!components::FilterGraphFitsExecutionBudget(
          filterGraph, pixels, components::FilterMemoryModel::CpuFloatNamedResults)) {
    return std::nullopt;
  }
  return pixels;
}

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
  if (retainedSpansEnabled_) {
    // Connect the geometry-invalidation listener before the driver instantiates the render
    // tree, so a shape whose outline changed between draws drops its recording before this
    // frame could replay it.
    EnsureRetainedSpanInvalidationWired(document.registry());
  } else {
    // Retained entries live on the document, not on the renderer that made them, so a renderer
    // that stops retaining has to hand them back. Otherwise turning retention off leaves the
    // document holding coverage nothing will ever replay.
    ClearRetainedSpans(document.registry());
  }

  RendererDriver driver(*this, verbose_);
  driver.draw(document);
}

void RendererTinySkia::beginFrame(const RenderViewport& viewport) {
  viewport_ = viewport;
  const int pixelWidth =
      CheckedPixelDimension(viewport.size.x, viewport.devicePixelRatio).value_or(0);
  const int pixelHeight =
      CheckedPixelDimension(viewport.size.y, viewport.devicePixelRatio).value_or(0);

  // Keep the frame buffer's allocation across frames. A renderer that draws the
  // same viewport repeatedly (editor, compositor, animation loop) otherwise
  // releases and re-acquires the whole buffer every frame, which costs a
  // malloc/free pair plus a first-touch page fault for every page of a
  // multi-megabyte buffer. The clear is not an optimization target: the buffer
  // must start transparent, so a retained buffer is explicitly zeroed and the
  // output is identical either way.
  const bool frameSizeUnchanged = pixelWidth > 0 && pixelHeight > 0 &&
                                  frame_.width() == static_cast<std::uint32_t>(pixelWidth) &&
                                  frame_.height() == static_cast<std::uint32_t>(pixelHeight);
  if (frameSizeUnchanged) {
    frame_.fill(tiny_skia::Color::transparent);
  } else {
    frame_ = createTransparentPixmap(pixelWidth, pixelHeight);
  }
  deviceFromLocalTransform_ = Transform2d();
  deviceFromLocalTransformStack_.clear();
  currentClipMask_.reset();
  clipStack_.clear();
  surfaceStack_.clear();
  filterLayerStack_.clear();
  rejectedFilterDepth_ = 0;
  if (ownsFilterExecutionBudget_) {
    filterExecutionBudget_->reset();
  }
  patternFillPaint_.reset();
  patternStrokePaint_.reset();
  frameCounters_ = RendererTinySkiaFrameCounters();
  // Both the counters and the listener-wiring memo describe one frame only. Dropping the memo
  // here is what bounds its lifetime to a frame, which is the property that makes comparing
  // registry addresses safe; see `EnsureCacheInvalidationWired`.
  cacheWiringCheckedRegistry_ = nullptr;

  ++frameIndex_;
  retainedSpanStats_ = RetainedSpanStats();
  clipEpoch_ = 0;
  clipEpochStack_.clear();
  if (frame_.size() != previousFrameSize_) {
    // The remembered clip masks are sized for the surface they were built against, so they can
    // never match again once it changes. Dropping them here is what keeps a resizing viewport
    // from holding one full-surface mask per depth per size.
    clipEpochSlots_.clear();
    previousFrameSize_ = frame_.size();
  }
}

void RendererTinySkia::endFrame() {
  if (!surfaceStack_.empty() && verbose_) {
    std::cerr << "RendererTinySkia: unbalanced surface stack at endFrame\n";
  }

  surfaceStack_.clear();
  filterLayerStack_.clear();
  rejectedFilterDepth_ = 0;
  deviceFromLocalTransform_ = Transform2d();
  deviceFromLocalTransformStack_.clear();
  currentClipMask_.reset();
  clipStack_.clear();
  cacheWiringCheckedRegistry_ = nullptr;
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
  clipEpochStack_.push_back(clipEpoch_);

  if (rejectedFilterDepth_ != 0) {
    return;
  }

  std::optional<tiny_skia::Mask> clipMask = buildClipMask(clip);
  if (!clipMask.has_value()) {
    return;
  }

  if (currentClipMask_.has_value()) {
    intersectMaskInPlace(*clipMask, *currentClipMask_);
  }

  currentClipMask_ = std::move(clipMask);
  // `clipEpochStack_` already holds the epoch this clip replaced, so the clip's own depth is
  // one below the stack's size, and the outermost clip is depth zero.
  clipEpoch_ = assignClipEpoch(clipEpochStack_.size() - 1);
}

void RendererTinySkia::popClip() {
  if (clipStack_.empty()) {
    currentClipMask_.reset();
    clipEpoch_ = 0;
    return;
  }

  currentClipMask_ = std::move(clipStack_.back());
  clipStack_.pop_back();
  clipEpoch_ = clipEpochStack_.back();
  clipEpochStack_.pop_back();
}

std::uint64_t RendererTinySkia::assignClipEpoch(std::size_t depth) {
  if (!retainedSpansEnabled_ || !currentClipMask_.has_value()) {
    return 0;
  }

  // Remembering one mask per depth is what makes the identity useful: a document's clip stack
  // is rebuilt in the same order every frame, so an unchanged clip meets its own previous mask
  // and keeps its identity, while any change takes a new one. Past the cap the identity is
  // fresh every time, so deeply clipped shapes rasterize rather than letting a document choose
  // how many full-surface masks this renderer holds.
  if (depth >= kMaxRetainedClipDepth) {
    return nextClipEpoch_++;
  }

  if (clipEpochSlots_.size() <= depth) {
    clipEpochSlots_.resize(depth + 1);
  }

  ClipEpochSlot& slot = clipEpochSlots_[depth];
  if (slot.epoch != 0 && slot.mask.has_value() && masksEqual(*slot.mask, *currentClipMask_)) {
    return slot.epoch;
  }

  slot.mask = currentClipMask_;
  slot.epoch = nextClipEpoch_++;
  return slot.epoch;
}

RendererTinySkia::RetainedTarget RendererTinySkia::retainedTargetFor(
    const EntityHandle& sourceEntity) {
  // Coverage is retained only for a draw that goes straight to the root surface: a layer,
  // mask, or pattern-tile surface is rebuilt with the thing that owns it, so a recording made
  // against one describes a surface that no longer exists. A draw with no source entity (a
  // replayed snapshot, an overlay, a test harness) has no identity to key on.
  if (!retainedSpansEnabled_ || !surfaceStack_.empty() || !sourceEntity) {
    return RetainedTarget();
  }

  Registry& registry = *sourceEntity.registry();
  EnsureRetainedSpanInvalidationWired(registry);

  RetainedSpanDocumentState& state = RetainedSpanStateFor(registry, retainedSpanBudgetBytes_);
  retainedSpanStats_.documentDisabled = state.disabled;
  retainedSpanStats_.evictions = state.evictions;
  if (state.disabled) {
    return RetainedTarget();
  }

  if (frameToken_ == 0 || frameTokenIndex_ != frameIndex_) {
    // Take this frame's identity from the document rather than from a renderer-local counter:
    // entries live on the document, so two renderers drawing it would otherwise agree on frame
    // numbers and each would look to the other like a second draw of the same shape.
    frameToken_ = ++state.frameCounter;
    frameTokenIndex_ = frameIndex_;
  }

  RetainedSpansComponent& entry = sourceEntity.get_or_emplace<RetainedSpansComponent>();
  if (entry.drawFrame != frameToken_) {
    entry.drawFrame = frameToken_;
    entry.drawsThisFrame = 0;
  }

  ++entry.drawsThisFrame;
  if (entry.drawsThisFrame > 1 && !entry.ambiguous) {
    // One entity, several draws in one frame: an instanced shape drawn through a shadow tree,
    // or a shape whose `paint-order` splits fill and stroke into separate draws. A single entry
    // can only describe one of them, and alternating between them would re-capture on every
    // draw, so this entity stops retaining.
    entry.ambiguous = true;
    state.liveBytes -= std::min(state.liveBytes, entry.chargedBytes);
    entry.chargedBytes = 0;
    entry.fill = RetainedSpanSlot();
    entry.stroke = RetainedSpanSlot();
  }

  if (entry.ambiguous) {
    return RetainedTarget();
  }

  entry.lastUsedFrame = frameToken_;
  return RetainedTarget{&entry, &state};
}

void RendererTinySkia::pushIsolatedLayer(double opacity, MixBlendMode blendMode) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::IsolatedLayer;
  frame.opacity = opacity;
  frame.blendMode = blendMode;
  if (rejectedFilterDepth_ != 0) {
    frame.allocationRejected = true;
    surfaceStack_.push_back(std::move(frame));
    return;
  }
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
  if (frame.allocationRejected) {
    return;
  }
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

#ifdef DONNER_FILTERS_ENABLED
std::optional<RendererTinySkia::FilterAdmission> RendererTinySkia::admitFilterLayer(
    const components::FilterGraph& filterGraph, const std::optional<Box2d>& filterRegion,
    const Transform2d& deviceFromFilter, int viewportWidth, int viewportHeight) {
  const FilterBufferMetrics buffer = ComputeFilterBufferMetrics(
      filterGraph, filterRegion, deviceFromFilter, viewportWidth, viewportHeight);
  const bool usesFillPaint =
      graphUsesStandardInput(filterGraph, components::FilterStandardInput::FillPaint);
  const bool usesStrokePaint =
      graphUsesStandardInput(filterGraph, components::FilterStandardInput::StrokePaint);
  const std::uint64_t surfaceCount =
      1u + static_cast<std::uint64_t>(usesFillPaint) + static_cast<std::uint64_t>(usesStrokePaint);
  if (buffer.width <= 0 || buffer.height <= 0) {
    filterExecutionBudget_->reject();
    return std::nullopt;
  }

  const std::uint64_t pixelCount =
      static_cast<std::uint64_t>(buffer.width) * static_cast<std::uint64_t>(buffer.height);
  if (pixelCount > components::kMaximumFilterSurfacePixels ||
      pixelCount > std::numeric_limits<std::uint64_t>::max() / 4u / surfaceCount) {
    filterExecutionBudget_->reject();
    return std::nullopt;
  }
  const bool fullExecutionFits = components::FilterGraphFitsExecutionBudget(
      filterGraph, pixelCount, components::FilterMemoryModel::CpuFloatNamedResults);
  const std::optional<std::uint64_t> localPixelCount =
      ComputeLocalFilterPixels(filterGraph, filterRegion, deviceFromFilter, fullExecutionFits);
  if (!fullExecutionFits && !localPixelCount.has_value()) {
    filterExecutionBudget_->reject();
    return std::nullopt;
  }

  std::uint64_t executionPixelCount = pixelCount;
  std::uint64_t captureBytes = pixelCount * 4u * surfaceCount;
  if (localPixelCount.has_value()) {
    executionPixelCount =
        fullExecutionFits ? std::max(pixelCount, *localPixelCount) : *localPixelCount;
    captureBytes += *localPixelCount * 4u;
  }
  auto reservation = filterExecutionBudget_->reserve(
      filterGraph, executionPixelCount, components::FilterMemoryModel::CpuFloatNamedResults,
      captureBytes);
  if (!reservation.has_value()) {
    return std::nullopt;
  }
  return FilterAdmission{buffer.width,  buffer.height,   buffer.offsetX,     buffer.offsetY,
                         usesFillPaint, usesStrokePaint, !fullExecutionFits, *reservation};
}

bool RendererTinySkia::initializeFilterSurface(SurfaceFrame& frame,
                                               const FilterAdmission& admission) {
  frame.pixmap = createTransparentPixmap(admission.width, admission.height);
  if (admission.usesFillPaint) {
    frame.fillPaintPixmap = createTransparentPixmap(admission.width, admission.height);
  }
  if (admission.usesStrokePaint) {
    frame.strokePaintPixmap = createTransparentPixmap(admission.width, admission.height);
  }
  const bool allocationFailed = frame.pixmap.width() == 0 || frame.pixmap.height() == 0 ||
                                (admission.usesFillPaint && !frame.fillPaintPixmap.has_value()) ||
                                (admission.usesStrokePaint && !frame.strokePaintPixmap.has_value());
  if (!allocationFailed) {
    return true;
  }
  filterExecutionBudget_->release(frame.filterReservation);
  filterExecutionBudget_->reject();
  return false;
}
#endif  // DONNER_FILTERS_ENABLED

void RendererTinySkia::pushFilterLayer(const components::FilterGraph& filterGraph,
                                       const std::optional<Box2d>& filterRegion) {
#ifdef DONNER_FILTERS_ENABLED
  if (rejectedFilterDepth_ != 0) {
    filterLayerStack_.push_back(false);
    ++rejectedFilterDepth_;
    return;
  }

  SurfaceFrame frame;
  frame.kind = SurfaceKind::FilterLayer;
  frame.filterGraph = filterGraph;
  frame.filterRegion = filterRegion;
  frame.deviceFromFilter = deviceFromLocalTransform_;

  const int viewportWidth = static_cast<int>(currentPixmap().width());
  const int viewportHeight = static_cast<int>(currentPixmap().height());
  const std::optional<FilterAdmission> admission = admitFilterLayer(
      filterGraph, filterRegion, frame.deviceFromFilter, viewportWidth, viewportHeight);
  if (!admission.has_value()) {
    filterLayerStack_.push_back(false);
    ++rejectedFilterDepth_;
    return;
  }
  frame.filterReservation = admission->reservation;
  frame.localRasterRequiredForBudget = admission->localRasterRequired;
  frame.filterBufferOffsetX = admission->bufferOffsetX;
  frame.filterBufferOffsetY = admission->bufferOffsetY;
  filterLayerStack_.push_back(true);
  if (!initializeFilterSurface(frame, *admission)) {
    filterLayerStack_.back() = false;
    ++rejectedFilterDepth_;
    return;
  }

  // Per SVG spec, the rendering order is: paint → filter → clip-path → mask → opacity.
  // The SourceGraphic for the filter should be the element's unclipped content. Save and clear
  // the current clip mask so that content renders unclipped into the filter's offscreen buffer.
  // The clip mask is restored in popFilterLayer and applied when compositing the filter output.
  frame.savedClipMask = std::move(currentClipMask_);
  frame.savedClipStack = std::move(clipStack_);
  frame.savedClipEpoch = clipEpoch_;
  frame.savedClipEpochStack = std::move(clipEpochStack_);
  currentClipMask_.reset();
  clipStack_.clear();
  clipEpoch_ = 0;
  clipEpochStack_.clear();

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

#ifdef DONNER_FILTERS_ENABLED
bool RendererTinySkia::compositeTransformedFilter(SurfaceFrame& frame) {
  const std::optional<LocalFilterRasterGeometry> geometry =
      ComputeLocalFilterRasterGeometry(frame.filterGraph, frame.filterRegion,
                                       frame.deviceFromFilter, !frame.localRasterRequiredForBudget);
  if (!geometry.has_value()) {
    return false;
  }

  tiny_skia::Pixmap localPixmap = createTransparentPixmap(geometry->width, geometry->height);
  if (localPixmap.width() == 0 || localPixmap.height() == 0) {
    return false;
  }
  const Transform2d filterFromDevice = frame.deviceFromFilter.inverse();
  const Transform2d localFromDevice =
      filterFromDevice *
      Transform2d::Translate(-geometry->paddedRegion.topLeft.x, -geometry->paddedRegion.topLeft.y) *
      Transform2d::Scale(geometry->scaleX, geometry->scaleY);
  tiny_skia::PixmapPaint resamplePaint =
      makePixmapPaint(localPixmap, tiny_skia::FilterQuality::Bilinear);
  resamplePaint.opacity = 1.0f;
  resamplePaint.blendMode = tiny_skia::BlendMode::Source;
  auto localView = localPixmap.mutableView();
  tiny_skia::Painter::drawPixmap(localView, 0, 0, frame.pixmap.view(), resamplePaint,
                                 toTinyTransform(localFromDevice));

  const Transform2d localFromFilter = Transform2d::Scale(geometry->scaleX, geometry->scaleY);
  const Box2d localFilterRegion(Vector2d(geometry->blurPadding, geometry->blurPadding),
                                Vector2d(geometry->blurPadding + frame.filterRegion->width(),
                                         geometry->blurPadding + frame.filterRegion->height()));
  ApplyFilterGraphToPixmap(localPixmap, frame.filterGraph, localFromFilter, localFilterRegion,
                           false);
  ClipFilterOutputToRegion(localPixmap, localFilterRegion, localFromFilter);

  const Transform2d deviceFromLocal =
      Transform2d::Scale(1.0 / geometry->scaleX, 1.0 / geometry->scaleY) *
      Transform2d::Translate(geometry->paddedRegion.topLeft.x, geometry->paddedRegion.topLeft.y) *
      frame.deviceFromFilter;
  tiny_skia::PixmapPaint compositePaint =
      makePixmapPaint(currentPixmap(), tiny_skia::FilterQuality::Bilinear);
  compositePaint.opacity = 1.0f;
  compositePaint.blendMode = tiny_skia::BlendMode::SourceOver;
  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
  auto pixmapView = currentPixmapView();
  tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, localPixmap.view(), compositePaint,
                                 toTinyTransform(deviceFromLocal), mask);
  return true;
}

tiny_skia::Pixmap RendererTinySkia::extractFilterViewport(const SurfaceFrame& frame, int width,
                                                          int height) {
  tiny_skia::Pixmap viewport = createTransparentPixmap(width, height);
  const auto source = frame.pixmap.data();
  auto destination = viewport.data();
  const int sourceWidth = static_cast<int>(frame.pixmap.width());
  const int sourceHeight = static_cast<int>(frame.pixmap.height());
  for (int y = 0; y < height; ++y) {
    const int sourceY = y + frame.filterBufferOffsetY;
    if (sourceY < 0 || sourceY >= sourceHeight) {
      continue;
    }
    const int sourceXStart = std::max(0, frame.filterBufferOffsetX);
    const int sourceXEnd = std::min(sourceWidth, frame.filterBufferOffsetX + width);
    if (sourceXStart >= sourceXEnd) {
      continue;
    }
    const int destinationX = sourceXStart - frame.filterBufferOffsetX;
    const auto sourceOffset = static_cast<std::size_t>((sourceY * sourceWidth + sourceXStart) * 4);
    const auto destinationOffset = static_cast<std::size_t>((y * width + destinationX) * 4);
    const auto count = static_cast<std::size_t>((sourceXEnd - sourceXStart) * 4);
    std::memcpy(&destination[destinationOffset], &source[sourceOffset], count);
  }
  return viewport;
}

void RendererTinySkia::compositeDeviceFilter(SurfaceFrame& frame) {
  const bool hasOffset = frame.filterBufferOffsetX != 0 || frame.filterBufferOffsetY != 0;
  const Transform2d bufferDeviceFromFilter =
      hasOffset ? frame.deviceFromFilter *
                      Transform2d::Translate(frame.filterBufferOffsetX, frame.filterBufferOffsetY)
                : frame.deviceFromFilter;
  ApplyFilterGraphToPixmap(
      frame.pixmap, frame.filterGraph, bufferDeviceFromFilter, frame.filterRegion, true,
      frame.fillPaintPixmap.has_value() ? &*frame.fillPaintPixmap : nullptr,
      frame.strokePaintPixmap.has_value() ? &*frame.strokePaintPixmap : nullptr);
  ClipFilterOutputToRegion(frame.pixmap, frame.filterRegion, bufferDeviceFromFilter);

  tiny_skia::PixmapPaint paint =
      makePixmapPaint(currentPixmap(), tiny_skia::FilterQuality::Nearest);
  paint.opacity = 1.0f;
  paint.blendMode = tiny_skia::BlendMode::SourceOver;
  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
  auto pixmapView = currentPixmapView();
  if (hasOffset) {
    tiny_skia::Pixmap viewport = extractFilterViewport(frame, static_cast<int>(pixmapView.width()),
                                                       static_cast<int>(pixmapView.height()));
    tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, viewport.view(), paint,
                                   tiny_skia::Transform::identity(), mask);
    return;
  }
  tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, frame.pixmap.view(), paint,
                                 tiny_skia::Transform::identity(), mask);
}
#endif  // DONNER_FILTERS_ENABLED

void RendererTinySkia::popFilterLayer() {
#ifdef DONNER_FILTERS_ENABLED
  if (filterLayerStack_.empty()) {
    return;
  }
  const bool layerAccepted = filterLayerStack_.back();
  filterLayerStack_.pop_back();
  if (!layerAccepted) {
    if (rejectedFilterDepth_ != 0) {
      --rejectedFilterDepth_;
    }
    return;
  }
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
  clipEpoch_ = frame.savedClipEpoch;
  clipEpochStack_ = std::move(frame.savedClipEpochStack);

  if (compositeTransformedFilter(frame)) {
    filterExecutionBudget_->release(frame.filterReservation);
    return;
  }
  if (frame.localRasterRequiredForBudget) {
    filterExecutionBudget_->reject();
    filterExecutionBudget_->release(frame.filterReservation);
    return;
  }
  compositeDeviceFilter(frame);
  filterExecutionBudget_->release(frame.filterReservation);
#endif  // DONNER_FILTERS_ENABLED
}

void RendererTinySkia::pushMask(const std::optional<Box2d>& maskBounds, MaskType maskType) {
  SurfaceFrame frame;
  frame.kind = SurfaceKind::MaskCapture;
  frame.maskBounds = maskBounds;
  frame.maskBoundsTransform = deviceFromLocalTransform_;
  frame.maskType = maskType;
  if (rejectedFilterDepth_ != 0) {
    frame.allocationRejected = true;
    surfaceStack_.push_back(std::move(frame));
    return;
  }
  frame.pixmap = createTransparentPixmap(static_cast<int>(currentPixmap().width()),
                                         static_cast<int>(currentPixmap().height()));
  surfaceStack_.push_back(std::move(frame));
}

void RendererTinySkia::transitionMaskToContent() {
  if (surfaceStack_.empty() || surfaceStack_.back().kind != SurfaceKind::MaskCapture) {
    return;
  }

  SurfaceFrame& frame = surfaceStack_.back();
  if (frame.allocationRejected) {
    frame.kind = SurfaceKind::MaskContent;
    return;
  }
  const tiny_skia::MaskType tinyMaskType = frame.maskType == MaskType::Alpha
                                               ? tiny_skia::MaskType::Alpha
                                               : tiny_skia::MaskType::Luminance;
  frame.maskAlpha = tiny_skia::Mask::fromPixmap(frame.pixmap.view(), tinyMaskType);

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

bool RendererTinySkia::beginPatternTile(const Box2d& tileRect,
                                        const Transform2d& targetFromPattern) {
  if (rejectedFilterDepth_ != 0) {
    return false;
  }

  SurfaceFrame frame;
  frame.kind = SurfaceKind::PatternTile;
  frame.savedTransform = deviceFromLocalTransform_;
  const Transform2d deviceFromPattern = targetFromPattern * frame.savedTransform;
  const std::optional<PatternTileRasterMetrics> rasterMetrics =
      ComputePatternTileRasterMetrics(tileRect, deviceFromPattern);
  if (!rasterMetrics.has_value()) {
    return false;
  }

  frame.patternRasterFromTile = Transform2d::Scale(rasterMetrics->rasterFromPatternScale);
  frame.targetFromPattern =
      TargetFromPatternRaster(targetFromPattern, rasterMetrics->rasterFromPatternScale);
  frame.pixmap = createTransparentPixmap(rasterMetrics->pixelWidth, rasterMetrics->pixelHeight);
  if (frame.pixmap.width() == 0 || frame.pixmap.height() == 0) {
    return false;
  }

  // Hand the live clip and transform state to the frame by move. Every entry of
  // `clipStack_` owns a surface-sized alpha mask, so copying the stack here
  // duplicated one buffer per nesting level for every pattern tile the document
  // draws, only to clear the originals three lines later. The moves are placed
  // after the last early return, so a rejected tile still leaves the renderer's
  // state exactly as it found it.
  frame.savedTransformStack = std::move(deviceFromLocalTransformStack_);
  frame.savedClipMask = std::move(currentClipMask_);
  frame.savedClipStack = std::move(clipStack_);
  frame.savedClipEpoch = clipEpoch_;
  frame.savedClipEpochStack = std::move(clipEpochStack_);

  // Tile-content draws must not consume the outer element's pending pattern shaders (a shape
  // inside the tile would otherwise pick them up as its own fill/stroke and reset them).
  frame.savedPatternFillPaint = std::move(patternFillPaint_);
  frame.savedPatternStrokePaint = std::move(patternStrokePaint_);
  patternFillPaint_.reset();
  patternStrokePaint_.reset();

  surfaceStack_.push_back(std::move(frame));

  deviceFromLocalTransform_ = surfaceStack_.back().patternRasterFromTile;
  deviceFromLocalTransformStack_.clear();
  currentClipMask_.reset();
  clipStack_.clear();
  clipEpoch_ = 0;
  clipEpochStack_.clear();
  return true;
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
  clipEpoch_ = frame.savedClipEpoch;
  clipEpochStack_ = std::move(frame.savedClipEpochStack);
  patternFillPaint_ = std::move(frame.savedPatternFillPaint);
  patternStrokePaint_ = std::move(frame.savedPatternStrokePaint);
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

  const Path& pathGeometry = path.pathOrEmpty();

  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
  tiny_skia::Pixmap* fillPaintPixmap =
      !surfaceStack_.empty() && surfaceStack_.back().fillPaintPixmap.has_value()
          ? &*surfaceStack_.back().fillPaintPixmap
          : nullptr;
  tiny_skia::Pixmap* strokePaintPixmap =
      !surfaceStack_.empty() && surfaceStack_.back().strokePaintPixmap.has_value()
          ? &*surfaceStack_.back().strokePaintPixmap
          : nullptr;

  // The tiny-skia outline is resolved on demand, through the per-entity conversion cache: a
  // draw served from retained coverage needs no outline at all, and one that does need it takes
  // the cached conversion rather than converting again. Retention never converts on its own, so
  // the cache stays the only place a conversion happens and its per-frame count keeps meaning
  // what it says.
  tiny_skia::Path uncachedPath;
  const tiny_skia::Path* resolvedPath = nullptr;
  const auto tinyPath = [&]() -> const tiny_skia::Path& {
    if (resolvedPath == nullptr) {
      resolvedPath = &ResolveTinyPath(path, TinyPathCloseBehavior::Preserve, uncachedPath,
                                      cacheWiringCheckedRegistry_, frameCounters_);
    }
    return *resolvedPath;
  };

  // A draw that also paints a context-paint capture surface is two draws sharing one outline,
  // which one retained entry cannot describe, so it rasterizes.
  const RetainedTarget retained = (fillPaintPixmap == nullptr && strokePaintPixmap == nullptr)
                                      ? retainedTargetFor(path.sourceEntity)
                                      : RetainedTarget();

  const bool usedPatternFill = patternFillPaint_.has_value();
  std::optional<tiny_skia::Paint> fillPaint =
      paint_.drawFillComponent ? makeFillPaint(pathGeometry.bounds()) : std::nullopt;
  if (fillPaint) {
    auto pixmapView = currentPixmapView();

    RetainedSpanKey key;
    key.deviceFromLocal = deviceFromLocalTransform_;
    key.paint = *fillPaint;
    key.clipEpoch = clipEpoch_;
    key.surfaceSize = pixmapView.size();
    key.fillRule = path.fillRule;

    RetainedSpanSlot* slot =
        (retained && !paintBorrowsPattern(*fillPaint)) ? &retained.entry->fill : nullptr;
    drawRetainablePass(
        slot, key, pixmapView, mask, retainedSpanStats_,
        [&] {
          tiny_skia::Painter::fillPath(pixmapView, tinyPath(), *fillPaint,
                                       toTinyFillRule(path.fillRule),
                                       toTinyTransform(deviceFromLocalTransform_), mask);
        },
        [&](tiny_skia::SpanCapture& capture) {
          return capture.fillPath(pixmapView, tinyPath(), *fillPaint, toTinyFillRule(path.fillRule),
                                  toTinyTransform(deviceFromLocalTransform_), mask);
        });

    if (fillPaintPixmap != nullptr) {
      auto fillPaintView = fillPaintPixmap->mutableView();
      tiny_skia::Painter::fillPath(fillPaintView, tinyPath(), *fillPaint,
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
    const double actualLength = pathGeometry.pathLength();
    const double dashUnitsScale = actualLength / adjustedStroke.pathLength;
    for (double& dash : adjustedStroke.dashArray) {
      dash *= dashUnitsScale;
    }
    adjustedStroke.dashOffset *= dashUnitsScale;
  }

  const bool usedPatternStroke = patternStrokePaint_.has_value();
  std::optional<tiny_skia::Paint> strokePaint =
      paint_.drawStrokeComponent ? makeStrokePaint(pathGeometry.bounds(), adjustedStroke)
                                 : std::nullopt;
  if (strokePaint) {
    tiny_skia::Stroke tinyStroke;
    tinyStroke.width = NarrowToFloat(adjustedStroke.strokeWidth);
    tinyStroke.miterLimit = NarrowToFloat(adjustedStroke.miterLimit);
    tinyStroke.lineCap = toTinyLineCap(adjustedStroke.lineCap);
    tinyStroke.lineJoin = toTinyLineJoin(adjustedStroke.lineJoin);

    bool dashHasOnlyZeroLengthGaps = false;
    if (!adjustedStroke.dashArray.empty()) {
      const int repeats = (adjustedStroke.dashArray.size() & 1u) != 0u ? 2 : 1;
      std::vector<float> dashArray;
      dashArray.reserve(adjustedStroke.dashArray.size() * repeats);
      for (int i = 0; i < repeats; ++i) {
        for (double dash : adjustedStroke.dashArray) {
          dashArray.push_back(NarrowToFloat(dash));
        }
      }

      dashHasOnlyZeroLengthGaps = true;
      for (std::size_t i = 1; i < dashArray.size(); i += 2) {
        if (dashArray[i] != 0.0f) {
          dashHasOnlyZeroLengthGaps = false;
          break;
        }
      }

      tinyStroke.dash = tiny_skia::StrokeDash::create(std::move(dashArray),
                                                      NarrowToFloat(adjustedStroke.dashOffset));
    }

    // A zero-length gap still terminates one dash and starts the next. TinySkia otherwise joins
    // those ranges across ClosePath, filling the seam as if the stroke were solid. Replace only
    // the stroke's ClosePath commands with explicit closing lines so the dash stroker emits caps
    // at that boundary; fills and ordinary dashed/solid strokes keep their closed contours.
    const bool openDashSeam = tinyStroke.dash.has_value() && dashHasOnlyZeroLengthGaps;
    tiny_skia::Path uncachedDashSeamPath;
    const tiny_skia::Path* resolvedDashSeamPath = nullptr;
    const auto strokePath = [&]() -> const tiny_skia::Path& {
      if (!openDashSeam) {
        return tinyPath();
      }
      if (resolvedDashSeamPath == nullptr) {
        resolvedDashSeamPath =
            &ResolveTinyPath(path, TinyPathCloseBehavior::EndWithLine, uncachedDashSeamPath,
                             cacheWiringCheckedRegistry_, frameCounters_);
      }
      return *resolvedDashSeamPath;
    };

    auto pixmapView = currentPixmapView();

    RetainedSpanKey key;
    key.deviceFromLocal = deviceFromLocalTransform_;
    key.paint = *strokePaint;
    key.stroke = tinyStroke;
    key.clipEpoch = clipEpoch_;
    key.surfaceSize = pixmapView.size();
    key.openDashSeam = openDashSeam;

    RetainedSpanSlot* slot =
        (retained && !paintBorrowsPattern(*strokePaint)) ? &retained.entry->stroke : nullptr;
    drawRetainablePass(
        slot, key, pixmapView, mask, retainedSpanStats_,
        [&] {
          tiny_skia::Painter::strokePath(pixmapView, strokePath(), *strokePaint, tinyStroke,
                                         toTinyTransform(deviceFromLocalTransform_), mask);
        },
        [&](tiny_skia::SpanCapture& capture) {
          return capture.strokePath(pixmapView, strokePath(), *strokePaint, tinyStroke,
                                    toTinyTransform(deviceFromLocalTransform_), mask);
        });

    if (strokePaintPixmap != nullptr) {
      auto strokePaintView = strokePaintPixmap->mutableView();
      tiny_skia::Painter::strokePath(strokePaintView, strokePath(), *strokePaint, tinyStroke,
                                     toTinyTransform(deviceFromLocalTransform_), mask);
    }
    if (usedPatternStroke) {
      patternStrokePaint_.reset();
    }
  }

  if (retained) {
    // Charge the whole entry once both passes have written it, rather than each pass its own
    // slot: the two share the entry's own footprint, and the paints one pass keeps are only
    // meaningful next to the coverage the other kept.
    const std::size_t entryBytes = RetainedEntryBytes(*retained.entry);
    // Subtract before adding, and never below zero: the running total is maintained across
    // several paths and an unsigned wrap here would read as a document holding gigabytes.
    retained.state->liveBytes -= std::min(retained.state->liveBytes, retained.entry->chargedBytes);
    retained.state->liveBytes += entryBytes;
    retained.entry->chargedBytes = entryBytes;

    // Evicting removes entries, so nothing may touch `retained.entry` after this point.
    EvictRetainedSpansToBudget(*path.sourceEntity.registry(), frameToken_);
    retainedSpanStats_.liveBytes = retained.state->liveBytes;
    retainedSpanStats_.evictions = retained.state->evictions;
    retainedSpanStats_.documentDisabled = retained.state->disabled;
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
  std::optional<tiny_skia::Paint> fillPaint =
      paint_.drawFillComponent ? makeFillPaint(rect) : std::nullopt;
  if (fillPaint) {
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
  std::optional<tiny_skia::Paint> strokePaint =
      paint_.drawStrokeComponent ? makeStrokePaint(rect, stroke) : std::nullopt;
  if (strokePaint) {
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
  std::optional<tiny_skia::Paint> fillPaint =
      paint_.drawFillComponent ? makeFillPaint(bounds) : std::nullopt;
  if (fillPaint) {
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
  std::optional<tiny_skia::Paint> strokePaint =
      paint_.drawStrokeComponent ? makeStrokePaint(bounds, stroke) : std::nullopt;
  if (strokePaint) {
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

void RendererTinySkia::drawImagePixmap(const tiny_skia::PixmapView& source, int sourceWidth,
                                       int sourceHeight, const ImageParams& params) {
  const std::optional<tiny_skia::Rect> targetRect = toTinyRect(params.targetRect);
  if (!targetRect.has_value() || sourceWidth <= 0 || sourceHeight <= 0) {
    return;
  }

  const ImageRendering imageRendering = ResolveImageRendering(params);
  tiny_skia::PixmapView sampledSource = source;
  int sampledWidth = sourceWidth;
  int sampledHeight = sourceHeight;
  std::optional<tiny_skia::Pixmap> pixelatedIntermediate;
  if (imageRendering == ImageRendering::Pixelated) {
    const PixelatedSamplingPlan plan = MakePixelatedSamplingPlan(
        params.targetRect, sourceWidth, sourceHeight, deviceFromLocalTransform_);
    pixelatedIntermediate = CreatePixelatedIntermediate(source, plan);
    if (pixelatedIntermediate.has_value()) {
      sampledSource = pixelatedIntermediate->view();
      sampledWidth = static_cast<int>(plan.intermediateWidth);
      sampledHeight = static_cast<int>(plan.intermediateHeight);
    } else if (plan.needsIntermediate) {
      tiny_skia::Pixmap& destination = currentPixmap();
      tiny_skia::PixmapPaint paint =
          makePixmapPaint(destination, tiny_skia::FilterQuality::Nearest);
      paint.opacity = NarrowToFloat(params.opacity * paintOpacity_);
      paint.blendMode = tiny_skia::BlendMode::SourceOver;
      const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
      DrawProceduralPixelatedImage(source, sourceWidth, sourceHeight, plan.destFromSource,
                                   destination, paint, mask, verbose_);
      return;
    }
  }

  const Transform2d destFromSampledSource = MakeDestFromSourceTransform(
      params.targetRect, sampledWidth, sampledHeight, deviceFromLocalTransform_);

  tiny_skia::PixmapPaint paint =
      makePixmapPaint(currentPixmap(), FilterQualityForImageRendering(imageRendering));
  paint.opacity = NarrowToFloat(params.opacity * paintOpacity_);
  paint.blendMode = tiny_skia::BlendMode::SourceOver;

  const tiny_skia::Mask* mask = currentClipMask_.has_value() ? &*currentClipMask_ : nullptr;
  auto pixmapView = currentPixmapView();
  tiny_skia::Painter::drawPixmap(pixmapView, 0, 0, sampledSource, paint,
                                 toTinyTransform(destFromSampledSource), mask);
}

void RendererTinySkia::drawImage(const ImageResource& image, const ImageParams& params) {
  if (rejectedFilterDepth_ != 0) {
    return;
  }
  if (!HasExactRgbaPayload(image.data, image.width, image.height)) {
    return;
  }

  // `ImageResource` publishes straight alpha and tiny-skia samples premultiplied, so the
  // conversion is unavoidable, but it does not have to recur. The pixels belong to the element's
  // loaded image and change only when that does, so the premultiplied form is cached on the
  // source entity and borrowed through a view; the previous code premultiplied into a fresh
  // buffer and wrapped it in a throwaway `Pixmap` on every draw of every frame.
  const std::span<const std::uint8_t> premultiplied = ResolvePremultipliedImage(
      image, params.sourceEntity, pixelScratch_, cacheWiringCheckedRegistry_, frameCounters_);
  const std::optional<tiny_skia::PixmapView> sourceView =
      tiny_skia::PixmapView::fromBytes(premultiplied, static_cast<std::uint32_t>(image.width),
                                       static_cast<std::uint32_t>(image.height));
  if (!sourceView.has_value()) {
    return;
  }

  drawImagePixmap(*sourceView, image.width, image.height, params);
}

void RendererTinySkia::drawBitmap(const RendererBitmap& bitmap, const ImageParams& params) {
  if (rejectedFilterDepth_ != 0) {
    return;
  }
  if (bitmap.empty()) {
    return;
  }

  const std::optional<tiny_skia::Rect> targetRect = toTinyRect(params.targetRect);
  if (!targetRect.has_value()) {
    return;
  }

  const std::size_t tightRowBytes = static_cast<std::size_t>(bitmap.dimensions.x) * 4u;

  // Premultiplied tightly-packed payloads draw through a borrowed view: no
  // conversion, no copy. Padded rows must be packed because tiny-skia's view
  // indexes rows by width, not by caller-supplied stride. Unpremultiplied
  // payloads premultiply directly into the draw scratch while packing rows.
  // All cases beat the ImageResource contract, which costs an extra
  // full-buffer copy and, for premultiplied payloads, an extra unpremultiply.
  //
  // The two converting cases stage through `pixelScratch_` rather than a local
  // buffer, so a caller that composes the same layer every frame reuses one
  // allocation instead of acquiring and releasing a payload-sized buffer per
  // draw. The scratch is only ever read back through the view built from it
  // below, and no draw nests inside another, so one buffer serves all of them.
  std::optional<tiny_skia::PixmapView> sourceView;
  if (bitmap.alphaType == AlphaType::Premultiplied) {
    if (bitmap.rowBytes == tightRowBytes) {
      sourceView =
          tiny_skia::PixmapView::fromBytes(std::span<const std::uint8_t>(bitmap.pixels),
                                           static_cast<std::uint32_t>(bitmap.dimensions.x),
                                           static_cast<std::uint32_t>(bitmap.dimensions.y));
    } else {
      CopyTightRgbaRowsInto(bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
                            bitmap.rowBytes, pixelScratch_);
      sourceView =
          tiny_skia::PixmapView::fromBytes(std::span<const std::uint8_t>(pixelScratch_),
                                           static_cast<std::uint32_t>(bitmap.dimensions.x),
                                           static_cast<std::uint32_t>(bitmap.dimensions.y));
    }
  } else {
    PremultiplyRgbaRowsInto(bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
                            bitmap.rowBytes, pixelScratch_);
    sourceView = tiny_skia::PixmapView::fromBytes(std::span<const std::uint8_t>(pixelScratch_),
                                                  static_cast<std::uint32_t>(bitmap.dimensions.x),
                                                  static_cast<std::uint32_t>(bitmap.dimensions.y));
  }
  if (!sourceView.has_value()) {
    return;
  }

  drawImagePixmap(*sourceView, bitmap.dimensions.x, bitmap.dimensions.y, params);
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

  // Text bounding box for objectBoundingBox gradient/pattern mapping - the same
  // shared computation RendererGeode::drawText uses, so the two backends can't
  // drift on the bbox. Per the SVG spec it uses
  // em-box cells from font v-metrics (ascent above baseline, |descent| below),
  // not the raw font size.
  const Box2d textBounds = ComputeTextBounds(textEngine, runs, text.spans, params.viewBox,
                                             params.fontMetrics, fontSizePx);

  // Use makeFillPaint/makeStrokePaint to support gradients, patterns, and solid colors.
  // These read from paint_ (set by setPaint()) which the driver already populated.
  std::optional<tiny_skia::Paint> fillPaint = makeFillPaint(textBounds);
  const auto makeSolidPaint = [&](const css::Color& color, double opacityScale = 1.0) {
    tiny_skia::Paint paint = makeBasePaint(antialias_);

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
    PaintOrder spanPaintOrder;
    // Outline glyph paths for this run, collected so fill and stroke can be painted as
    // two whole-run passes in `paint-order` (matching resvg, which paints the text's
    // fill then the text's stroke as units rather than per-glyph).
    std::vector<tiny_skia::Path> runGlyphPaths;
    if (runIndex < text.spans.size()) {
      const auto& span = text.spans[runIndex];
      spanPaintOrder = span.paintOrder;
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
          paint.shader = std::move(*shader);
          spanFillPaint = paint;
        } else if (patternFillPaint_.has_value()) {
          tiny_skia::Paint paint = makeBasePaint(antialias_);
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
        // No explicit fill but has per-span opacity - re-apply with opacity.
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
            paint.shader = std::move(*shader);
            spanStrokePaint = paint;
          } else if (patternStrokePaint_.has_value()) {
            tiny_skia::Paint paint = makeBasePaint(antialias_);
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

      // Placed outline in document space (outline -> stretch -> translate ->
      // rotate). Shared with RendererGeode via PlacedTextGeometry so the two
      // backends can't drift on placement (0038). Empty for bitmap-only fonts
      // / outline-less glyphs, which fall through to the bitmap branch below
      // exactly as before.
      Path glyphPath;
      if (!isBitmapFont) {
        glyphPath = PlacedGlyphOutline(textEngine, run.font, glyph, scale);
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

          tiny_skia::PixmapPaint paint =
              makePixmapPaint(currentPixmap(), tiny_skia::FilterQuality::Bilinear);
          paint.opacity = NarrowToFloat(paintOpacity_);
          paint.blendMode = tiny_skia::BlendMode::SourceOver;

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

      // `glyphPath` is already placed in document space (translate/rotate baked
      // in by placedGlyphOutline); the renderer's current transform maps it to
      // device space below. Collected for the ordered fill/stroke passes after the loop.
      runGlyphPaths.push_back(toTinyPath(glyphPath));
    }

    // Honor `paint-order`: paint the whole run's fill and stroke in the resolved order
    // (markers do not apply to text glyphs, so they are skipped). The fill and stroke
    // are each painted across all glyphs as a unit so overlapping glyphs composite the
    // same way resvg renders them.
    auto pixmapView = currentPixmapView();
    const auto drawRunFill = [&]() {
      if (!spanFillPaint) return;
      for (const auto& tinyPath : runGlyphPaths) {
        tiny_skia::Painter::fillPath(pixmapView, tinyPath, *spanFillPaint,
                                     tiny_skia::FillRule::Winding,
                                     toTinyTransform(deviceFromLocalTransform_), mask);
      }
    };
    const auto drawRunStroke = [&]() {
      if (!spanStrokePaint) return;
      for (const auto& tinyPath : runGlyphPaths) {
        tiny_skia::Painter::strokePath(pixmapView, tinyPath, *spanStrokePaint, spanTinyStroke,
                                       toTinyTransform(deviceFromLocalTransform_), mask);
      }
    };

    bool fillDrawn = false;
    bool strokeDrawn = false;
    for (const PaintComponent component : spanPaintOrder.order) {
      if (component == PaintComponent::Fill && !fillDrawn) {
        drawRunFill();
        fillDrawn = true;
      } else if (component == PaintComponent::Stroke && !strokeDrawn) {
        drawRunStroke();
        strokeDrawn = true;
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

            drawDecoPath(toTinyPath(TransformPath(segPath, segTransform)));
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
  // `frame_` stores premultiplied RGBA8 (see the storage-model note on the
  // member declaration). This is the one place the frame leaves the renderer,
  // so it is the one place the premultiplied -> straight-alpha conversion runs.
  // The published contract stays straight alpha so every existing consumer
  // (compositor layer rasters, PNG export, texture uploads, the editor) keeps
  // reading the same semantics.
  snapshot.alphaType = AlphaType::Unpremultiplied;
  if (frame_.width() == 0 || frame_.height() == 0) {
    return snapshot;
  }

  // Copy straight into the snapshot's buffer. The caller owns the returned
  // pixels and `frame_` has to survive for the next frame, so one copy is
  // unavoidable; staging it through a temporary `Pixmap` (build a vector,
  // re-validate its length against a size we already own, then move the vector
  // back out) only adds moves and a redundant check around that same copy.
  const std::span<const std::uint8_t> framePixels = frame_.data();
  snapshot.pixels.assign(framePixels.begin(), framePixels.end());
  // Scalar pass, measured at roughly 2 ms for a 900x900 frame. It already
  // short-circuits alpha==255 and alpha==0 pixels, so a separate "is the frame
  // opaque?" pre-scan would only add a second full read without removing work;
  // the useful next step is a SIMD (4-8 pixels per iteration) implementation
  // inside `UnpremultiplyRgbaInPlace`, which would speed up every caller.
  UnpremultiplyRgbaInPlace(snapshot.pixels);
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
  auto renderer = std::make_unique<RendererTinySkia>(verbose_);
  renderer->filterExecutionBudget_ = filterExecutionBudget_;
  renderer->ownsFilterExecutionBudget_ = false;
  return renderer;
}

int RendererTinySkia::width() const {
  return static_cast<int>(frame_.width());
}

int RendererTinySkia::height() const {
  return static_cast<int>(frame_.height());
}

tiny_skia::Pixmap& RendererTinySkia::currentPixmap() {
  if (rejectedFilterDepth_ != 0) {
    return rejectedPixmap_;
  }
  return surfaceStack_.empty() ? frame_ : surfaceStack_.back().pixmap;
}

const tiny_skia::Pixmap& RendererTinySkia::currentPixmap() const {
  if (rejectedFilterDepth_ != 0) {
    return rejectedPixmap_;
  }
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

  const auto renderShapeMask = [&](const ClipPathShape& shape) -> std::optional<tiny_skia::Mask> {
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
        const ClipPathShape& shape = clip.clipPaths[static_cast<std::size_t>(index)];
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

  // `Pixmap::fromSize` hands back a zero-filled buffer, which is already the
  // transparent-black bit pattern this function promises. An explicit
  // `fill(transparent)` on top of that memsets every byte a second time, so it
  // is dropped: the result is byte-identical and each surface (frame buffer,
  // isolated layer, filter buffer plus its fill/stroke paint buffers, mask
  // capture and content, pattern tile) pays one full-buffer write instead of
  // two. Reusing a surface across frames is the one case that still needs an
  // explicit clear; see `beginFrame`.
  auto maybePixmap = tiny_skia::Pixmap::fromSize(static_cast<std::uint32_t>(width),
                                                 static_cast<std::uint32_t>(height));
  if (!maybePixmap.has_value()) {
    return tiny_skia::Pixmap();
  }

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

tiny_skia::PixmapPaint RendererTinySkia::makePixmapPaint(const tiny_skia::Pixmap& destination,
                                                         tiny_skia::FilterQuality quality) const {
  tiny_skia::PixmapPaint paint;
  paint.quality = quality;
  // Composites landing on `frame_` stay on the float raster pipeline. The
  // 8-bit compose path drifts a fully opaque source pixel to alpha 250 instead
  // of 255, which an intermediate surface absorbs at its next composite but
  // `frame_` cannot: it is what `takeSnapshot` hands to callers, so the drift
  // would surface as a nominally opaque region reporting partial coverage.
  paint.forceHqPipeline = &destination == &frame_;
  return paint;
}

void RendererTinySkia::compositePixmapInto(tiny_skia::Pixmap& destination,
                                           const tiny_skia::Pixmap& pixmap, double opacity,
                                           MixBlendMode blendMode) {
  if (opacity <= 0.0 || pixmap.width() == 0 || pixmap.height() == 0) {
    return;
  }

  tiny_skia::PixmapPaint paint = makePixmapPaint(destination, tiny_skia::FilterQuality::Nearest);
  paint.opacity = NarrowToFloat(opacity);
  paint.blendMode = toTinyBlendMode(blendMode);

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
