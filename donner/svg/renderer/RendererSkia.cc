#include "donner/svg/renderer/RendererSkia.h"

// Skia
#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPathMeasure.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkLumaColorFilter.h"
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
// Donner
#include "donner/base/Length.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererImageIO.h"

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

SkColor toSkia(const css::RGBA rgba) {
  return SkColorSetARGB(rgba.a, rgba.r, rgba.g, rgba.b);
}

SkTileMode toSkia(GradientSpreadMethod spreadMethod);

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
    const components::ComputedLocalTransformComponent* maybeTransformComponent, const Boxd& viewBox) {
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

  const bool objectBoundingBox = computedGradient->gradientUnits == GradientUnits::ObjectBoundingBox;
  const bool numbersArePercent = objectBoundingBox;

  // Use a generous tolerance for degenerate bounding box detection: cubic bezier computation
  // can produce floating-point artifacts (e.g. 1.4e-14 width for a perfectly vertical path).
  constexpr double kDegenerateBBoxTolerance = 1e-6;
  if (objectBoundingBox &&
      (NearZero(pathBounds.width(), kDegenerateBBoxTolerance) ||
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
    paint.setShader(SkGradientShader::MakeLinear(points, colors.data(), positions.data(),
                                                 static_cast<int>(positions.size()),
                                                 toSkia(computedGradient->spreadMethod), 0,
                                                 &skGradientFromGradientUnits));
    return paint;
  }

  if (const auto* radial = handle.try_get<components::ComputedRadialGradientComponent>()) {
    const double radius = resolveGradientCoord(radial->r, bounds, numbersArePercent);
    const Vector2d center = resolveGradientCoords(radial->cx, radial->cy, bounds, numbersArePercent);
    const double focalRadius = resolveGradientCoord(radial->fr, bounds, numbersArePercent);
    const Vector2d focalCenter = resolveGradientCoords(radial->fx.value_or(radial->cx),
                                                      radial->fy.value_or(radial->cy), bounds,
                                                      numbersArePercent);

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
}

void RendererSkia::setTransform(const Transformd& transform) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  currentCanvas_->setMatrix(toSkiaMatrix(transform));
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

  currentCanvas_->save();
  if (clip.clipRect.has_value()) {
    currentCanvas_->clipRect(toSkia(*clip.clipRect));
  }

  if (!clip.clipPaths.empty()) {
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
        // Intersect the accumulated layer path with this path.
        assert(!layeredPaths.empty());
        SkPath layerPath = layeredPaths[layeredPaths.size() - 1];
        layeredPaths.pop_back();
        // Transform the layer path into the current path's coordinate space before intersecting.
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

    currentCanvas_->clipPath(fullPath, SkClipOp::kIntersect, true);
  }

  ++clipDepth_;
}

void RendererSkia::popClip() {
  if (currentCanvas_ == nullptr || clipDepth_ <= 0) {
    return;
  }

  currentCanvas_->restore();
  --clipDepth_;
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

void RendererSkia::pushFilterLayer(std::span<const FilterEffect> effects) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  SkPaint filterPaint;
  filterPaint.setAntiAlias(antialias_);
  for (const FilterEffect& effect : effects) {
    std::visit(
        [&](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, FilterEffect::Blur>) {
            filterPaint.setImageFilter(SkImageFilters::Blur(
                static_cast<float>(e.stdDeviationX.value),
                static_cast<float>(e.stdDeviationY.value), nullptr));
          }
        },
        effect.value);
  }
  currentCanvas_->saveLayer(nullptr, &filterPaint);
}

void RendererSkia::popFilterLayer() {
  if (currentCanvas_ == nullptr) {
    return;
  }

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

  // Layer 1: Isolation layer so that SrcIn blend doesn't affect the base canvas.
  currentCanvas_->saveLayer(nullptr, nullptr);

  // Layer 2: Luma color filter converts mask content RGB to alpha.
  // This layer is restored in transitionMaskToContent(), so the luma filter
  // only applies to the mask content, not the actual drawing content.
  SkPaint maskFilter;
  maskFilter.setColorFilter(SkLumaColorFilter::Make());
  currentCanvas_->saveLayer(nullptr, &maskFilter);

  if (maskBounds.has_value()) {
    currentCanvas_->clipRect(toSkia(*maskBounds), SkClipOp::kIntersect, true);
  }
}

void RendererSkia::transitionMaskToContent() {
  if (currentCanvas_ == nullptr) {
    return;
  }

  // Restore the Luma layer — mask content luminance becomes alpha in the isolation layer.
  currentCanvas_->restore();

  // Layer 3: SrcIn blend — multiplies content by the mask alpha from the isolation layer.
  SkPaint maskPaint;
  maskPaint.setBlendMode(SkBlendMode::kSrcIn);
  currentCanvas_->saveLayer(nullptr, &maskPaint);
}

void RendererSkia::popMask() {
  if (currentCanvas_ == nullptr) {
    return;
  }

  // Restore the SrcIn content layer, then the isolation layer.
  currentCanvas_->restore();
  currentCanvas_->restore();
}

void RendererSkia::beginPatternTile(const Boxd& tileRect, const Transformd& patternToTarget) {
  if (currentCanvas_ == nullptr) {
    return;
  }

  PatternState state;
  state.patternToTarget = patternToTarget;
  state.savedCanvas = currentCanvas_;
  state.recorder = std::make_unique<SkPictureRecorder>();
  currentCanvas_ = state.recorder->beginRecording(toSkia(tileRect));
  patternStack_.push_back(std::move(state));
}

void RendererSkia::endPatternTile(bool forStroke) {
  if (patternStack_.empty()) {
    return;
  }

  PatternState state = std::move(patternStack_.back());
  patternStack_.pop_back();

  currentCanvas_ = state.savedCanvas;

  const SkMatrix skPatternToTarget = toSkiaMatrix(state.patternToTarget);
  sk_sp<SkPicture> picture = state.recorder->finishRecordingAsPicture();
  state.recorder.reset();

  if (picture == nullptr) {
    return;
  }

  const SkRect tileRect = picture->cullRect();
  SkPaint patternPaint;
  patternPaint.setAntiAlias(antialias_);
  patternPaint.setShader(picture->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat,
                                             SkFilterMode::kLinear, &skPatternToTarget, &tileRect));

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
    std::cout << "[Skia::drawPath] saveCount=" << currentCanvas_->getSaveCount()
              << " matrix=[" << m.getScaleX() << "," << m.getScaleY() << ","
              << m.getTranslateX() << "," << m.getTranslateY() << "]"
              << " bounds=" << path.path.bounds()
              << " fillOpacity=" << paint_.fillOpacity
              << " fillIsSolid=" << isSolid
              << " isRef=" << std::holds_alternative<components::PaintResolvedReference>(paint_.fill)
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

  if (std::optional<SkPaint> fillPaint = makeFillPaint(path.path.bounds())) {
    currentCanvas_->drawPath(skPath, *fillPaint);
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

  if (std::optional<SkPaint> strokePaint =
          makeStrokePaint(path.path.bounds(), adjustedStroke)) {
    currentCanvas_->drawPath(skPath, *strokePaint);
  }
}

void RendererSkia::drawRect(const Boxd& rect, const StrokeParams& stroke) {
  const SkRect skRect = toSkia(rect);
  if (std::optional<SkPaint> fillPaint = makeFillPaint(rect)) {
    currentCanvas_->drawRect(skRect, *fillPaint);
  }

  if (std::optional<SkPaint> strokePaint = makeStrokePaint(rect, stroke)) {
    currentCanvas_->drawRect(skRect, *strokePaint);
  }
}

void RendererSkia::drawEllipse(const Boxd& bounds, const StrokeParams& stroke) {
  SkPath ellipse;
  ellipse.addOval(toSkia(bounds));
  if (std::optional<SkPaint> fillPaint = makeFillPaint(bounds)) {
    currentCanvas_->drawPath(ellipse, *fillPaint);
  }

  if (std::optional<SkPaint> strokePaint = makeStrokePaint(bounds, stroke)) {
    currentCanvas_->drawPath(ellipse, *strokePaint);
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

  SkPaint paint = basePaint(antialias_, params.opacity * paintOpacity_);
  paint.setColor(toSkia(params.fillColor.rgba()));

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

  for (const auto& span : text.spans) {
    const SkScalar x = static_cast<SkScalar>(
        span.x.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X) +
        span.dx.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X));
    const SkScalar y = static_cast<SkScalar>(
        span.y.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y) +
        span.dy.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y));
    currentCanvas_->drawSimpleText(span.text.data(), span.text.size(), SkTextEncoding::kUTF8, x, y,
                                   font, paint);
  }
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
