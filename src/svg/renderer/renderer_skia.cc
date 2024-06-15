#include "src/svg/renderer/renderer_skia.h"

// Skia
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPathMeasure.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkStream.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkImageFilters.h"
//
#include "src/svg/components/filter/filter_component.h"
#include "src/svg/components/filter/filter_effect.h"
#include "src/svg/components/id_component.h"  // For verbose logging.
#include "src/svg/components/layout/layout_system.h"
#include "src/svg/components/layout/sized_element_component.h"
#include "src/svg/components/paint/gradient_component.h"
#include "src/svg/components/paint/linear_gradient_component.h"
#include "src/svg/components/paint/pattern_component.h"
#include "src/svg/components/paint/radial_gradient_component.h"
#include "src/svg/components/path_length_component.h"
#include "src/svg/components/preserve_aspect_ratio_component.h"
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/components/rendering_instance_component.h"
#include "src/svg/components/shadow/computed_shadow_tree_component.h"
#include "src/svg/components/shadow/shadow_branch.h"
#include "src/svg/components/shadow/shadow_entity_component.h"
#include "src/svg/components/shape/computed_path_component.h"
#include "src/svg/components/style/computed_style_component.h"
#include "src/svg/components/transform_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/components/viewbox_component.h"
#include "src/svg/graph/reference.h"
#include "src/svg/renderer/common/rendering_instance_view.h"
#include "src/svg/renderer/renderer_image_io.h"
#include "src/svg/renderer/renderer_utils.h"

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

SkM44 toSkia(const Transformd& transform) {
  return SkM44{float(transform.data[0]),
               float(transform.data[2]),
               0.0f,
               float(transform.data[4]),
               float(transform.data[1]),
               float(transform.data[3]),
               0.0f,
               float(transform.data[5]),
               0.0f,
               0.0f,
               1.0f,
               0.0f,
               0.0f,
               0.0f,
               0.0f,
               1.0f};
}

SkRect toSkia(const Boxd& box) {
  return SkRect::MakeLTRB(
      static_cast<SkScalar>(box.topLeft.x), static_cast<SkScalar>(box.topLeft.y),
      static_cast<SkScalar>(box.bottomRight.x), static_cast<SkScalar>(box.bottomRight.y));
}

SkColor toSkia(const css::RGBA rgba) {
  return SkColorSetARGB(rgba.a, rgba.r, rgba.g, rgba.b);
}

SkPaint::Cap toSkia(StrokeLinecap lineCap) {
  switch (lineCap) {
    case StrokeLinecap::Butt: return SkPaint::Cap::kButt_Cap;
    case StrokeLinecap::Round: return SkPaint::Cap::kRound_Cap;
    case StrokeLinecap::Square: return SkPaint::Cap::kSquare_Cap;
  }
}

SkPaint::Join toSkia(StrokeLinejoin lineJoin) {
  // TODO: Implement MiterClip and Arcs. For now, fallback to Miter, which is the default linejoin,
  // since the feature is not implemented.
  switch (lineJoin) {
    case StrokeLinejoin::Miter: return SkPaint::Join::kMiter_Join;
    case StrokeLinejoin::MiterClip: return SkPaint::Join::kMiter_Join;
    case StrokeLinejoin::Round: return SkPaint::Join::kRound_Join;
    case StrokeLinejoin::Bevel: return SkPaint::Join::kBevel_Join;
    case StrokeLinejoin::Arcs: return SkPaint::Join::kMiter_Join;
  }
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
}

}  // namespace

class RendererSkia::Impl {
public:
  Impl(RendererSkia& renderer, RenderingInstanceView&& view)
      : renderer_(renderer), view_(std::move(view)) {}

  void drawUntil(Registry& registry, Entity endEntity) {
    bool foundEndEntity = false;

    while (!view_.done() && !foundEndEntity) {
      foundEndEntity = view_.currentEntity() == endEntity;

      const components::RenderingInstanceComponent& instance = view_.get();
      const Entity entity = view_.currentEntity();
      view_.advance();

      if (renderer_.verbose_) {
        std::cout << "Rendering "
                  << TypeToString(
                         registry.get<components::TreeComponent>(instance.dataEntity).type())
                  << " ";

        if (const auto* idComponent =
                registry.try_get<components::IdComponent>(instance.dataEntity)) {
          std::cout << "id=" << idComponent->id << " ";
        }

        std::cout << instance.dataEntity;
        if (instance.isShadow(registry)) {
          std::cout << " (shadow " << instance.styleHandle(registry).entity() << ")";
        }

        std::cout << " transform=" << instance.transformCanvasSpace << "\n";

        std::cout << "\n";
      }

      if (instance.clipRect) {
        renderer_.currentCanvas_->save();
        renderer_.currentCanvas_->clipRect(toSkia(instance.clipRect.value()));
      }

      renderer_.currentCanvas_->setMatrix(
          toSkia(layerBaseTransform_ * instance.transformCanvasSpace));

      const components::ComputedStyleComponent& styleComponent =
          instance.styleHandle(registry).get<components::ComputedStyleComponent>();
      const auto& properties = styleComponent.properties.value();

      if (instance.isolatedLayer) {
        // Create a new layer if opacity is less than 1.
        if (properties.opacity.getRequired() < 1.0) {
          SkPaint opacityPaint;
          opacityPaint.setAlphaf(NarrowToFloat(properties.opacity.getRequired()));

          // TODO: Calculate hint for size of layer.
          renderer_.currentCanvas_->saveLayer(nullptr, &opacityPaint);
        } else if (instance.resolvedFilter) {
          SkPaint filterPaint;
          filterPaint.setAntiAlias(renderer_.antialias_);
          createFilterPaint(filterPaint, registry, instance.resolvedFilter.value());

          // TODO: Calculate the bounds.
          renderer_.currentCanvas_->saveLayer(nullptr, &filterPaint);
        } else {
          assert(false && "Failed to find reason for isolatedLayer");
        }
      }

      if (instance.visible) {
        if (const auto* path =
                instance.dataHandle(registry).try_get<components::ComputedPathComponent>()) {
          drawPath(instance.dataHandle(registry), instance, *path,
                   styleComponent.properties.value(), styleComponent.viewbox.value(),
                   FontMetrics());
        }
      }

      if (instance.subtreeInfo) {
        subtreeMarkers_.push_back(instance.subtreeInfo.value());
      }

      while (!subtreeMarkers_.empty() && subtreeMarkers_.back().lastRenderedEntity == entity) {
        const components::SubtreeInfo subtreeInfo = subtreeMarkers_.back();
        subtreeMarkers_.pop_back();

        // SkCanvas also has restoreToCount, but it just calls restore in a loop.
        for (int i = 0; i < subtreeInfo.restorePopDepth; ++i) {
          renderer_.currentCanvas_->restore();
        }
      }
    }

    renderer_.currentCanvas_->restoreToCount(1);
  }

  void drawPath(EntityHandle dataHandle, const components::RenderingInstanceComponent& instance,
                const components::ComputedPathComponent& path, const PropertyRegistry& style,
                const Boxd& viewbox, const FontMetrics& fontMetrics) {
    if (HasPaint(instance.resolvedFill)) {
      drawPathFill(dataHandle, path, instance.resolvedFill, style, viewbox);
    }

    if (HasPaint(instance.resolvedStroke)) {
      drawPathStroke(dataHandle, path, instance.resolvedStroke, style, viewbox, fontMetrics);
    }
  }

  std::optional<SkPaint> createFallbackPaint(const components::PaintResolvedReference& ref,
                                             css::RGBA currentColor, float opacity) {
    if (ref.fallback) {
      SkPaint paint;
      paint.setAntiAlias(renderer_.antialias_);
      paint.setColor(toSkia(ref.fallback.value().resolve(currentColor, opacity)));
      return paint;
    }

    return std::nullopt;
  }

  inline Lengthd toPercent(Lengthd value, bool numbersArePercent) {
    if (!numbersArePercent) {
      return value;
    }

    if (value.unit == Lengthd::Unit::None) {
      value.value *= 100.0;
      value.unit = Lengthd::Unit::Percent;
    }

    assert(value.unit == Lengthd::Unit::Percent);
    return value;
  }

  inline SkScalar resolveGradientCoord(Lengthd value, const Boxd& viewbox, bool numbersArePercent) {
    // Not plumbing FontMetrics here, since only percentage values are accepted.
    return NarrowToFloat(toPercent(value, numbersArePercent).toPixels(viewbox, FontMetrics()));
  }

  Vector2d resolveGradientCoords(Lengthd x, Lengthd y, const Boxd& viewbox,
                                 bool numbersArePercent) {
    return Vector2d(
        toPercent(x, numbersArePercent).toPixels(viewbox, FontMetrics(), Lengthd::Extent::X),
        toPercent(y, numbersArePercent).toPixels(viewbox, FontMetrics(), Lengthd::Extent::Y));
  }

  static bool CircleContainsPoint(Vector2d center, double radius, Vector2d point) {
    return (point - center).lengthSquared() <= radius * radius;
  }

  Transformd ResolveTransform(const components::ComputedTransformComponent* maybeTransformComponent,
                              const Boxd& viewbox, const FontMetrics& fontMetrics) {
    if (maybeTransformComponent) {
      return maybeTransformComponent->rawCssTransform.compute(viewbox, fontMetrics);
    } else {
      return Transformd();
    }
  }

  std::optional<SkPaint> instantiateGradient(
      EntityHandle target, const components::ComputedGradientComponent& computedGradient,
      const components::PaintResolvedReference& ref, const Boxd& pathBounds, const Boxd& viewbox,
      css::RGBA currentColor, float opacity) {
    // Apply gradientUnits and gradientTransform.
    const bool objectBoundingBox =
        computedGradient.gradientUnits == GradientUnits::ObjectBoundingBox;

    const auto* maybeTransformComponent = target.try_get<components::ComputedTransformComponent>();

    bool numbersArePercent = false;
    Transformd transform;
    if (objectBoundingBox) {
      // From https://www.w3.org/TR/SVG2/coords.html#ObjectBoundingBoxUnits:
      //
      // > Keyword objectBoundingBox should not be used when the geometry of the applicable
      // > element has no width or no height, such as the case of a horizontal or vertical line,
      // > even when the line has actual thickness when viewed due to having a non-zero stroke
      // > width since stroke width is ignored for bounding box calculations. When the geometry
      // > of the applicable element has no width or height and objectBoundingBox is specified,
      // > then the given effect (e.g., a gradient or a filter) will be ignored.
      //
      if (NearZero(pathBounds.width()) || NearZero(pathBounds.height())) {
        return createFallbackPaint(ref, currentColor, opacity);
      }

      transform = ResolveTransform(maybeTransformComponent, kUnitPathBounds, FontMetrics());

      // Note that this applies *before* transform.
      transform *= Transformd::Scale(pathBounds.size());
      transform *= Transformd::Translate(pathBounds.topLeft);

      // TODO: Can numbersArePercent be represented by the transform instead?
      numbersArePercent = true;
    } else {
      transform = ResolveTransform(maybeTransformComponent, viewbox, FontMetrics());
    }

    const Boxd& bounds = objectBoundingBox ? kUnitPathBounds : viewbox;

    std::vector<SkScalar> pos;
    std::vector<SkColor> color;
    for (const GradientStop& stop : computedGradient.stops) {
      pos.push_back(stop.offset);
      color.push_back(toSkia(stop.color.resolve(currentColor, stop.opacity * opacity)));
    }

    assert(pos.size() == color.size());

    // From https://www.w3.org/TR/SVG2/pservers.html#StopNotes:
    //
    // > It is necessary that at least two stops defined to have a gradient effect. If no stops
    // > are defined, then painting shall occur as if 'none' were specified as the paint style.
    // > If one stop is defined, then paint with the solid color fill using the color defined
    // > for that gradient stop.
    //
    if (pos.empty() || pos.size() > std::numeric_limits<int>::max()) {
      return createFallbackPaint(ref, currentColor, opacity);
    }

    const int numStops = static_cast<int>(pos.size());
    if (numStops == 1) {
      SkPaint paint;
      paint.setAntiAlias(renderer_.antialias_);
      paint.setColor(color[0]);
      return paint;
    }

    // Transform applied to the gradient coordinates, and for radial gradients the focal point and
    // radius.
    const SkMatrix localMatrix = toSkiaMatrix(transform);

    if (const auto* linearGradient =
            target.try_get<components::ComputedLinearGradientComponent>()) {
      const SkPoint points[] = {toSkia(resolveGradientCoords(linearGradient->x1, linearGradient->y1,
                                                             bounds, numbersArePercent)),
                                toSkia(resolveGradientCoords(linearGradient->x2, linearGradient->y2,
                                                             bounds, numbersArePercent))};

      SkPaint paint;
      paint.setAntiAlias(renderer_.antialias_);
      paint.setShader(SkGradientShader::MakeLinear(points, color.data(), pos.data(), numStops,
                                                   toSkia(computedGradient.spreadMethod), 0,
                                                   &localMatrix));
      return paint;
    } else {
      const auto& radialGradient = target.get<components::ComputedRadialGradientComponent>();
      const Vector2d center =
          resolveGradientCoords(radialGradient.cx, radialGradient.cy, bounds, numbersArePercent);
      const SkScalar radius = resolveGradientCoord(radialGradient.r, bounds, numbersArePercent);

      Vector2d focalCenter = resolveGradientCoords(radialGradient.fx.value_or(radialGradient.cx),
                                                   radialGradient.fy.value_or(radialGradient.cy),
                                                   bounds, numbersArePercent);
      const SkScalar focalRadius =
          resolveGradientCoord(radialGradient.fr, bounds, numbersArePercent);

      if (NearZero(radius)) {
        SkPaint paint;
        paint.setAntiAlias(renderer_.antialias_);
        paint.setColor(color.back());
        return paint;
      }

      // NOTE: In SVG1, if the focal point lies outside of the circle, the focal point set to
      // the intersection of the circle and the focal point.
      //
      // This changes in SVG2, where a cone is created,
      // https://www.w3.org/TR/SVG2/pservers.html#RadialGradientNotes:
      //
      // > If the start circle defined by ‘fx’, ‘fy’ and ‘fr’ lies outside the end circle
      // > defined by ‘cx’, ‘cy’, and ‘r’, effectively a cone is created, touched by the two
      // > circles. Areas outside the cone stay untouched by the gradient (transparent black).
      //
      // Skia will automatically create the cone, but we need to handle the degenerate case:
      //
      // > If the start [focal] circle fully overlaps with the end circle, no gradient is drawn.
      // > The area stays untouched (transparent black).
      //
      const double distanceBetweenCenters = (center - focalCenter).length();
      if (distanceBetweenCenters + radius <= focalRadius) {
        return std::nullopt;
      }

      SkPaint paint;
      paint.setAntiAlias(renderer_.antialias_);
      if (NearZero(focalRadius) && focalCenter == center) {
        paint.setShader(
            SkGradientShader::MakeRadial(toSkia(center), radius, color.data(), pos.data(), numStops,
                                         toSkia(computedGradient.spreadMethod), 0, &localMatrix));
      } else {
        paint.setShader(SkGradientShader::MakeTwoPointConical(
            toSkia(focalCenter), focalRadius, toSkia(center), radius, color.data(), pos.data(),
            numStops, toSkia(computedGradient.spreadMethod), 0, &localMatrix));
      }
      return paint;
    }
  }

  PreserveAspectRatio GetPreserveAspectRatio(EntityHandle handle) {
    if (const auto* preserveAspectRatioComponent =
            handle.try_get<components::PreserveAspectRatioComponent>()) {
      return preserveAspectRatioComponent->preserveAspectRatio;
    }

    return PreserveAspectRatio::None();
  }

  std::optional<SkPaint> instantiatePattern(
      components::ShadowBranchType branchType, EntityHandle dataHandle, EntityHandle target,
      const components::ComputedPatternComponent& computedPattern,
      const components::PaintResolvedReference& ref, const Boxd& pathBounds, const Boxd& viewbox,
      css::RGBA currentColor, float opacity) {
    if (!ref.subtreeInfo) {
      // Subtree did not instantiate, indicating that recursion was detected.
      return std::nullopt;
    }

    Registry& registry = *dataHandle.registry();
    const bool objectBoundingBox = computedPattern.patternUnits == PatternUnits::ObjectBoundingBox;
    const bool patternContentObjectBoundingBox =
        computedPattern.patternContentUnits == PatternContentUnits::ObjectBoundingBox;

    const auto* maybeTransformComponent = target.try_get<components::ComputedTransformComponent>();

    Transformd transform;
    Transformd contentRootTransform;
    std::optional<Boxd> patternBounds;

    if (NearZero(computedPattern.tileRect.width()) || NearZero(computedPattern.tileRect.height())) {
      return createFallbackPaint(ref, currentColor, opacity);
    }

    if (objectBoundingBox) {
      // From https://www.w3.org/TR/SVG2/coords.html#ObjectBoundingBoxUnits:
      //
      // > Keyword objectBoundingBox should not be used when the geometry of the applicable
      // > element has no width or no height, such as the case of a horizontal or vertical line,
      // > even when the line has actual thickness when viewed due to having a non-zero stroke
      // > width since stroke width is ignored for bounding box calculations. When the geometry
      // > of the applicable element has no width or height and objectBoundingBox is specified,
      // > then the given effect (e.g., a gradient or a filter) will be ignored.
      //
      if (NearZero(pathBounds.width()) || NearZero(pathBounds.height())) {
        return createFallbackPaint(ref, currentColor, opacity);
      }

      transform = ResolveTransform(maybeTransformComponent, kUnitPathBounds, FontMetrics());

      // Resolve x/y/width/height in tileRect to translation/bounds.
      transform *= Transformd::Translate(pathBounds.size() * computedPattern.tileRect.topLeft);
      patternBounds = Boxd(Vector2d(), pathBounds.size() * computedPattern.tileRect.size());
    } else {
      transform = Transformd::Translate(computedPattern.tileRect.topLeft);
      transform *= ResolveTransform(maybeTransformComponent, viewbox, FontMetrics());

      patternBounds = computedPattern.tileRect;
    }

    if (patternContentObjectBoundingBox) {
      contentRootTransform = Transformd::Scale(pathBounds.size());
    }

    if (patternBounds) {
      const SkRect skPatternBounds = toSkia(patternBounds.value());
      const SkRect tileRect = toSkia(Boxd(Vector2d(), patternBounds.value().size()));

      SkPictureRecorder recorder;
      renderer_.currentCanvas_ = recorder.beginRecording(skPatternBounds);
      layerBaseTransform_ = contentRootTransform;

      // Render the subtree into the offscreen SkPictureRecorder.
      assert(ref.subtreeInfo);
      drawUntil(registry, ref.subtreeInfo->lastRenderedEntity);

      renderer_.currentCanvas_ = renderer_.rootCanvas_;
      layerBaseTransform_ = Transformd();

      // Transform to apply to the pattern contents.
      const SkMatrix localMatrix = toSkiaMatrix(transform);

      SkPaint skPaint;
      skPaint.setAntiAlias(renderer_.antialias_);
      skPaint.setShader(recorder.finishRecordingAsPicture()->makeShader(
          SkTileMode::kRepeat, SkTileMode::kRepeat, SkFilterMode::kLinear, &localMatrix,
          &tileRect));
      return skPaint;
    }

    return std::nullopt;
  }

  std::optional<SkPaint> instantiatePaintReference(components::ShadowBranchType branchType,
                                                   EntityHandle dataHandle,
                                                   const components::PaintResolvedReference& ref,
                                                   const Boxd& pathBounds, const Boxd& viewbox,
                                                   css::RGBA currentColor, float opacity) {
    const EntityHandle target = ref.reference.handle;

    if (const auto* computedGradient = target.try_get<components::ComputedGradientComponent>()) {
      return instantiateGradient(target, *computedGradient, ref, pathBounds, viewbox, currentColor,
                                 opacity);
    } else if (const auto* computedPattern =
                   target.try_get<components::ComputedPatternComponent>()) {
      return instantiatePattern(branchType, dataHandle, target, *computedPattern, ref, pathBounds,
                                viewbox, currentColor, opacity);
    }

    UTILS_UNREACHABLE();  // The computed tree should invalidate any references that don't point to
                          // a valid point server, see IsValidPaintServer.
  }

  void drawPathFillWithSkPaint(const components::ComputedPathComponent& path, SkPaint& skPaint,
                               const PropertyRegistry& style) {
    SkPath skPath = toSkia(path.spline);
    if (style.fillRule.get() == FillRule::EvenOdd) {
      skPath.setFillType(SkPathFillType::kEvenOdd);
    }

    skPaint.setAntiAlias(renderer_.antialias_);
    skPaint.setStyle(SkPaint::Style::kFill_Style);
    renderer_.currentCanvas_->drawPath(skPath, skPaint);
  }

  void drawPathFill(EntityHandle dataHandle, const components::ComputedPathComponent& path,
                    const components::ResolvedPaintServer& paint, const PropertyRegistry& style,
                    const Boxd& viewbox) {
    const float fillOpacity = NarrowToFloat(style.fillOpacity.get().value());

    if (const auto* solid = std::get_if<PaintServer::Solid>(&paint)) {
      SkPaint skPaint;
      skPaint.setAntiAlias(renderer_.antialias_);
      skPaint.setColor(toSkia(solid->color.resolve(style.color.getRequired().rgba(), fillOpacity)));

      drawPathFillWithSkPaint(path, skPaint, style);
    } else if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint)) {
      std::optional<SkPaint> skPaint = instantiatePaintReference(
          components::ShadowBranchType::OffscreenFill, dataHandle, *ref, path.spline.bounds(),
          viewbox, style.color.getRequired().rgba(), fillOpacity);
      if (skPaint) {
        drawPathFillWithSkPaint(path, skPaint.value(), style);
      }
    }
  }

  struct StrokeConfig {
    double strokeWidth;
    double miterLimit;
  };

  void drawPathStrokeWithSkPaint(EntityHandle dataHandle,
                                 const components::ComputedPathComponent& path,
                                 const StrokeConfig& config, SkPaint& skPaint,
                                 const PropertyRegistry& style, const Boxd& viewbox,
                                 const FontMetrics& fontMetrics) {
    const SkPath skPath = toSkia(path.spline);

    if (style.strokeDasharray.hasValue()) {
      double dashUnitsScale = 1.0;
      if (const auto* pathLength = dataHandle.try_get<components::PathLengthComponent>();
          pathLength && !NearZero(pathLength->value)) {
        // If the user specifies a path length, we need to scale between the user's length
        // and computed length.
        const double skiaLength = SkPathMeasure(skPath, false).getLength();
        dashUnitsScale = skiaLength / pathLength->value;
      }

      // Use getRequiredRef to avoid copying the vector on access.
      const std::vector<Lengthd>& dashes = style.strokeDasharray.getRequiredRef();

      // We need to repeat if there are an odd number of values, Skia requires an even number
      // of dash lengths.
      const size_t numRepeats = (dashes.size() & 1) ? 2 : 1;

      std::vector<SkScalar> skiaDashes;
      skiaDashes.reserve(dashes.size() * numRepeats);

      for (int i = 0; i < numRepeats; ++i) {
        for (const Lengthd& dash : dashes) {
          skiaDashes.push_back(
              static_cast<float>(dash.toPixels(viewbox, fontMetrics) * dashUnitsScale));
        }
      }

      skPaint.setPathEffect(SkDashPathEffect::Make(
          skiaDashes.data(), skiaDashes.size(),
          static_cast<SkScalar>(
              style.strokeDashoffset.get().value().toPixels(viewbox, fontMetrics) *
              dashUnitsScale)));
    }

    skPaint.setAntiAlias(renderer_.antialias_);
    skPaint.setStyle(SkPaint::Style::kStroke_Style);

    skPaint.setStrokeWidth(static_cast<SkScalar>(config.strokeWidth));
    skPaint.setStrokeCap(toSkia(style.strokeLinecap.get().value()));
    skPaint.setStrokeJoin(toSkia(style.strokeLinejoin.get().value()));
    skPaint.setStrokeMiter(static_cast<SkScalar>(config.miterLimit));

    renderer_.currentCanvas_->drawPath(skPath, skPaint);
  }

  void drawPathStroke(EntityHandle dataHandle, const components::ComputedPathComponent& path,
                      const components::ResolvedPaintServer& paint, const PropertyRegistry& style,
                      const Boxd& viewbox, const FontMetrics& fontMetrics) {
    const StrokeConfig config = {
        .strokeWidth = style.strokeWidth.get().value().toPixels(viewbox, fontMetrics),
        .miterLimit = style.strokeMiterlimit.get().value()};
    const double strokeOpacity = style.strokeOpacity.get().value();

    if (config.strokeWidth <= 0.0) {
      return;
    }

    if (const auto* solid = std::get_if<PaintServer::Solid>(&paint)) {
      SkPaint skPaint;
      skPaint.setAntiAlias(renderer_.antialias_);
      skPaint.setColor(toSkia(solid->color.resolve(style.color.getRequired().rgba(),
                                                   static_cast<float>(strokeOpacity))));

      drawPathStrokeWithSkPaint(dataHandle, path, config, skPaint, style, viewbox, fontMetrics);
    } else if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint)) {
      std::optional<SkPaint> skPaint = instantiatePaintReference(
          components::ShadowBranchType::OffscreenStroke, dataHandle, *ref,
          path.spline.strokeMiterBounds(config.strokeWidth, config.miterLimit), viewbox,
          style.color.getRequired().rgba(), NarrowToFloat((strokeOpacity)));
      if (skPaint) {
        drawPathStrokeWithSkPaint(dataHandle, path, config, skPaint.value(), style, viewbox,
                                  fontMetrics);
      }
    }
  }

  void createFilterChain(SkPaint& filterPaint, const std::vector<FilterEffect>& effectList) {
    for (const FilterEffect& effect : effectList) {
      std::visit(entt::overloaded{//
                                  [&](const FilterEffect::None&) {},
                                  [&](const FilterEffect::Blur& blur) {
                                    // TODO: Convert these Length units
                                    filterPaint.setImageFilter(SkImageFilters::Blur(
                                        static_cast<float>(blur.stdDeviationX.value),
                                        static_cast<float>(blur.stdDeviationY.value), nullptr));
                                  },
                                  [&](const FilterEffect::ElementReference& ref) {
                                    assert(false && "Element references must already be resolved");
                                  }},
                 effect.value);
    }
  }

  void createFilterPaint(SkPaint& filterPaint, Registry& registry,
                         const components::ResolvedFilterEffect& filter) {
    if (const auto* effects = std::get_if<std::vector<FilterEffect>>(&filter)) {
      createFilterChain(filterPaint, *effects);
    } else if (const auto* reference = std::get_if<ResolvedReference>(&filter)) {
      if (const auto* filter = registry.try_get<components::ComputedFilterComponent>(*reference)) {
        createFilterChain(filterPaint, filter->effectChain);
      }
    }
  }

private:
  RendererSkia& renderer_;
  RenderingInstanceView view_;

  std::vector<components::SubtreeInfo> subtreeMarkers_;
  Transformd layerBaseTransform_ = Transformd();
};

RendererSkia::RendererSkia(bool verbose) : verbose_(verbose) {}

RendererSkia::~RendererSkia() {}

void RendererSkia::draw(SVGDocument& document) {
  Registry& registry = document.registry();
  const Entity rootEntity = document.rootEntity();

  // TODO: Plumb outWarnings.
  RendererUtils::prepareDocumentForRendering(document, verbose_);

  const Vector2i renderingSize = components::LayoutSystem().calculateViewportScaledDocumentSize(
      EntityHandle(registry, rootEntity),
      components::LayoutSystem::InvalidSizeBehavior::ReturnDefault);

  bitmap_.allocPixels(
      SkImageInfo::MakeN32(renderingSize.x, renderingSize.y, SkAlphaType::kUnpremul_SkAlphaType));
  SkCanvas canvas(bitmap_);
  rootCanvas_ = &canvas;
  currentCanvas_ = &canvas;

  draw(registry, rootEntity);

  rootCanvas_ = currentCanvas_ = nullptr;
}

std::string RendererSkia::drawIntoAscii(SVGDocument& document) {
  Registry& registry = document.registry();
  const Entity rootEntity = document.rootEntity();

  // TODO: Plumb outWarnings.
  RendererUtils::prepareDocumentForRendering(document, verbose_);

  const Vector2i renderingSize = components::LayoutSystem().calculateViewportScaledDocumentSize(
      EntityHandle(registry, rootEntity),
      components::LayoutSystem::InvalidSizeBehavior::ReturnDefault);

  assert(renderingSize.x <= 64 && renderingSize.y <= 64 &&
         "Rendering size must be less than or equal to 64x64");

  bitmap_.allocPixels(SkImageInfo::Make(renderingSize.x, renderingSize.y, kGray_8_SkColorType,
                                        kOpaque_SkAlphaType));
  SkCanvas canvas(bitmap_);
  rootCanvas_ = &canvas;
  currentCanvas_ = &canvas;

  draw(registry, rootEntity);

  rootCanvas_ = currentCanvas_ = nullptr;

  std::string asciiArt;
  asciiArt.reserve(renderingSize.x * renderingSize.y +
                   renderingSize.y);  // Reserve space including newlines

  static const std::array<char, 10> grayscaleTable = {'.', ',', ':', '-', '=',
                                                      '+', '*', '#', '%', '@'};

  for (int y = 0; y < renderingSize.y; ++y) {
    for (int x = 0; x < renderingSize.x; ++x) {
      const uint8_t pixel = *bitmap_.getAddr8(x, y);
      int index = pixel / static_cast<int>(256 / grayscaleTable.size());
      if (index >= grayscaleTable.size()) {
        index = grayscaleTable.size() - 1;
      }
      asciiArt += grayscaleTable.at(index);
    }

    asciiArt += '\n';
  }

  bitmap_.reset();

  return asciiArt;
}

sk_sp<SkPicture> RendererSkia::drawIntoSkPicture(SVGDocument& document) {
  Registry& registry = document.registry();
  const Entity rootEntity = document.rootEntity();

  // TODO: Plumb outWarnings.
  RendererUtils::prepareDocumentForRendering(document, verbose_);

  const Vector2i renderingSize = components::LayoutSystem().calculateViewportScaledDocumentSize(
      EntityHandle(registry, rootEntity),
      components::LayoutSystem::InvalidSizeBehavior::ReturnDefault);

  SkPictureRecorder recorder;
  rootCanvas_ = recorder.beginRecording(toSkia(Boxd::WithSize(renderingSize)));
  currentCanvas_ = rootCanvas_;

  draw(registry, rootEntity);

  rootCanvas_ = currentCanvas_ = nullptr;

  return recorder.finishRecordingAsPicture();
}

bool RendererSkia::save(const char* filename) {
  assert(bitmap_.colorType() == kRGBA_8888_SkColorType);
  return RendererImageIO::writeRgbaPixelsToPngFile(filename, pixelData(), bitmap_.width(),
                                                   bitmap_.height());
}

std::span<const uint8_t> RendererSkia::pixelData() const {
  return std::span<const uint8_t>(static_cast<const uint8_t*>(bitmap_.getPixels()),
                                  bitmap_.computeByteSize());
}

void RendererSkia::draw(Registry& registry, Entity root) {
  Impl impl(*this, RenderingInstanceView{registry.view<components::RenderingInstanceComponent>()});
  impl.drawUntil(registry, entt::null);
}

}  // namespace donner::svg
