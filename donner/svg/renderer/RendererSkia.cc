#include "donner/svg/renderer/RendererSkia.h"

// Skia
#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPathMeasure.h"
#include "include/core/SkPicture.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkImageFilters.h"
#include "include/pathops/SkPathOps.h"

#ifdef DONNER_USE_CORETEXT
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(DONNER_USE_FREETYPE)
#include "include/ports/SkFontMgr_empty.h"
#elif defined(DONNER_USE_FREETYPE_WITH_FONTCONFIG)
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#else
#error \
    "Neither DONNER_USE_CORETEXT, DONNER_USE_FREETYPE, nor DONNER_USE_FREETYPE_WITH_FONTCONFIG is defined"
#endif
#ifdef DONNER_TEXT_SHAPING_ENABLED
#include "donner/svg/renderer/TextShaper.h"
#include "donner/svg/resources/FontManager.h"
#endif
// Donner
#include "donner/base/Length.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/renderer/FilterGraphExecutor.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "tiny_skia/Painter.h"

// Embedded resources
#include "embed_resources/PublicSansFont.h"

namespace donner::svg {

namespace {

const Boxd kUnitPathBounds(Vector2d::Zero(), Vector2d(1, 1));

SkPoint toSkia(Vector2d value) {
  return SkPoint::Make(NarrowToFloat(value.x), NarrowToFloat(value.y));
}

SkMatrix toSkiaMatrix(const Transformd& transform) {
  return SkMatrix::MakeAll(NarrowToFloat(transform.data[0]),  // scaleX
                           NarrowToFloat(transform.data[2]),  // skewX
                           NarrowToFloat(transform.data[4]),  // transX
                           NarrowToFloat(transform.data[1]),  // skewY
                           NarrowToFloat(transform.data[3]),  // scaleY
                           NarrowToFloat(transform.data[5]),  // transY
                           0, 0, 1);
}

SkRect toSkia(const Boxd& box) {
  return SkRect::MakeLTRB(
      static_cast<SkScalar>(box.topLeft.x), static_cast<SkScalar>(box.topLeft.y),
      static_cast<SkScalar>(box.bottomRight.x), static_cast<SkScalar>(box.bottomRight.y));
}

Transformd toDonnerTransform(const SkMatrix& matrix) {
  Transformd transform;
  transform.data[0] = matrix.getScaleX();
  transform.data[1] = matrix.getSkewY();
  transform.data[2] = matrix.getSkewX();
  transform.data[3] = matrix.getScaleY();
  transform.data[4] = matrix.getTranslateX();
  transform.data[5] = matrix.getTranslateY();
  return transform;
}

SkColor toSkia(const css::RGBA rgba) {
  return SkColorSetARGB(rgba.a, rgba.r, rgba.g, rgba.b);
}

SkPath toSkia(const PathSpline& spline);

SkTileMode toSkia(GradientSpreadMethod spreadMethod);

void applyClipToCanvas(SkCanvas* canvas, const ResolvedClip& clip) {
  if (clip.clipRect.has_value()) {
    canvas->clipRect(toSkia(*clip.clipRect));
  }

  if (clip.clipPaths.empty()) {
    return;
  }

  const SkMatrix skUnitsTransform = toSkiaMatrix(clip.clipPathUnitsTransform);

  SkPath fullPath;
  SmallVector<SkPath, 5> layeredPaths;
  int currentLayer = 0;

  for (const PathShape& shape : clip.clipPaths) {
    SkPath skPath = toSkia(shape.path);
    skPath.setFillType(shape.fillRule == FillRule::EvenOdd ? SkPathFillType::kEvenOdd
                                                           : SkPathFillType::kWinding);
    skPath.transform(toSkiaMatrix(shape.entityFromParent) * skUnitsTransform);

    if (shape.layer > currentLayer) {
      layeredPaths.push_back(skPath);
      currentLayer = shape.layer;
      continue;
    } else if (shape.layer < currentLayer) {
      assert(!layeredPaths.empty());
      SkPath layerPath = layeredPaths[layeredPaths.size() - 1];
      layeredPaths.pop_back();
      layerPath.transform(toSkiaMatrix(shape.entityFromParent) * skUnitsTransform);
      Op(layerPath, skPath, kIntersect_SkPathOp, &skPath);
      currentLayer = shape.layer;

      if (currentLayer != 0) {
        layeredPaths.push_back(skPath);
        continue;
      }
    }

    SkPath& targetPath = layeredPaths.empty() ? fullPath : layeredPaths[layeredPaths.size() - 1];
    Op(targetPath, skPath, kUnion_SkPathOp, &targetPath);
  }

  canvas->clipPath(fullPath, SkClipOp::kIntersect, true);
}

const components::filter_primitive::GaussianBlur* getSimpleNativeSkiaBlur(
    const components::FilterGraph& filterGraph, const std::optional<Boxd>& filterRegion,
    const Transformd& deviceFromFilter) {
  if (filterRegion.has_value()) {
    return nullptr;
  }

  if (filterGraph.nodes.size() != 1u) {
    return nullptr;
  }

  const components::FilterNode& node = filterGraph.nodes.front();
  if (node.inputs.size() != 1u ||
      !std::holds_alternative<components::FilterInput::Previous>(node.inputs.front().value) ||
      node.result.has_value() || node.x.has_value() || node.y.has_value() ||
      node.width.has_value() || node.height.has_value() ||
      node.colorInterpolationFilters.has_value()) {
    return nullptr;
  }

  const auto* blur = std::get_if<components::filter_primitive::GaussianBlur>(&node.primitive);
  if (blur == nullptr ||
      blur->edgeMode != components::filter_primitive::GaussianBlur::EdgeMode::None ||
      NearEquals(blur->stdDeviationX, blur->stdDeviationY, 1e-6)) {
    return nullptr;
  }

  const Vector2d xAxis = deviceFromFilter.transformVector(Vector2d(1.0, 0.0));
  const Vector2d yAxis = deviceFromFilter.transformVector(Vector2d(0.0, 1.0));
  const double xLength = xAxis.length();
  const double yLength = yAxis.length();
  const double dot = xAxis.x * yAxis.x + xAxis.y * yAxis.y;
  if (!NearEquals(xLength, yLength, 1e-6) || !NearZero(dot, 1e-6)) {
    return nullptr;
  }

  return blur;
}

bool isEligibleForTransformedBlurPath(const components::FilterGraph& filterGraph) {
  if (filterGraph.nodes.empty() ||
      filterGraph.primitiveUnits == PrimitiveUnits::ObjectBoundingBox) {
    return false;
  }

  bool hasBlur = false;
  for (const components::FilterNode& node : filterGraph.nodes) {
    const bool isBlur =
        std::holds_alternative<components::filter_primitive::GaussianBlur>(node.primitive) ||
        std::holds_alternative<components::filter_primitive::DropShadow>(node.primitive);
    const bool isOffset =
        std::holds_alternative<components::filter_primitive::Offset>(node.primitive);
    if (!isBlur && !isOffset) {
      return false;
    }
    hasBlur |= isBlur;

    if (node.result.has_value() || node.x.has_value() || node.y.has_value() ||
        node.width.has_value() || node.height.has_value()) {
      return false;
    }

    if (node.inputs.size() > 1u) {
      return false;
    }
    if (node.inputs.size() == 1u) {
      const auto& input = node.inputs.front();
      if (std::holds_alternative<components::FilterInput::Previous>(input.value)) {
        continue;
      }
      const auto* stdInput = std::get_if<components::FilterStandardInput>(&input.value);
      if (stdInput == nullptr || *stdInput != components::FilterStandardInput::SourceGraphic) {
        return false;
      }
    }
  }

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
        [&](const auto& primitive) {
          using T = std::decay_t<decltype(primitive)>;
          if constexpr (std::is_same_v<T, components::filter_primitive::GaussianBlur>) {
            anisotropic = !NearEquals(primitive.stdDeviationX, primitive.stdDeviationY, 1e-6);
          } else if constexpr (std::is_same_v<T, components::filter_primitive::DropShadow>) {
            anisotropic = !NearEquals(primitive.stdDeviationX, primitive.stdDeviationY, 1e-6);
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
  if (!NearZero(dot, 1e-6)) {
    return true;
  }

  const bool hasRotation =
      !NearZero(deviceFromFilter.data[1], 1e-6) || !NearZero(deviceFromFilter.data[2], 1e-6);
  return hasRotation && graphHasAnisotropicBlur(filterGraph);
}

double computeBlurPadding(const components::FilterGraph& filterGraph) {
  double maxSigma = 0.0;
  for (const auto& node : filterGraph.nodes) {
    std::visit(
        [&](const auto& primitive) {
          using T = std::decay_t<decltype(primitive)>;
          if constexpr (std::is_same_v<T, components::filter_primitive::GaussianBlur>) {
            maxSigma = std::max({maxSigma, primitive.stdDeviationX, primitive.stdDeviationY});
          } else if constexpr (std::is_same_v<T, components::filter_primitive::DropShadow>) {
            maxSigma = std::max({maxSigma, primitive.stdDeviationX, primitive.stdDeviationY});
          }
        },
        node.primitive);
  }
  return maxSigma * 3.0 + 1.0;
}

tiny_skia::Transform toTinyTransform(const Transformd& transform) {
  return tiny_skia::Transform::fromRow(
      NarrowToFloat(transform.data[0]), NarrowToFloat(transform.data[1]),
      NarrowToFloat(transform.data[2]), NarrowToFloat(transform.data[3]),
      NarrowToFloat(transform.data[4]), NarrowToFloat(transform.data[5]));
}

std::optional<tiny_skia::Pixmap> readSurfaceToPixmap(const sk_sp<SkSurface>& surface, int width,
                                                     int height) {
  const auto size = tiny_skia::IntSize::fromWH(static_cast<std::uint32_t>(width),
                                               static_cast<std::uint32_t>(height));
  if (!size.has_value()) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4u, 0);
  const SkImageInfo info =
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  if (!surface->readPixels(info, pixels.data(), static_cast<std::size_t>(width) * 4u, 0, 0)) {
    return std::nullopt;
  }

  return tiny_skia::Pixmap::fromVec(std::move(pixels), *size);
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

inline SkScalar resolveGradientCoord(Lengthd value, const Boxd& viewBox, bool numbersArePercent) {
  return NarrowToFloat(toPercent(value, numbersArePercent).toPixels(viewBox, FontMetrics()));
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

std::optional<SkPaint> instantiateGradientPaint(const components::PaintResolvedReference& ref,
                                                const Boxd& pathBounds, const Boxd& viewBox,
                                                const css::RGBA currentColor, float opacity,
                                                bool antialias) {
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

  // Use a generous tolerance for degenerate bounding box detection: cubic bezier computation
  // can produce floating-point artifacts (e.g. 1.4e-14 width for a perfectly vertical path).
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

  std::vector<SkScalar> positions;
  std::vector<SkColor> colors;
  positions.reserve(computedGradient->stops.size());
  colors.reserve(computedGradient->stops.size());
  for (const GradientStop& stop : computedGradient->stops) {
    positions.push_back(stop.offset);
    colors.push_back(toSkia(stop.color.resolve(currentColor, stop.opacity * opacity)));
  }

  if (positions.empty()) {
    return std::nullopt;
  }

  if (positions.size() == 1) {
    SkPaint paint;
    paint.setAntiAlias(antialias);
    paint.setColor(colors[0]);
    return paint;
  }

  const SkMatrix skGradientFromGradientUnits = toSkiaMatrix(gradientFromGradientUnits);

  SkPaint paint;
  paint.setAntiAlias(antialias);

  if (const auto* linear = handle.try_get<components::ComputedLinearGradientComponent>()) {
    const Vector2d start = resolveGradientCoords(linear->x1, linear->y1, bounds, numbersArePercent);
    const Vector2d end = resolveGradientCoords(linear->x2, linear->y2, bounds, numbersArePercent);

    const SkPoint points[] = {toSkia(start), toSkia(end)};
    paint.setShader(SkGradientShader::MakeLinear(
        points, colors.data(), positions.data(), static_cast<int>(positions.size()),
        toSkia(computedGradient->spreadMethod), 0, &skGradientFromGradientUnits));
    return paint;
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
      SkPaint solidPaint;
      solidPaint.setAntiAlias(antialias);
      solidPaint.setColor(colors.back());
      return solidPaint;
    }

    // SVG2: If the start (focal) circle fully overlaps the end circle, no gradient is drawn.
    // https://www.w3.org/TR/SVG2/pservers.html#RadialGradientNotes
    const double distanceBetweenCenters = (center - focalCenter).length();
    if (distanceBetweenCenters + radius <= focalRadius) {
      return std::nullopt;
    }

    const float skRadius = static_cast<float>(radius);
    if (NearZero(focalRadius) && focalCenter == center) {
      paint.setShader(SkGradientShader::MakeRadial(
          toSkia(center), skRadius, colors.data(), positions.data(),
          static_cast<int>(positions.size()), toSkia(computedGradient->spreadMethod), 0,
          &skGradientFromGradientUnits));
    } else {
      paint.setShader(SkGradientShader::MakeTwoPointConical(
          toSkia(focalCenter), static_cast<SkScalar>(focalRadius), toSkia(center), skRadius,
          colors.data(), positions.data(), static_cast<int>(positions.size()),
          toSkia(computedGradient->spreadMethod), 0, &skGradientFromGradientUnits));
    }
    return paint;
  }

  return std::nullopt;
}

SkPaint basePaint(bool antialias, double opacity) {
  SkPaint paint;
  paint.setAntiAlias(antialias);
  paint.setAlphaf(NarrowToFloat(opacity));
  return paint;
}

SkPaint::Cap toSkia(StrokeLinecap lineCap) {
  switch (lineCap) {
    case StrokeLinecap::Butt: return SkPaint::Cap::kButt_Cap;
    case StrokeLinecap::Round: return SkPaint::Cap::kRound_Cap;
    case StrokeLinecap::Square: return SkPaint::Cap::kSquare_Cap;
  }

  UTILS_UNREACHABLE();
}

SkPaint::Join toSkia(StrokeLinejoin lineJoin) {
  // TODO(jwmcglynn): Implement MiterClip and Arcs. For now, fallback to Miter, which is the default
  // linejoin, since the feature is not implemented.
  switch (lineJoin) {
    case StrokeLinejoin::Miter: return SkPaint::Join::kMiter_Join;
    case StrokeLinejoin::MiterClip: return SkPaint::Join::kMiter_Join;
    case StrokeLinejoin::Round: return SkPaint::Join::kRound_Join;
    case StrokeLinejoin::Bevel: return SkPaint::Join::kBevel_Join;
    case StrokeLinejoin::Arcs: return SkPaint::Join::kMiter_Join;
  }

  UTILS_UNREACHABLE();
}

SkPath toSkia(const PathSpline& spline) {
  SkPath path;

  const std::vector<Vector2d>& points = spline.points();
  for (const PathSpline::Command& command : spline.commands()) {
    switch (command.type) {
      case PathSpline::CommandType::MoveTo: {
        auto pt = points[command.pointIndex];
        path.moveTo(static_cast<SkScalar>(pt.x), static_cast<SkScalar>(pt.y));
        break;
      }
      case PathSpline::CommandType::CurveTo: {
        auto c0 = points[command.pointIndex];
        auto c1 = points[command.pointIndex + 1];
        auto end = points[command.pointIndex + 2];
        path.cubicTo(static_cast<SkScalar>(c0.x), static_cast<SkScalar>(c0.y),
                     static_cast<SkScalar>(c1.x), static_cast<SkScalar>(c1.y),
                     static_cast<SkScalar>(end.x), static_cast<SkScalar>(end.y));
        break;
      }
      case PathSpline::CommandType::LineTo: {
        auto pt = points[command.pointIndex];
        path.lineTo(static_cast<SkScalar>(pt.x), static_cast<SkScalar>(pt.y));
        break;
      }
      case PathSpline::CommandType::ClosePath: {
        path.close();
        break;
      }
    }
  }

  return path;
}

SkTileMode toSkia(GradientSpreadMethod spreadMethod) {
  switch (spreadMethod) {
    case GradientSpreadMethod::Pad: return SkTileMode::kClamp;
    case GradientSpreadMethod::Reflect: return SkTileMode::kMirror;
    case GradientSpreadMethod::Repeat: return SkTileMode::kRepeat;
  }

  UTILS_UNREACHABLE();
}

}  // namespace

RendererSkia::RendererSkia(bool verbose) : verbose_(verbose) {
#if defined(DONNER_USE_CORETEXT)
  fontMgr_ = SkFontMgr_New_CoreText();
#elif defined(DONNER_USE_FREETYPE_WITH_FONTCONFIG)
  fontMgr_ = SkFontMgr_New_FontConfig(SkFontScanner_FT::Make().release());
#elif defined(DONNER_USE_FREETYPE)
  fontMgr_ = SkFontMgr_New_Custom_Empty();
#endif
}

RendererSkia::~RendererSkia() {}

RendererSkia::RendererSkia(RendererSkia&&) noexcept = default;
RendererSkia& RendererSkia::operator=(RendererSkia&&) noexcept = default;

void RendererSkia::beginFrame(const RenderViewport& viewport) {
  viewport_ = viewport;
  const int pixelWidth = static_cast<int>(viewport.size.x * viewport.devicePixelRatio);
  const int pixelHeight = static_cast<int>(viewport.size.y * viewport.devicePixelRatio);

  bitmap_.allocPixels(
      SkImageInfo::MakeN32(pixelWidth, pixelHeight, SkAlphaType::kUnpremul_SkAlphaType));

  if (externalCanvas_ != nullptr) {
    currentCanvas_ = externalCanvas_;
  } else {
    bitmapCanvas_ = std::make_unique<SkCanvas>(bitmap_);
    currentCanvas_ = bitmapCanvas_.get();
  }

  transformDepth_ = 0;
  clipDepth_ = 0;
  activeClips_.clear();
  filterLayerStack_.clear();
}

void RendererSkia::endFrame() {
  for (; clipDepth_ > 0; --clipDepth_) {
    currentCanvas_->restore();
  }
  for (; transformDepth_ > 0; --transformDepth_) {
    currentCanvas_->restore();
  }

  currentCanvas_ = nullptr;
  externalCanvas_ = nullptr;
  bitmapCanvas_.reset();
  activeClips_.clear();
  filterLayerStack_.clear();
}

void RendererSkia::setTransform(const Transformd& transform) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  if (!patternStack_.empty() && patternStack_.back().surface != nullptr &&
      currentCanvas_ == patternStack_.back().surface->getCanvas()) {
    const Transformd& rasterFromTile = patternStack_.back().patternRasterFromTile;
    currentCanvas_->setMatrix(toSkiaMatrix(
        scaleTransformOutput(transform, Vector2d(rasterFromTile.data[0], rasterFromTile.data[3]))));
  } else {
    currentCanvas_->setMatrix(toSkiaMatrix(transform));
  }
}

void RendererSkia::pushTransform(const Transformd& transform) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  currentCanvas_->save();
  currentCanvas_->concat(toSkiaMatrix(transform));
  ++transformDepth_;
}

void RendererSkia::popTransform() {
  if (currentCanvas_ == nullptr || transformDepth_ <= 0) {
    return;
  }

  currentCanvas_->restore();
  --transformDepth_;
}

void RendererSkia::pushClip(const ResolvedClip& clip) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  const SkMatrix clipMatrix = currentCanvas_->getTotalMatrix();
  currentCanvas_->save();
  applyClipToCanvas(currentCanvas_, clip);
  ++clipDepth_;
  activeClips_.push_back(ClipStackEntry{clip, clipMatrix});
}

void RendererSkia::popClip() {
  if (currentCanvas_ == nullptr || clipDepth_ <= 0) {
    return;
  }

  currentCanvas_->restore();
  --clipDepth_;
  if (!activeClips_.empty()) {
    activeClips_.pop_back();
  }
}

std::optional<SkPaint> RendererSkia::makeFillPaint(const Boxd& bounds) {
  if (std::holds_alternative<PaintServer::None>(paint_.fill)) {
    return std::nullopt;
  }

  // Use pre-recorded pattern tile if available.
  if (patternFillPaint_.has_value()) {
    SkPaint paint = std::move(*patternFillPaint_);
    patternFillPaint_.reset();
    paint.setStyle(SkPaint::Style::kFill_Style);
    return paint;
  }

  const css::RGBA currentColor = paint_.currentColor.rgba();
  const float fillOpacity = NarrowToFloat(paint_.fillOpacity);

  if (const auto* solid = std::get_if<PaintServer::Solid>(&paint_.fill)) {
    SkPaint paint;
    paint.setAntiAlias(antialias_);
    paint.setStyle(SkPaint::Style::kFill_Style);
    paint.setColor(toSkia(solid->color.resolve(currentColor, fillOpacity)));
    return paint;
  }

  if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint_.fill)) {
    if (std::optional<SkPaint> gradient = instantiateGradientPaint(
            *ref, bounds, paint_.viewBox, currentColor, fillOpacity, antialias_)) {
      gradient->setStyle(SkPaint::Style::kFill_Style);
      return gradient;
    }

    if (ref->fallback) {
      SkPaint paint;
      paint.setAntiAlias(antialias_);
      paint.setStyle(SkPaint::Style::kFill_Style);
      paint.setColor(toSkia(ref->fallback->resolve(currentColor, fillOpacity)));
      return paint;
    }
  }

  return std::nullopt;
}

std::optional<SkPaint> RendererSkia::makeStrokePaint(const Boxd& bounds,
                                                     const StrokeParams& stroke) {
  if (std::holds_alternative<PaintServer::None>(paint_.stroke) || stroke.strokeWidth <= 0.0) {
    return std::nullopt;
  }

  auto configureStroke = [&](SkPaint& paint) {
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(static_cast<SkScalar>(stroke.strokeWidth));
    paint.setStrokeCap(toSkia(stroke.lineCap));
    paint.setStrokeJoin(toSkia(stroke.lineJoin));
    paint.setStrokeMiter(static_cast<SkScalar>(stroke.miterLimit));

    if (!stroke.dashArray.empty()) {
      // Skia requires an even number of dash lengths; repeat odd-length arrays.
      const int numRepeats = (stroke.dashArray.size() & 1) ? 2 : 1;
      std::vector<SkScalar> dashes;
      dashes.reserve(stroke.dashArray.size() * numRepeats);
      for (int i = 0; i < numRepeats; ++i) {
        for (double dash : stroke.dashArray) {
          dashes.push_back(static_cast<SkScalar>(dash));
        }
      }

      paint.setPathEffect(SkDashPathEffect::Make(dashes.data(), static_cast<int>(dashes.size()),
                                                 static_cast<SkScalar>(stroke.dashOffset)));
    }
  };

  // Use pre-recorded pattern tile if available.
  if (patternStrokePaint_.has_value()) {
    SkPaint paint = std::move(*patternStrokePaint_);
    patternStrokePaint_.reset();
    configureStroke(paint);
    return paint;
  }

  const css::RGBA currentColor = paint_.currentColor.rgba();
  const float strokeOpacity = NarrowToFloat(paint_.strokeOpacity);

  if (const auto* solid = std::get_if<PaintServer::Solid>(&paint_.stroke)) {
    SkPaint paint;
    paint.setAntiAlias(antialias_);
    configureStroke(paint);
    paint.setColor(toSkia(solid->color.resolve(currentColor, strokeOpacity)));
    return paint;
  }

  if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint_.stroke)) {
    if (std::optional<SkPaint> gradient = instantiateGradientPaint(
            *ref, bounds, paint_.viewBox, currentColor, strokeOpacity, antialias_)) {
      configureStroke(*gradient);
      return gradient;
    }

    if (ref->fallback) {
      SkPaint paint;
      paint.setAntiAlias(antialias_);
      configureStroke(paint);
      paint.setColor(toSkia(ref->fallback->resolve(currentColor, strokeOpacity)));
      return paint;
    }
  }

  return std::nullopt;
}

RendererSkia::FilterLayerState* RendererSkia::currentFilterLayerState() {
  if (filterLayerStack_.empty()) {
    return nullptr;
  }

  FilterLayerState& state = filterLayerStack_.back();
  return state.usesNativeSkiaFilter ? nullptr : &state;
}

void RendererSkia::drawOnFilterInputSurface(const sk_sp<SkSurface>& surface,
                                            const std::function<void(SkCanvas*)>& drawFn) {
  if (surface == nullptr || currentCanvas_ == nullptr) {
    return;
  }

  SkCanvas* canvas = surface->getCanvas();
  canvas->save();
  for (const ClipStackEntry& entry : activeClips_) {
    canvas->save();
    canvas->setMatrix(entry.matrix);
    applyClipToCanvas(canvas, entry.clip);
  }

  canvas->setMatrix(currentCanvas_->getTotalMatrix());
  drawFn(canvas);

  for (std::size_t i = 0; i < activeClips_.size(); ++i) {
    canvas->restore();
  }
  canvas->restore();
}

void RendererSkia::pushFilterLayer(const components::FilterGraph& filterGraph,
                                   const std::optional<Boxd>& filterRegion) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  const Transformd deviceFromFilter = toDonnerTransform(currentCanvas_->getTotalMatrix());
  if (!shouldUseTransformedBlurPath(filterGraph, deviceFromFilter)) {
    if (const auto* blur = getSimpleNativeSkiaBlur(filterGraph, filterRegion, deviceFromFilter)) {
      SkPaint filterPaint;
      filterPaint.setAntiAlias(antialias_);
      sk_sp<SkImageFilter> imageFilter;
      if (filterRegion.has_value()) {
        const SkRect cropRect = currentCanvas_->getTotalMatrix().mapRect(toSkia(*filterRegion));
        imageFilter = SkImageFilters::Blur(static_cast<SkScalar>(blur->stdDeviationX),
                                           static_cast<SkScalar>(blur->stdDeviationY), nullptr,
                                           SkImageFilters::CropRect(cropRect));
      } else {
        imageFilter = SkImageFilters::Blur(static_cast<SkScalar>(blur->stdDeviationX),
                                           static_cast<SkScalar>(blur->stdDeviationY), nullptr);
      }
      filterPaint.setImageFilter(std::move(imageFilter));
      currentCanvas_->saveLayer(nullptr, &filterPaint);

      FilterLayerState state;
      state.usesNativeSkiaFilter = true;
      filterLayerStack_.push_back(std::move(state));
      return;
    }
  }

  const int width = static_cast<int>(viewport_.size.x * viewport_.devicePixelRatio);
  const int height = static_cast<int>(viewport_.size.y * viewport_.devicePixelRatio);
  const SkImageInfo surfaceInfo =
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(surfaceInfo);
  if (surface == nullptr) {
    return;
  }

  auto createFilterInputSurface = [&](bool needed) -> sk_sp<SkSurface> {
    if (!needed) {
      return nullptr;
    }

    sk_sp<SkSurface> inputSurface = SkSurfaces::Raster(surfaceInfo);
    if (inputSurface != nullptr) {
      inputSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
    }
    return inputSurface;
  };

  SkCanvas* filterCanvas = surface->getCanvas();
  filterCanvas->clear(SK_ColorTRANSPARENT);
  for (const ClipStackEntry& entry : activeClips_) {
    filterCanvas->save();
    filterCanvas->setMatrix(entry.matrix);
    applyClipToCanvas(filterCanvas, entry.clip);
  }
  filterCanvas->setMatrix(currentCanvas_->getTotalMatrix());

  FilterLayerState state;
  state.surface = std::move(surface);
  state.fillPaintSurface = createFilterInputSurface(
      graphUsesStandardInput(filterGraph, components::FilterStandardInput::FillPaint));
  state.strokePaintSurface = createFilterInputSurface(
      graphUsesStandardInput(filterGraph, components::FilterStandardInput::StrokePaint));
  state.parentCanvas = currentCanvas_;
  state.filterGraph = filterGraph;
  state.filterRegion = filterRegion;
  state.deviceFromFilter = deviceFromFilter;
  currentCanvas_ = filterCanvas;
  filterLayerStack_.push_back(std::move(state));
}

void RendererSkia::popFilterLayer() {
  if (currentCanvas_ == nullptr || filterLayerStack_.empty()) {
    return;
  }

  FilterLayerState state = std::move(filterLayerStack_.back());
  filterLayerStack_.pop_back();

  if (state.usesNativeSkiaFilter) {
    currentCanvas_->restore();
    return;
  }

  const int width = static_cast<int>(viewport_.size.x * viewport_.devicePixelRatio);
  const int height = static_cast<int>(viewport_.size.y * viewport_.devicePixelRatio);
  const auto size = tiny_skia::IntSize::fromWH(static_cast<std::uint32_t>(width),
                                               static_cast<std::uint32_t>(height));
  if (!size.has_value()) {
    currentCanvas_ = state.parentCanvas;
    return;
  }

  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4u, 0);
  const SkImageInfo info =
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  if (!state.surface->readPixels(info, pixels.data(), static_cast<std::size_t>(width) * 4u, 0, 0)) {
    currentCanvas_ = state.parentCanvas;
    return;
  }

  auto maybePixmap = tiny_skia::Pixmap::fromVec(std::move(pixels), *size);
  if (!maybePixmap.has_value()) {
    currentCanvas_ = state.parentCanvas;
    return;
  }

  std::optional<tiny_skia::Pixmap> fillPaintPixmap;
  if (state.fillPaintSurface != nullptr) {
    fillPaintPixmap = readSurfaceToPixmap(state.fillPaintSurface, width, height);
  }

  std::optional<tiny_skia::Pixmap> strokePaintPixmap;
  if (state.strokePaintSurface != nullptr) {
    strokePaintPixmap = readSurfaceToPixmap(state.strokePaintSurface, width, height);
  }

  const bool useTransformedBlurPath =
      state.filterRegion.has_value() && state.filterRegion->width() > 0 &&
      state.filterRegion->height() > 0 && !NearZero(state.deviceFromFilter.determinant()) &&
      isEligibleForTransformedBlurPath(state.filterGraph) &&
      shouldUseTransformedBlurPath(state.filterGraph, state.deviceFromFilter);

  if (useTransformedBlurPath) {
    const Boxd& filterRegion = *state.filterRegion;
    const double scaleX =
        std::max(1.0, state.deviceFromFilter.transformVector(Vector2d(1.0, 0.0)).length());
    const double scaleY =
        std::max(1.0, state.deviceFromFilter.transformVector(Vector2d(0.0, 1.0)).length());
    const double blurPadding = computeBlurPadding(state.filterGraph);
    const Boxd paddedRegion(filterRegion.topLeft - Vector2d(blurPadding, blurPadding),
                            filterRegion.bottomRight + Vector2d(blurPadding, blurPadding));

    const int localWidth = std::max(1, static_cast<int>(std::ceil(paddedRegion.width() * scaleX)));
    const int localHeight =
        std::max(1, static_cast<int>(std::ceil(paddedRegion.height() * scaleY)));

    auto maybeLocalPixmap = tiny_skia::Pixmap::fromSize(static_cast<std::uint32_t>(localWidth),
                                                        static_cast<std::uint32_t>(localHeight));
    if (maybeLocalPixmap.has_value()) {
      tiny_skia::Pixmap localPixmap = std::move(*maybeLocalPixmap);
      localPixmap.fill(tiny_skia::Color::transparent);

      const Transformd filterFromDevice = state.deviceFromFilter.inverse();
      const Transformd deviceToLocal =
          Transformd::Scale(scaleX, scaleY) *
          Transformd::Translate(-paddedRegion.topLeft.x, -paddedRegion.topLeft.y) *
          filterFromDevice;

      tiny_skia::PixmapPaint resamplePaint;
      resamplePaint.opacity = 1.0f;
      resamplePaint.blendMode = tiny_skia::BlendMode::Source;
      resamplePaint.quality = tiny_skia::FilterQuality::Bilinear;
      auto localView = localPixmap.mutableView();
      tiny_skia::Painter::drawPixmap(localView, 0, 0, maybePixmap->view(), resamplePaint,
                                     toTinyTransform(deviceToLocal));

      const Transformd localDeviceFromFilter = Transformd::Scale(scaleX, scaleY);
      const Boxd localFilterRegion(
          Vector2d(blurPadding, blurPadding),
          Vector2d(blurPadding + filterRegion.width(), blurPadding + filterRegion.height()));
      ApplyFilterGraphToPixmap(localPixmap, state.filterGraph, localDeviceFromFilter,
                               localFilterRegion, false);
      ClipFilterOutputToRegion(localPixmap, localFilterRegion, localDeviceFromFilter);

      const Transformd deviceFromLocal =
          state.deviceFromFilter *
          Transformd::Translate(paddedRegion.topLeft.x, paddedRegion.topLeft.y) *
          Transformd::Scale(1.0 / scaleX, 1.0 / scaleY);

      const std::vector<std::uint8_t> localPixels = localPixmap.release();
      const SkImageInfo localInfo =
          SkImageInfo::Make(localWidth, localHeight, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
      const SkPixmap localSkPixmap(localInfo, localPixels.data(),
                                   static_cast<std::size_t>(localWidth) * 4u);
      sk_sp<SkImage> localImage = SkImages::RasterFromPixmapCopy(localSkPixmap);

      currentCanvas_ = state.parentCanvas;
      if (localImage != nullptr) {
        currentCanvas_->save();
        currentCanvas_->resetMatrix();
        currentCanvas_->concat(toSkiaMatrix(deviceFromLocal));
        currentCanvas_->drawImage(localImage, 0.0f, 0.0f, SkSamplingOptions(SkFilterMode::kLinear),
                                  nullptr);
        currentCanvas_->restore();
        return;
      }
    }
  }

  ApplyFilterGraphToPixmap(*maybePixmap, state.filterGraph, state.deviceFromFilter,
                           state.filterRegion, true,
                           fillPaintPixmap.has_value() ? &*fillPaintPixmap : nullptr,
                           strokePaintPixmap.has_value() ? &*strokePaintPixmap : nullptr);
  ClipFilterOutputToRegion(*maybePixmap, state.filterRegion, state.deviceFromFilter);

  const std::vector<std::uint8_t> filteredPixels = maybePixmap->release();
  const SkPixmap pixmap(info, filteredPixels.data(), static_cast<std::size_t>(width) * 4u);
  sk_sp<SkImage> filteredImage = SkImages::RasterFromPixmapCopy(pixmap);

  currentCanvas_ = state.parentCanvas;
  if (filteredImage == nullptr) {
    return;
  }

  currentCanvas_->save();
  currentCanvas_->resetMatrix();
  currentCanvas_->drawImage(filteredImage, 0.0f, 0.0f);
  currentCanvas_->restore();
}

void RendererSkia::pushIsolatedLayer(double opacity) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  SkPaint layerPaint;
  layerPaint.setAlphaf(NarrowToFloat(opacity));
  currentCanvas_->saveLayer(nullptr, &layerPaint);
}

void RendererSkia::popIsolatedLayer() {
  if (currentCanvas_ == nullptr) {
    return;
  }

  currentCanvas_->restore();
}

void RendererSkia::pushMask(const std::optional<Boxd>& maskBounds) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  const int width = static_cast<int>(viewport_.size.x * viewport_.devicePixelRatio);
  const int height = static_cast<int>(viewport_.size.y * viewport_.devicePixelRatio);
  sk_sp<SkSurface> maskSurface = SkSurfaces::Raster(
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
  if (maskSurface == nullptr) {
    return;
  }

  SkCanvas* maskCanvas = maskSurface->getCanvas();
  maskCanvas->clear(SK_ColorTRANSPARENT);
  for (const ClipStackEntry& entry : activeClips_) {
    maskCanvas->save();
    maskCanvas->setMatrix(entry.matrix);
    applyClipToCanvas(maskCanvas, entry.clip);
  }

  maskCanvas->setMatrix(currentCanvas_->getTotalMatrix());
  if (maskBounds.has_value()) {
    maskCanvas->clipRect(toSkia(*maskBounds), SkClipOp::kIntersect, true);
  }

  MaskLayerState state;
  state.maskSurface = std::move(maskSurface);
  state.parentCanvas = currentCanvas_;
  currentCanvas_ = maskCanvas;
  maskLayerStack_.push_back(std::move(state));
}

void RendererSkia::transitionMaskToContent() {
  if (currentCanvas_ == nullptr || maskLayerStack_.empty()) {
    return;
  }

  MaskLayerState& state = maskLayerStack_.back();
  const int width = static_cast<int>(viewport_.size.x * viewport_.devicePixelRatio);
  const int height = static_cast<int>(viewport_.size.y * viewport_.devicePixelRatio);
  std::optional<tiny_skia::Pixmap> maskPixmap =
      readSurfaceToPixmap(state.maskSurface, width, height);
  if (maskPixmap.has_value()) {
    state.maskAlpha =
        tiny_skia::Mask::fromPixmap(maskPixmap->view(), tiny_skia::MaskType::Luminance);
  }

  state.contentSurface = SkSurfaces::Raster(
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
  if (state.contentSurface == nullptr) {
    currentCanvas_ = state.parentCanvas;
    return;
  }

  SkCanvas* contentCanvas = state.contentSurface->getCanvas();
  contentCanvas->clear(SK_ColorTRANSPARENT);
  for (const ClipStackEntry& entry : activeClips_) {
    contentCanvas->save();
    contentCanvas->setMatrix(entry.matrix);
    applyClipToCanvas(contentCanvas, entry.clip);
  }

  contentCanvas->setMatrix(state.parentCanvas->getTotalMatrix());
  currentCanvas_ = contentCanvas;
}

void RendererSkia::popMask() {
  if (currentCanvas_ == nullptr || maskLayerStack_.empty()) {
    return;
  }

  const int width = static_cast<int>(viewport_.size.x * viewport_.devicePixelRatio);
  const int height = static_cast<int>(viewport_.size.y * viewport_.devicePixelRatio);

  MaskLayerState state = std::move(maskLayerStack_.back());
  maskLayerStack_.pop_back();

  currentCanvas_ = state.parentCanvas;
  if (state.contentSurface == nullptr) {
    return;
  }

  std::optional<tiny_skia::Pixmap> contentPixmap =
      readSurfaceToPixmap(state.contentSurface, width, height);
  if (!contentPixmap.has_value()) {
    return;
  }

  if (state.maskAlpha.has_value()) {
    auto contentView = contentPixmap->mutableView();
    tiny_skia::Painter::applyMask(contentView, *state.maskAlpha);
  }

  const SkImageInfo info =
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  const std::vector<std::uint8_t> maskedPixels = contentPixmap->release();
  const SkPixmap pixmap(info, maskedPixels.data(), static_cast<std::size_t>(width) * 4u);
  sk_sp<SkImage> maskedImage = SkImages::RasterFromPixmapCopy(pixmap);
  if (maskedImage == nullptr) {
    return;
  }

  currentCanvas_->save();
  currentCanvas_->resetMatrix();
  currentCanvas_->drawImage(maskedImage, 0.0f, 0.0f);
  currentCanvas_->restore();
}

void RendererSkia::beginPatternTile(const Boxd& tileRect, const Transformd& targetFromPattern) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  const Transformd deviceFromTarget = toDonnerTransform(currentCanvas_->getTotalMatrix());
  const Transformd deviceFromPattern = deviceFromTarget * targetFromPattern;
  const Vector2d requestedRasterScale = patternRasterScaleForTransform(deviceFromPattern);
  const int pixelWidth =
      std::max(1, static_cast<int>(std::ceil(tileRect.width() * requestedRasterScale.x)));
  const int pixelHeight =
      std::max(1, static_cast<int>(std::ceil(tileRect.height() * requestedRasterScale.y)));
  const Vector2d rasterScale =
      effectivePatternRasterScale(tileRect, pixelWidth, pixelHeight, requestedRasterScale);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(
      SkImageInfo::Make(pixelWidth, pixelHeight, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
  if (surface == nullptr) {
    return;
  }

  PatternState state;
  state.surface = std::move(surface);
  state.targetFromPattern = targetFromPattern;
  state.patternRasterFromTile = Transformd::Scale(rasterScale);
  state.targetFromPattern.data[4] *= rasterScale.x;
  state.targetFromPattern.data[5] *= rasterScale.y;
  state.targetFromPattern =
      state.targetFromPattern * Transformd::Scale(1.0 / rasterScale.x, 1.0 / rasterScale.y);
  state.savedCanvas = currentCanvas_;
  currentCanvas_ = state.surface->getCanvas();
  currentCanvas_->clear(SK_ColorTRANSPARENT);
  patternStack_.push_back(std::move(state));
}

void RendererSkia::endPatternTile(bool forStroke) {
  if (patternStack_.empty()) {
    return;
  }

  PatternState state = std::move(patternStack_.back());
  patternStack_.pop_back();

  currentCanvas_ = state.savedCanvas;

  const SkMatrix skTargetFromPattern = toSkiaMatrix(state.targetFromPattern);
  if (state.surface == nullptr) {
    return;
  }

  sk_sp<SkImage> image = state.surface->makeImageSnapshot();
  if (image == nullptr) {
    return;
  }

  SkPaint patternPaint;
  patternPaint.setAntiAlias(antialias_);
  patternPaint.setShader(image->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat,
                                           SkSamplingOptions(SkFilterMode::kLinear),
                                           &skTargetFromPattern));

  if (forStroke) {
    patternStrokePaint_ = std::move(patternPaint);
  } else {
    patternFillPaint_ = std::move(patternPaint);
  }
}

void RendererSkia::setPaint(const PaintParams& paint) {
  paint_ = paint;
  paintOpacity_ = paint.opacity;
}

void RendererSkia::drawPath(const PathShape& path, const StrokeParams& stroke) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  if (verbose_) {
    const bool isSolid = std::holds_alternative<PaintServer::Solid>(paint_.fill);
    const SkMatrix m = currentCanvas_->getTotalMatrix();
    std::cout << "[Skia::drawPath] saveCount=" << currentCanvas_->getSaveCount() << " matrix=["
              << m.getScaleX() << "," << m.getScaleY() << "," << m.getTranslateX() << ","
              << m.getTranslateY() << "]"
              << " bounds=" << path.path.bounds() << " fillOpacity=" << paint_.fillOpacity
              << " fillIsSolid=" << isSolid << " isRef="
              << std::holds_alternative<components::PaintResolvedReference>(paint_.fill)
              << " isNone=" << std::holds_alternative<PaintServer::None>(paint_.fill);
    if (isSolid) {
      const auto& solid = std::get<PaintServer::Solid>(paint_.fill);
      std::cout << " color=" << solid.color;
    }
    std::cout << "\n";
  }

  SkPath skPath = toSkia(path.path);
  if (path.fillRule == FillRule::EvenOdd) {
    skPath.setFillType(SkPathFillType::kEvenOdd);
  }

  FilterLayerState* filterLayerState = currentFilterLayerState();
  if (std::optional<SkPaint> fillPaint = makeFillPaint(path.path.bounds())) {
    currentCanvas_->drawPath(skPath, *fillPaint);
    if (filterLayerState != nullptr && filterLayerState->fillPaintSurface != nullptr) {
      drawOnFilterInputSurface(filterLayerState->fillPaintSurface,
                               [&](SkCanvas* canvas) { canvas->drawPath(skPath, *fillPaint); });
    }
  }

  // Apply pathLength scaling to dash arrays if needed.
  StrokeParams adjustedStroke = stroke;
  if (!adjustedStroke.dashArray.empty() && adjustedStroke.pathLength > 0.0 &&
      !NearZero(adjustedStroke.pathLength)) {
    const double skiaLength = SkPathMeasure(skPath, false).getLength();
    const double dashUnitsScale = skiaLength / adjustedStroke.pathLength;
    for (double& dash : adjustedStroke.dashArray) {
      dash *= dashUnitsScale;
    }
    adjustedStroke.dashOffset *= dashUnitsScale;
  }

  if (std::optional<SkPaint> strokePaint = makeStrokePaint(path.path.bounds(), adjustedStroke)) {
    currentCanvas_->drawPath(skPath, *strokePaint);
    if (filterLayerState != nullptr && filterLayerState->strokePaintSurface != nullptr) {
      drawOnFilterInputSurface(filterLayerState->strokePaintSurface,
                               [&](SkCanvas* canvas) { canvas->drawPath(skPath, *strokePaint); });
    }
  }
}

void RendererSkia::drawRect(const Boxd& rect, const StrokeParams& stroke) {
  const SkRect skRect = toSkia(rect);
  FilterLayerState* filterLayerState = currentFilterLayerState();
  if (std::optional<SkPaint> fillPaint = makeFillPaint(rect)) {
    currentCanvas_->drawRect(skRect, *fillPaint);
    if (filterLayerState != nullptr && filterLayerState->fillPaintSurface != nullptr) {
      drawOnFilterInputSurface(filterLayerState->fillPaintSurface,
                               [&](SkCanvas* canvas) { canvas->drawRect(skRect, *fillPaint); });
    }
  }

  if (std::optional<SkPaint> strokePaint = makeStrokePaint(rect, stroke)) {
    currentCanvas_->drawRect(skRect, *strokePaint);
    if (filterLayerState != nullptr && filterLayerState->strokePaintSurface != nullptr) {
      drawOnFilterInputSurface(filterLayerState->strokePaintSurface,
                               [&](SkCanvas* canvas) { canvas->drawRect(skRect, *strokePaint); });
    }
  }
}

void RendererSkia::drawEllipse(const Boxd& bounds, const StrokeParams& stroke) {
  SkPath ellipse;
  ellipse.addOval(toSkia(bounds));
  FilterLayerState* filterLayerState = currentFilterLayerState();
  if (std::optional<SkPaint> fillPaint = makeFillPaint(bounds)) {
    currentCanvas_->drawPath(ellipse, *fillPaint);
    if (filterLayerState != nullptr && filterLayerState->fillPaintSurface != nullptr) {
      drawOnFilterInputSurface(filterLayerState->fillPaintSurface,
                               [&](SkCanvas* canvas) { canvas->drawPath(ellipse, *fillPaint); });
    }
  }

  if (std::optional<SkPaint> strokePaint = makeStrokePaint(bounds, stroke)) {
    currentCanvas_->drawPath(ellipse, *strokePaint);
    if (filterLayerState != nullptr && filterLayerState->strokePaintSurface != nullptr) {
      drawOnFilterInputSurface(filterLayerState->strokePaintSurface,
                               [&](SkCanvas* canvas) { canvas->drawPath(ellipse, *strokePaint); });
    }
  }
}

void RendererSkia::drawImage(const ImageResource& image, const ImageParams& params) {
  if (currentCanvas_ == nullptr || image.data.empty()) {
    return;
  }

  SkImageInfo info =
      SkImageInfo::Make(image.width, image.height, SkColorType::kRGBA_8888_SkColorType,
                        SkAlphaType::kPremul_SkAlphaType);
  const SkPixmap pixmap(info, image.data.data(), static_cast<size_t>(image.width * 4));
  sk_sp<SkImage> skImage = SkImages::RasterFromPixmapCopy(pixmap);
  if (skImage == nullptr) {
    return;
  }

  SkPaint paint = basePaint(antialias_, params.opacity * paintOpacity_);
  const SkSamplingOptions sampling(params.imageRenderingPixelated ? SkFilterMode::kNearest
                                                                  : SkFilterMode::kLinear);

  currentCanvas_->drawImageRect(skImage, toSkia(params.targetRect), sampling, &paint);
}

void RendererSkia::drawText(const components::ComputedTextComponent& text,
                            const TextParams& params) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  // Resolve fill paint.
  SkPaint fillPaint = basePaint(antialias_, params.opacity * paintOpacity_);
  fillPaint.setStyle(SkPaint::kFill_Style);
  fillPaint.setColor(toSkia(params.fillColor.rgba()));

  // Resolve stroke paint.
  const bool hasStroke = params.strokeParams.strokeWidth > 0.0;
  SkPaint strokePaint;
  if (hasStroke) {
    strokePaint = basePaint(antialias_, params.opacity * paintOpacity_);
    strokePaint.setStyle(SkPaint::kStroke_Style);
    strokePaint.setColor(toSkia(params.strokeColor.rgba()));
    strokePaint.setStrokeWidth(NarrowToFloat(params.strokeParams.strokeWidth));
    strokePaint.setStrokeMiter(NarrowToFloat(params.strokeParams.miterLimit));
    switch (params.strokeParams.lineCap) {
      case StrokeLinecap::Butt: strokePaint.setStrokeCap(SkPaint::kButt_Cap); break;
      case StrokeLinecap::Round: strokePaint.setStrokeCap(SkPaint::kRound_Cap); break;
      case StrokeLinecap::Square: strokePaint.setStrokeCap(SkPaint::kSquare_Cap); break;
    }
    switch (params.strokeParams.lineJoin) {
      case StrokeLinejoin::Miter:
      case StrokeLinejoin::MiterClip:
      case StrokeLinejoin::Arcs:
        strokePaint.setStrokeJoin(SkPaint::kMiter_Join);
        break;
      case StrokeLinejoin::Round: strokePaint.setStrokeJoin(SkPaint::kRound_Join); break;
      case StrokeLinejoin::Bevel: strokePaint.setStrokeJoin(SkPaint::kBevel_Join); break;
    }
  }

  // Resolve typeface.
  const SmallVector<RcString, 1>& families = params.fontFamilies;
  const std::string familyName = families.empty() ? "" : families[0].str();
  sk_sp<SkTypeface> typeface = fontMgr_->matchFamilyStyle(familyName.c_str(), SkFontStyle());
  if (!typeface) {
    typeface = fontMgr_->makeFromData(SkData::MakeWithoutCopy(
        embedded::kPublicSansMediumOtf.data(), embedded::kPublicSansMediumOtf.size()));
  }

  const SkScalar fontSizePx = static_cast<SkScalar>(
      params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));

  SkFont font(typeface, fontSizePx);
  font.setEdging(SkFont::Edging::kSubpixelAntiAlias);

#ifdef DONNER_TEXT_SHAPING_ENABLED
  // When text shaping is enabled, use TextShaper for layout and drawGlyphs() for rendering.
  // This provides full OpenType GSUB/GPOS support with identical shaping across backends.
  {
    static FontManager fontManager;
    TextShaper shaper(fontManager);
    std::vector<ShapedTextRun> runs = shaper.layout(text, params);

    // Create SkTypeface from the FontManager's font data so glyph IDs match HarfBuzz output.
    sk_sp<SkTypeface> shapedTypeface;
    if (!runs.empty() && runs[0].font) {
      const auto fontBytes = fontManager.fontData(runs[0].font);
      if (!fontBytes.empty()) {
        shapedTypeface = fontMgr_->makeFromData(
            SkData::MakeWithoutCopy(fontBytes.data(), fontBytes.size()));
      }
    }
    if (!shapedTypeface) {
      shapedTypeface = typeface;
    }

    SkFont shapedFont(shapedTypeface, fontSizePx);
    shapedFont.setEdging(SkFont::Edging::kSubpixelAntiAlias);

    for (const auto& run : runs) {
      if (run.glyphs.empty()) {
        continue;
      }

      // Convert shaped glyphs to Skia arrays.
      const auto glyphCount = static_cast<int>(run.glyphs.size());
      std::vector<SkGlyphID> skGlyphs(run.glyphs.size());
      std::vector<SkPoint> skPositions(run.glyphs.size());

      for (size_t i = 0; i < run.glyphs.size(); ++i) {
        skGlyphs[i] = static_cast<SkGlyphID>(run.glyphs[i].glyphIndex);
        skPositions[i] =
            SkPoint::Make(NarrowToFloat(run.glyphs[i].xPosition),
                          NarrowToFloat(run.glyphs[i].yPosition));
      }

      const SkPoint origin = SkPoint::Make(0, 0);

      if (run.glyphs[0].rotateDegrees != 0.0) {
        // Per-glyph rotation: draw each glyph individually with rotation.
        for (int i = 0; i < glyphCount; ++i) {
          currentCanvas_->save();
          currentCanvas_->translate(skPositions[i].x(), skPositions[i].y());
          currentCanvas_->rotate(NarrowToFloat(run.glyphs[i].rotateDegrees));
          if (hasStroke) {
            currentCanvas_->drawGlyphs(1, &skGlyphs[i], &origin, origin, shapedFont, strokePaint);
          }
          currentCanvas_->drawGlyphs(1, &skGlyphs[i], &origin, origin, shapedFont, fillPaint);
          currentCanvas_->restore();
        }
      } else {
        // No rotation: batch draw all glyphs.
        if (hasStroke) {
          currentCanvas_->drawGlyphs(glyphCount, skGlyphs.data(), skPositions.data(), origin,
                                     shapedFont, strokePaint);
        }
        currentCanvas_->drawGlyphs(glyphCount, skGlyphs.data(), skPositions.data(), origin,
                                   shapedFont, fillPaint);
      }

      // Draw text-decoration lines.
      if (params.textDecoration != TextDecoration::None) {
        SkFontMetrics metrics;
        shapedFont.getMetrics(&metrics);

        const SkScalar firstX = NarrowToFloat(run.glyphs.front().xPosition);
        const SkScalar lastEnd = NarrowToFloat(run.glyphs.back().xPosition +
                                               run.glyphs.back().xAdvance);
        const SkScalar textWidth = lastEnd - firstX;
        const SkScalar y = NarrowToFloat(run.glyphs.front().yPosition);

        SkScalar decoY = y;
        SkScalar thickness = metrics.fUnderlineThickness > 0 ? metrics.fUnderlineThickness
                                                             : fontSizePx / 18.0f;

        if (params.textDecoration == TextDecoration::Underline) {
          decoY = y + (metrics.fUnderlinePosition > 0 ? metrics.fUnderlinePosition
                                                      : fontSizePx * 0.15f);
        } else if (params.textDecoration == TextDecoration::Overline) {
          decoY = y + metrics.fAscent;
        } else if (params.textDecoration == TextDecoration::LineThrough) {
          decoY = y + (metrics.fStrikeoutPosition != 0 ? metrics.fStrikeoutPosition
                                                       : metrics.fAscent * 0.35f);
          if (metrics.fStrikeoutThickness > 0) {
            thickness = metrics.fStrikeoutThickness;
          }
        }

        SkRect decoRect = SkRect::MakeXYWH(firstX, decoY - thickness / 2, textWidth, thickness);
        if (hasStroke) {
          currentCanvas_->drawRect(decoRect, strokePaint);
        }
        currentCanvas_->drawRect(decoRect, fillPaint);
      }
    }
  }
#else
  // Compute dominant-baseline shift using Skia font metrics.
  SkScalar baselineShift = 0;
  if (params.dominantBaseline != DominantBaseline::Auto &&
      params.dominantBaseline != DominantBaseline::Alphabetic) {
    SkFontMetrics fm;
    font.getMetrics(&fm);
    // Skia: fAscent < 0 (above baseline), fDescent > 0 (below baseline).
    // Negate to get positive-up shift values matching the stb_truetype convention.
    switch (params.dominantBaseline) {
      case DominantBaseline::Auto:
      case DominantBaseline::Alphabetic:
        break;
      case DominantBaseline::Middle:
      case DominantBaseline::Central:
        baselineShift = -(fm.fAscent + fm.fDescent) * 0.5f;
        break;
      case DominantBaseline::Hanging:
        baselineShift = -fm.fAscent * 0.8f;
        break;
      case DominantBaseline::Mathematical:
        baselineShift = -fm.fAscent * 0.5f;
        break;
      case DominantBaseline::TextTop:
        baselineShift = -fm.fAscent;
        break;
      case DominantBaseline::TextBottom:
      case DominantBaseline::Ideographic:
        baselineShift = -fm.fDescent;
        break;
    }
  }

  for (const auto& span : text.spans) {
    SkScalar x = static_cast<SkScalar>(
        span.x.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X) +
        span.dx.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X));
    const SkScalar y = static_cast<SkScalar>(
        span.y.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y) +
        span.dy.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y)) +
        baselineShift;

    // Use the span slice, not the full text string.
    const char* spanData = span.text.data() + span.start;
    const size_t spanLen = span.end - span.start;

    // Apply text-anchor adjustment.
    if (params.textAnchor != TextAnchor::Start) {
      const SkScalar textWidth = font.measureText(spanData, spanLen, SkTextEncoding::kUTF8);
      if (params.textAnchor == TextAnchor::Middle) {
        x -= textWidth / 2;
      } else if (params.textAnchor == TextAnchor::End) {
        x -= textWidth;
      }
    }

    if (span.rotateDegrees != 0.0) {
      // Per-glyph rotation: convert to glyphs and draw individually.
      const int glyphCount = font.countText(spanData, spanLen, SkTextEncoding::kUTF8);
      if (glyphCount <= 0) {
        continue;
      }

      std::vector<SkGlyphID> glyphs(static_cast<size_t>(glyphCount));
      font.textToGlyphs(spanData, spanLen, SkTextEncoding::kUTF8, glyphs.data(), glyphCount);

      std::vector<SkScalar> widths(static_cast<size_t>(glyphCount));
      font.getWidths(glyphs.data(), glyphCount, widths.data());

      SkScalar penX = x;
      const SkPoint origin = SkPoint::Make(0, 0);
      for (int i = 0; i < glyphCount; ++i) {
        currentCanvas_->save();
        currentCanvas_->translate(penX, y);
        currentCanvas_->rotate(static_cast<SkScalar>(span.rotateDegrees));
        // SVG stroke is drawn first (behind fill), then fill on top.
        if (hasStroke) {
          currentCanvas_->drawGlyphs(1, &glyphs[i], &origin, origin, font, strokePaint);
        }
        currentCanvas_->drawGlyphs(1, &glyphs[i], &origin, origin, font, fillPaint);
        currentCanvas_->restore();
        penX += widths[i];
      }
    } else {
      // No rotation: use drawSimpleText for better performance.
      // SVG stroke is drawn first (behind fill), then fill on top.
      if (hasStroke) {
        currentCanvas_->drawSimpleText(spanData, spanLen, SkTextEncoding::kUTF8, x, y, font,
                                       strokePaint);
      }
      currentCanvas_->drawSimpleText(spanData, spanLen, SkTextEncoding::kUTF8, x, y, font,
                                     fillPaint);
    }

    // Draw text-decoration lines.
    if (params.textDecoration != TextDecoration::None) {
      SkFontMetrics metrics;
      font.getMetrics(&metrics);

      const SkScalar textWidth = font.measureText(spanData, spanLen, SkTextEncoding::kUTF8);
      SkScalar decoY = y;
      SkScalar thickness = metrics.fUnderlineThickness > 0 ? metrics.fUnderlineThickness
                                                           : fontSizePx / 18.0f;

      if (params.textDecoration == TextDecoration::Underline) {
        decoY = y + (metrics.fUnderlinePosition > 0 ? metrics.fUnderlinePosition
                                                    : fontSizePx * 0.15f);
      } else if (params.textDecoration == TextDecoration::Overline) {
        decoY = y + metrics.fAscent;  // fAscent is negative (above baseline)
      } else if (params.textDecoration == TextDecoration::LineThrough) {
        decoY = y + (metrics.fStrikeoutPosition != 0 ? metrics.fStrikeoutPosition
                                                     : metrics.fAscent * 0.35f);
        if (metrics.fStrikeoutThickness > 0) {
          thickness = metrics.fStrikeoutThickness;
        }
      }

      SkRect decoRect = SkRect::MakeXYWH(x, decoY - thickness / 2, textWidth, thickness);
      // SVG stroke first, then fill.
      if (hasStroke) {
        currentCanvas_->drawRect(decoRect, strokePaint);
      }
      currentCanvas_->drawRect(decoRect, fillPaint);
    }
  }
#endif
}

RendererBitmap RendererSkia::takeSnapshot() const {
  RendererBitmap snapshot;
  snapshot.dimensions = Vector2i(bitmap_.width(), bitmap_.height());
  snapshot.rowBytes = bitmap_.rowBytes();

  if (bitmap_.empty()) {
    return snapshot;
  }

  const size_t size = bitmap_.computeByteSize();
  snapshot.pixels.resize(size);
  const bool copied =
      bitmap_.readPixels(bitmap_.info(), snapshot.pixels.data(), snapshot.rowBytes, 0, 0);
  if (!copied) {
    snapshot.pixels.clear();
    snapshot.dimensions = Vector2i::Zero();
    snapshot.rowBytes = 0;
  }

  return snapshot;
}

void RendererSkia::draw(SVGDocument& document) {
  RendererDriver driver(*this, verbose_);
  driver.draw(document);
}

std::string RendererSkia::drawIntoAscii(SVGDocument& document) {
  // Render directly into a grayscale bitmap to match the expected ASCII output.
  // This produces different results than rendering into RGBA and converting because
  // Skia's compositing differs between color types.
  const Vector2i renderingSize = document.canvasSize();
  assert(renderingSize.x <= 64 && renderingSize.y <= 64 &&
         "Rendering size must be less than or equal to 64x64");

  SkBitmap grayBitmap;
  grayBitmap.allocPixels(SkImageInfo::Make(renderingSize.x, renderingSize.y, kGray_8_SkColorType,
                                           kOpaque_SkAlphaType));
  SkCanvas grayCanvas(grayBitmap);

  // Set the grayscale canvas so beginFrame uses it instead of creating an RGBA surface.
  externalCanvas_ = &grayCanvas;
  draw(document);
  externalCanvas_ = nullptr;

  std::string asciiArt;
  asciiArt.reserve(static_cast<size_t>(renderingSize.x * renderingSize.y + renderingSize.y));

  static const std::array<char, 10> grayscaleTable = {'.', ',', ':', '-', '=',
                                                      '+', '*', '#', '%', '@'};

  for (int y = 0; y < renderingSize.y; ++y) {
    for (int x = 0; x < renderingSize.x; ++x) {
      const uint8_t pixel = *grayBitmap.getAddr8(x, y);
      size_t index = pixel / static_cast<size_t>(256 / grayscaleTable.size());
      if (index >= grayscaleTable.size()) {
        index = grayscaleTable.size() - 1;
      }
      asciiArt += grayscaleTable.at(index);
    }
    asciiArt += '\n';
  }

  return asciiArt;
}

sk_sp<SkPicture> RendererSkia::drawIntoSkPicture(SVGDocument& document) {
  SkPictureRecorder recorder;
  const Vector2i renderingSize = document.canvasSize();
  externalCanvas_ = recorder.beginRecording(SkRect::MakeWH(static_cast<SkScalar>(renderingSize.x),
                                                           static_cast<SkScalar>(renderingSize.y)));
  draw(document);
  return recorder.finishRecordingAsPicture();
}

bool RendererSkia::save(const char* filename) {
  const RendererBitmap snapshot = takeSnapshot();
  if (snapshot.empty()) {
    return false;
  }

  return RendererImageIO::writeRgbaPixelsToPngFile(filename, snapshot.pixels, snapshot.dimensions.x,
                                                   snapshot.dimensions.y);
}

std::span<const uint8_t> RendererSkia::pixelData() const {
  return std::span<const uint8_t>(
      bitmap_.computeByteSize() == 0 ? nullptr : static_cast<const uint8_t*>(bitmap_.getPixels()),
      bitmap_.computeByteSize());
}

}  // namespace donner::svg
