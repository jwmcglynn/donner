#include "donner/svg/renderer/RendererSkia.h"

// Skia
#include "donner/svg/components/ElementTypeComponent.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPathMeasure.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkStream.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkLumaColorFilter.h"
#include "include/pathops/SkPathOps.h"
//
#include "donner/base/xml/components/TreeComponent.h"  // ForAllChildren
#include "donner/svg/SVGMarkerElement.h"
#include "donner/svg/components/IdComponent.h"  // For verbose logging.
#include "donner/svg/components/PathLengthComponent.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/MarkerComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowBranch.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"

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
  // TODO(jwmcglynn): Implement MiterClip and Arcs. For now, fallback to Miter, which is the default
  // linejoin, since the feature is not implemented.
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

/// Implementation class for \ref RendererSkia
class RendererSkia::Impl {
public:
  Impl(RendererSkia& renderer, const RenderingInstanceView& view)
      : renderer_(renderer), view_(view) {}

  void drawUntil(Registry& registry, Entity endEntity) {
    bool foundEndEntity = false;

    while (!view_.done() && !foundEndEntity) {
      // When we find the end we do one more iteration of the loop and then exit.
      foundEndEntity = view_.currentEntity() == endEntity;

      const components::RenderingInstanceComponent& instance = view_.get();
      const Entity entity = view_.currentEntity();
      view_.advance();

      const Transformd entityFromCanvas = layerBaseTransform_ * instance.entityFromWorldTransform;

      if (renderer_.verbose_) {
        std::cout << "Rendering "
                  << registry.get<components::ElementTypeComponent>(instance.dataEntity).type()
                  << " ";

        if (const auto* idComponent =
                registry.try_get<components::IdComponent>(instance.dataEntity)) {
          std::cout << "id=" << idComponent->id() << " ";
        }

        std::cout << instance.dataEntity;
        if (instance.isShadow(registry)) {
          std::cout << " (shadow " << instance.styleHandle(registry).entity() << ")";
        }

        std::cout << " transform=" << entityFromCanvas << "\n";

        std::cout << "\n";
      }

      if (instance.clipRect) {
        renderer_.currentCanvas_->save();
        renderer_.currentCanvas_->clipRect(toSkia(instance.clipRect.value()));
      }

      renderer_.currentCanvas_->setMatrix(toSkia(entityFromCanvas));

      const components::ComputedStyleComponent& styleComponent =
          instance.styleHandle(registry).get<components::ComputedStyleComponent>();
      const auto& properties = styleComponent.properties.value();

      if (instance.isolatedLayer) {
        // Create a new layer if opacity is less than 1.
        if (properties.opacity.getRequired() < 1.0) {
          SkPaint opacityPaint;
          opacityPaint.setAlphaf(NarrowToFloat(properties.opacity.getRequired()));

          // const SkRect layerBounds = toSkia(shapeWorldBounds.value_or(Boxd()));
          renderer_.currentCanvas_->saveLayer(nullptr, &opacityPaint);
        }

        if (instance.resolvedFilter) {
          SkPaint filterPaint;
          filterPaint.setAntiAlias(renderer_.antialias_);
          createFilterPaint(filterPaint, registry, instance.resolvedFilter.value());

          // const SkRect layerBounds = toSkia(shapeWorldBounds.value_or(Boxd()));
          renderer_.currentCanvas_->saveLayer(nullptr, &filterPaint);
        }

        if (instance.clipPath) {
          const components::ResolvedClipPath& ref = instance.clipPath.value();

          Transformd userSpaceFromClipPathContent;
          if (ref.units == ClipPathUnits::ObjectBoundingBox) {
            if (const auto* path =
                    instance.dataHandle(registry).try_get<components::ComputedPathComponent>()) {
              // TODO(jwmcglynn): Extend this to get the element bounds for all child elements by
              // uing ShapeSystem::getShapeWorldBounds()
              const Boxd bounds = path->spline.bounds();
              userSpaceFromClipPathContent =
                  Transformd::Scale(bounds.size()) * Transformd::Translate(bounds.topLeft);
            }
          }

          if (auto* computedTransform =
                  ref.reference.handle.try_get<components::ComputedLocalTransformComponent>()) {
            userSpaceFromClipPathContent =
                userSpaceFromClipPathContent * computedTransform->entityFromParent;
          }

          renderer_.currentCanvas_->save();
          const SkMatrix skUserSpaceFromClipPathContent =
              toSkiaMatrix(userSpaceFromClipPathContent);

          SkPath fullPath;

          // Iterate over children and add any paths to the clip.
          // TODO(jwmcglynn): Move path/clip-rule aggregation and to a Computed component
          // pre-calculation?
          donner::components::ForAllChildren(ref.reference.handle, [&](EntityHandle child) {
            if (const auto* clipPathData = child.try_get<components::ComputedPathComponent>()) {
              SkPath path = toSkia(clipPathData->spline);
              path.transform(skUserSpaceFromClipPathContent);

              if (const auto* computedStyle = child.try_get<components::ComputedStyleComponent>()) {
                const auto& style = computedStyle->properties.value();
                const ClipRule clipRule = style.clipRule.get().value_or(ClipRule::NonZero);

                path.setFillType(clipRule == ClipRule::NonZero ? SkPathFillType::kWinding
                                                               : SkPathFillType::kEvenOdd);
              }

              Op(fullPath, path, kUnion_SkPathOp, &fullPath);
            }
          });

          renderer_.currentCanvas_->clipPath(fullPath, SkClipOp::kIntersect, true);
        }

        if (instance.mask) {
          const components::ResolvedMask& ref = instance.mask.value();

          SkPaint maskFilter;
          // TODO: SRGB colorspace conversion
          // Use Luma color filter for the mask, which converts the mask to alpha.
          maskFilter.setColorFilter(SkLumaColorFilter::Make());

          // Save the current layer with the mask filter
          renderer_.currentCanvas_->saveLayer(nullptr, &maskFilter);

          // Render the mask content
          instantiateMask(ref.reference.handle, instance, instance.dataHandle(registry), ref);

          // Content layer
          // Dst is the mask, Src is the content.
          // kSrcIn multiplies the mask alpha: r = s * da
          SkPaint maskPaint;
          maskPaint.setBlendMode(SkBlendMode::kSrcIn);
          renderer_.currentCanvas_->saveLayer(nullptr, &maskPaint);

          // Restore the matrix after starting the layer
          renderer_.currentCanvas_->setMatrix(toSkia(entityFromCanvas));
        }
      }

      if (instance.visible) {
        if (const auto* path =
                instance.dataHandle(registry).try_get<components::ComputedPathComponent>()) {
          drawPath(
              instance.dataHandle(registry), instance, *path, styleComponent.properties.value(),
              components::LayoutSystem().getViewport(instance.dataHandle(registry)), FontMetrics());
        } else if (const auto* image =
                       instance.dataHandle(registry).try_get<components::LoadedImageComponent>()) {
          drawImage(instance.dataHandle(registry), instance, *image);
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

  void skipUntil(Registry& registry, Entity endEntity) {
    bool foundEndEntity = false;

    while (!view_.done() && !foundEndEntity) {
      // When we find the end we do one more iteration of the loop and then exit.
      foundEndEntity = view_.currentEntity() == endEntity;

      view_.advance();
    }
  }

  void drawRange(Registry& registry, Entity startEntity, Entity endEntity) {
    bool foundStartEntity = false;

    while (!view_.done() && !foundStartEntity) {
      if (view_.currentEntity() == startEntity) {
        break;
      } else {
        view_.advance();
      }
    }

    drawUntil(registry, endEntity);
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

    drawMarkers(dataHandle, instance, path, viewbox, fontMetrics);
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

  Transformd ResolveTransform(
      const components::ComputedLocalTransformComponent* maybeTransformComponent,
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

    const auto* maybeTransformComponent =
        target.try_get<components::ComputedLocalTransformComponent>();

    bool numbersArePercent = false;
    Transformd gradientFromGradientUnits;

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

      gradientFromGradientUnits =
          ResolveTransform(maybeTransformComponent, kUnitPathBounds, FontMetrics());

      // Apply scaling and translation from unit box to path bounds
      const Transformd objectBoundingBoxFromUnitBox =
          Transformd::Scale(pathBounds.size()) * Transformd::Translate(pathBounds.topLeft);

      // Combine the transforms
      gradientFromGradientUnits = gradientFromGradientUnits * objectBoundingBoxFromUnitBox;

      // TODO(jwmcglynn): Can numbersArePercent be represented by the transform instead?
      numbersArePercent = true;
    } else {
      gradientFromGradientUnits = ResolveTransform(maybeTransformComponent, viewbox, FontMetrics());
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
    const SkMatrix skGradientFromGradientUnits = toSkiaMatrix(gradientFromGradientUnits);

    if (const auto* linearGradient =
            target.try_get<components::ComputedLinearGradientComponent>()) {
      const SkPoint points[] = {toSkia(resolveGradientCoords(linearGradient->x1, linearGradient->y1,
                                                             bounds, numbersArePercent)),
                                toSkia(resolveGradientCoords(linearGradient->x2, linearGradient->y2,
                                                             bounds, numbersArePercent))};

      SkPaint paint;
      paint.setAntiAlias(renderer_.antialias_);
      paint.setShader(SkGradientShader::MakeLinear(
          static_cast<const SkPoint*>(points), color.data(), pos.data(), numStops,
          toSkia(computedGradient.spreadMethod), 0, &skGradientFromGradientUnits));
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
        paint.setShader(SkGradientShader::MakeRadial(
            toSkia(center), radius, color.data(), pos.data(), numStops,
            toSkia(computedGradient.spreadMethod), 0, &skGradientFromGradientUnits));
      } else {
        paint.setShader(SkGradientShader::MakeTwoPointConical(
            toSkia(focalCenter), focalRadius, toSkia(center), radius, color.data(), pos.data(),
            numStops, toSkia(computedGradient.spreadMethod), 0, &skGradientFromGradientUnits));
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

  /**
   * Renders the mask contents to the current layer. The caller should call saveLayer before this
   * call.
   *
   * @param dataHandle The handle to the pattern data.
   * @param instance The rendering instance component for the currently rendered entity (same entity
   * as \p target).
   * @param target The target entity to which the pattern is applied.
   * @param ref The reference to the mask.
   */
  void instantiateMask(EntityHandle dataHandle,
                       const components::RenderingInstanceComponent& instance, EntityHandle target,
                       const components::ResolvedMask& ref) {
    if (!ref.subtreeInfo) {
      // Subtree did not instantiate, indicating that recursion was detected.
      return;
    }

    Registry& registry = *dataHandle.registry();

    auto layerBaseRestore = overrideLayerBaseTransform(instance.entityFromWorldTransform);

    if (renderer_.verbose_) {
      std::cout << "Start mask contents\n";
    }

    // Get maskUnits and maskContentUnits
    const components::MaskComponent& maskComponent =
        ref.reference.handle.get<components::MaskComponent>();

    // Get x, y, width, height with default values
    const Lengthd x = maskComponent.x.value_or(Lengthd(-10.0, Lengthd::Unit::Percent));
    const Lengthd y = maskComponent.y.value_or(Lengthd(-10.0, Lengthd::Unit::Percent));
    const Lengthd width = maskComponent.width.value_or(Lengthd(120.0, Lengthd::Unit::Percent));
    const Lengthd height = maskComponent.height.value_or(Lengthd(120.0, Lengthd::Unit::Percent));

    const std::optional<Boxd> shapeWorldBounds =
        components::ShapeSystem().getShapeWorldBounds(target);
    const Boxd shapeLocalBounds =
        instance.entityFromWorldTransform.inverse().transformBox(shapeWorldBounds.value_or(Boxd()));

    // Compute the reference bounds based on maskUnits
    Boxd maskUnitsBounds;

    if (maskComponent.maskUnits == MaskUnits::ObjectBoundingBox) {
      maskUnitsBounds = shapeLocalBounds;
    } else {
      // maskUnits == UserSpaceOnUse
      // Use the viewport as bounds
      maskUnitsBounds = components::LayoutSystem().getViewport(instance.dataHandle(registry));
    }

    if (!maskComponent.useAutoBounds()) {
      // Resolve x, y, width, height
      const double x_px = x.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::X);
      const double y_px = y.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::Y);
      const double width_px = width.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::X);
      const double height_px = height.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::Y);

      // Create maskBounds
      const Boxd maskBounds = Boxd::FromXYWH(x_px, y_px, width_px, height_px);

      // Apply clipRect with maskBounds
      renderer_.currentCanvas_->clipRect(toSkia(maskBounds), SkClipOp::kIntersect, true);
    }

    // Adjust layerBaseTransform_ according to maskContentUnits
    if (maskComponent.maskContentUnits == MaskContentUnits::ObjectBoundingBox) {
      // Compute the transform from mask content coordinate system to user space
      const Transformd userSpaceFromMaskContent = Transformd::Scale(shapeLocalBounds.size()) *
                                                  Transformd::Translate(shapeLocalBounds.topLeft);

      // Update the layer base transform
      layerBaseTransform_ = userSpaceFromMaskContent * layerBaseTransform_;
    } else {
      // maskContentUnits == UserSpaceOnUse
      // No adjustment needed
    }

    // Render the mask content
    assert(ref.subtreeInfo);
    if (!shapeLocalBounds.isEmpty()) {
      drawUntil(registry, ref.subtreeInfo->lastRenderedEntity);
    } else {
      // Skip child elements.
      skipUntil(registry, ref.subtreeInfo->lastRenderedEntity);
    }

    if (renderer_.verbose_) {
      std::cout << "End mask contents\n";
    }
  }

  /**
   * Instantiates a pattern paint. See \ref PatternUnits, \ref PatternContentUnits for details on
   * their behavior.
   *
   * @param branchType Determined by whether this is the fill or stroke.
   * @param dataHandle The handle to the pattern data.
   * @param target The target entity to which the pattern is applied.
   * @param computedPattern The resolved pattern component.
   * @param ref The reference to the pattern.
   * @param pathBounds The bounds of the path to which the pattern is applied.
   * @param viewbox The viewbox of the the target entity.
   * @param currentColor Current context color inherited from the parent.
   * @param opacity Current opacity inherited from the parent.
   * @return SkPaint instance containing the pattern.
   */
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

    const auto* maybeTransformComponent =
        target.try_get<components::ComputedLocalTransformComponent>();

    Transformd patternContentFromPatternTile;
    Transformd patternTileFromTarget;
    Boxd rect = computedPattern.tileRect;

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
        // Skip rendering the pattern contents
        assert(ref.subtreeInfo);
        skipUntil(registry, ref.subtreeInfo->lastRenderedEntity);

        return createFallbackPaint(ref, currentColor, opacity);
      }

      const Vector2d rectSize = rect.size();

      rect.topLeft = rect.topLeft * pathBounds.size() + pathBounds.topLeft;
      rect.bottomRight = rectSize * pathBounds.size() + rect.topLeft;
    }

    if (computedPattern.viewbox) {
      patternContentFromPatternTile = computedPattern.preserveAspectRatio.computeTransform(
          rect.toOrigin(), computedPattern.viewbox);
    } else if (patternContentObjectBoundingBox) {
      patternContentFromPatternTile = Transformd::Scale(pathBounds.size());
    }

    patternTileFromTarget = Transformd::Translate(rect.topLeft) *
                            ResolveTransform(maybeTransformComponent, viewbox, FontMetrics());

    const SkRect skTileRect = toSkia(rect.toOrigin());

    SkCanvas* const savedCanvas = renderer_.currentCanvas_;
    const Transformd savedLayerBaseTransform = layerBaseTransform_;

    if (renderer_.verbose_) {
      std::cout << "Start pattern contents\n";
    }

    SkPictureRecorder recorder;
    renderer_.currentCanvas_ = recorder.beginRecording(skTileRect);
    layerBaseTransform_ = patternContentFromPatternTile;

    // Render the subtree into the offscreen SkPictureRecorder.
    assert(ref.subtreeInfo);
    drawUntil(registry, ref.subtreeInfo->lastRenderedEntity);

    if (renderer_.verbose_) {
      std::cout << "End pattern contents\n";
    }

    renderer_.currentCanvas_ = savedCanvas;
    layerBaseTransform_ = savedLayerBaseTransform;

    // Transform to apply to the pattern contents.
    const SkMatrix skPatternContentFromPatternTile = toSkiaMatrix(patternTileFromTarget);

    SkPaint skPaint;
    skPaint.setAntiAlias(renderer_.antialias_);
    skPaint.setShader(recorder.finishRecordingAsPicture()->makeShader(
        SkTileMode::kRepeat, SkTileMode::kRepeat, SkFilterMode::kLinear,
        &skPatternContentFromPatternTile, &skTileRect));
    return skPaint;
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

    UTILS_UNREACHABLE();  // The computed tree should invalidate any references that don't point
                          // to a valid point server, see IsValidPaintServer.
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

    if (renderer_.verbose_) {
      std::cout << "Drawing path bounds " << path.spline.bounds() << "\n";
    }

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
      const int numRepeats = (dashes.size() & 1) ? 2 : 1;

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

  void drawImage(EntityHandle dataHandle, const components::RenderingInstanceComponent& instance,
                 const components::LoadedImageComponent& image) {
    if (!image.image) {
      return;
    }

    SkBitmap bitmap;
    bitmap.allocPixels(SkImageInfo::MakeN32Premul(image.image->width, image.image->height));
    memcpy(bitmap.getPixels(), image.image->data.data(), image.image->data.size());

    bitmap.setImmutable();
    sk_sp<SkImage> skImage = SkImages::RasterFromBitmap(bitmap);

    SkPaint paint;
    paint.setAntiAlias(renderer_.antialias_);
    paint.setStroke(true);
    paint.setColor(toSkia(css::RGBA(255, 255, 255, 255)));

    const auto& sizedElement = dataHandle.get<components::ComputedSizedElementComponent>();

    const PreserveAspectRatio preserveAspectRatio =
        dataHandle.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;

    const Boxd intrinsicSize = Boxd::WithSize(Vector2d(image.image->width, image.image->height));

    const Transformd imageFromLocal =
        preserveAspectRatio.computeTransform(sizedElement.bounds, intrinsicSize);

    renderer_.currentCanvas_->save();
    renderer_.currentCanvas_->clipRect(toSkia(sizedElement.bounds));
    renderer_.currentCanvas_->concat(toSkia(imageFromLocal));
    renderer_.currentCanvas_->drawImage(skImage, 0, 0, SkSamplingOptions(SkFilterMode::kLinear),
                                        &paint);
    renderer_.currentCanvas_->restore();
  }

  void createFilterChain(SkPaint& filterPaint, const std::vector<FilterEffect>& effectList) {
    for (const FilterEffect& effect : effectList) {
      std::visit(entt::overloaded{//
                                  [&](const FilterEffect::None&) {},
                                  [&](const FilterEffect::Blur& blur) {
                                    // TODO(jwmcglynn): Convert these Length units
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
      if (const auto* computedFilter =
              registry.try_get<components::ComputedFilterComponent>(*reference)) {
        createFilterChain(filterPaint, computedFilter->effectChain);
      }
    }
  }

  void drawMarkers(EntityHandle dataHandle, const components::RenderingInstanceComponent& instance,
                   const components::ComputedPathComponent& path, const Boxd& viewbox,
                   const FontMetrics& fontMetrics) {
    const auto& commands = path.spline.commands();

    if (commands.size() < 2) {
      return;
    }

    bool hasMarkerStart = instance.markerStart.has_value();
    bool hasMarkerMid = instance.markerMid.has_value();
    bool hasMarkerEnd = instance.markerEnd.has_value();

    if (hasMarkerStart || hasMarkerMid || hasMarkerEnd) {
      const RenderingInstanceView::SavedState viewSnapshot = view_.save();

      const std::vector<PathSpline::Vertex> vertices = path.spline.vertices();

      for (size_t i = 0; i < vertices.size(); ++i) {
        const PathSpline::Vertex& vertex = vertices[i];

        if (i == 0) {
          if (hasMarkerStart) {
            drawMarker(dataHandle, instance, instance.markerStart.value(), vertex.point,
                       vertex.orientation, /*isMarkerStart*/ true, viewbox, fontMetrics);
          }
        } else if (i == vertices.size() - 1) {
          if (hasMarkerEnd) {
            drawMarker(dataHandle, instance, instance.markerEnd.value(), vertex.point,
                       vertex.orientation, /*isMarkerStart*/ false, viewbox, fontMetrics);
          }
        } else if (hasMarkerMid) {
          drawMarker(dataHandle, instance, instance.markerMid.value(), vertex.point,
                     vertex.orientation, /*isMarkerStart*/ false, viewbox, fontMetrics);
        }

        view_.restore(viewSnapshot);
      }

      // Skipping the rendered marker definitions to avoid duplication
      if (hasMarkerEnd) {
        skipUntil(*dataHandle.registry(),
                  instance.markerEnd.value().subtreeInfo->lastRenderedEntity);
      } else if (hasMarkerMid) {
        skipUntil(*dataHandle.registry(),
                  instance.markerMid.value().subtreeInfo->lastRenderedEntity);
      } else if (hasMarkerStart) {
        skipUntil(*dataHandle.registry(),
                  instance.markerStart.value().subtreeInfo->lastRenderedEntity);
      }
    }
  }

  void drawMarker(EntityHandle dataHandle, const components::RenderingInstanceComponent& instance,
                  const components::ResolvedMarker& marker, const Vector2d& vertexPosition,
                  const Vector2d& direction, bool isMarkerStart, const Boxd& viewbox,
                  const FontMetrics& fontMetrics) {
    Registry& registry = *dataHandle.registry();

    const EntityHandle markerHandle = marker.reference.handle;

    if (!markerHandle.valid()) {
      return;
    }

    // Get the marker component
    const auto& markerComponent = markerHandle.get<components::MarkerComponent>();

    if (markerComponent.markerWidth <= 0.0 || markerComponent.markerHeight <= 0.0) {
      return;
    }

    const Boxd markerSize =
        Boxd::FromXYWH(0, 0, markerComponent.markerWidth, markerComponent.markerHeight);

    // Get the marker's viewBox and preserveAspectRatio
    components::LayoutSystem layoutSystem;

    const std::optional<Boxd> markerViewBox =
        layoutSystem.overridesViewport(markerHandle)
            ? std::optional<Boxd>(layoutSystem.getViewport(markerHandle))
            : std::nullopt;
    const PreserveAspectRatio preserveAspectRatio =
        markerHandle.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;

    // Compute the rotation angle according to the orient attribute
    const double angleRadians =
        markerComponent.orient.computeAngleRadians(direction, isMarkerStart);

    // Compute scale according to markerUnits
    double markerScale = 1.0;
    if (markerComponent.markerUnits == MarkerUnits::StrokeWidth) {
      // Scale by stroke width
      const components::ComputedStyleComponent& styleComponent =
          instance.styleHandle(registry).get<components::ComputedStyleComponent>();
      const double strokeWidth = styleComponent.properties->strokeWidth.getRequired().value;
      markerScale = strokeWidth;
    }

    const Transformd markerUnitsFromViewbox =
        preserveAspectRatio.computeTransform(markerSize, markerViewBox);

    const Transformd markerOffsetFromVertex =
        Transformd::Translate(-markerComponent.refX * markerUnitsFromViewbox.data[0],
                              -markerComponent.refY * markerUnitsFromViewbox.data[3]);

    const Transformd vertexFromEntity = Transformd::Scale(markerScale) *
                                        Transformd::Rotate(angleRadians) *
                                        Transformd::Translate(vertexPosition);

    const Transformd vertexFromWorld =
        vertexFromEntity * layerBaseTransform_ * instance.entityFromWorldTransform;

    const Transformd markerUserSpaceFromWorld =
        Transformd::Scale(markerUnitsFromViewbox.data[0], markerUnitsFromViewbox.data[3]) *
        markerOffsetFromVertex * vertexFromWorld;

    // Now, render the marker's content with the computed transform
    auto layerBaseRestore = overrideLayerBaseTransform(markerUserSpaceFromWorld);

    renderer_.currentCanvas_->save();
    renderer_.currentCanvas_->resetMatrix();

    const auto& computedStyle = markerHandle.get<components::ComputedStyleComponent>();
    const Overflow overflow = computedStyle.properties->overflow.getRequired();
    if (overflow != Overflow::Visible && overflow != Overflow::Auto) {
      renderer_.currentCanvas_->clipRect(
          toSkia(markerUserSpaceFromWorld.transformBox(markerViewBox.value_or(markerSize))));
    }

    // Render the marker's content
    if (marker.subtreeInfo) {
      // Draw the marker's subtree
      drawRange(registry, marker.subtreeInfo->firstRenderedEntity,
                marker.subtreeInfo->lastRenderedEntity);
    }

    renderer_.currentCanvas_->restore();
  }

private:
  struct LayerBaseRestore {
    LayerBaseRestore(RendererSkia::Impl& impl, const Transformd& savedTransform)
        : impl_(impl), savedTransform_(savedTransform) {}

    ~LayerBaseRestore() { impl_.layerBaseTransform_ = savedTransform_; }

  private:
    RendererSkia::Impl& impl_;
    Transformd savedTransform_;
  };

  LayerBaseRestore overrideLayerBaseTransform(const Transformd& newLayerBaseTransform) {
    const Transformd savedTransform = layerBaseTransform_;
    layerBaseTransform_ = newLayerBaseTransform;
    return LayerBaseRestore(*this, savedTransform);
  }

  RendererSkia& renderer_;
  RenderingInstanceView view_;

  std::vector<components::SubtreeInfo> subtreeMarkers_;
  Transformd layerBaseTransform_ = Transformd();
};

RendererSkia::RendererSkia(bool verbose) : verbose_(verbose) {}

RendererSkia::~RendererSkia() {}

RendererSkia::RendererSkia(RendererSkia&&) noexcept = default;
RendererSkia& RendererSkia::operator=(RendererSkia&&) noexcept = default;

void RendererSkia::draw(SVGDocument& document) {
  // TODO(jwmcglynn): Plumb outWarnings.
  std::vector<parser::ParseError> warnings;
  RendererUtils::prepareDocumentForRendering(document, verbose_, verbose_ ? &warnings : nullptr);

  if (!warnings.empty()) {
    for (const parser::ParseError& warning : warnings) {
      std::cerr << warning << '\n';
    }
  }

  const Vector2i renderingSize = document.canvasSize();

  bitmap_.allocPixels(
      SkImageInfo::MakeN32(renderingSize.x, renderingSize.y, SkAlphaType::kUnpremul_SkAlphaType));
  SkCanvas canvas(bitmap_);
  rootCanvas_ = &canvas;
  currentCanvas_ = &canvas;

  draw(document.registry());

  rootCanvas_ = currentCanvas_ = nullptr;
}

std::string RendererSkia::drawIntoAscii(SVGDocument& document) {
  // TODO(jwmcglynn): Plumb outWarnings.
  RendererUtils::prepareDocumentForRendering(document, verbose_);

  const Vector2i renderingSize = document.canvasSize();

  assert(renderingSize.x <= 64 && renderingSize.y <= 64 &&
         "Rendering size must be less than or equal to 64x64");

  bitmap_.allocPixels(SkImageInfo::Make(renderingSize.x, renderingSize.y, kGray_8_SkColorType,
                                        kOpaque_SkAlphaType));
  SkCanvas canvas(bitmap_);
  rootCanvas_ = &canvas;
  currentCanvas_ = &canvas;

  draw(document.registry());

  rootCanvas_ = currentCanvas_ = nullptr;

  std::string asciiArt;
  asciiArt.reserve(renderingSize.x * renderingSize.y +
                   renderingSize.y);  // Reserve space including newlines

  static const std::array<char, 10> grayscaleTable = {'.', ',', ':', '-', '=',
                                                      '+', '*', '#', '%', '@'};

  for (int y = 0; y < renderingSize.y; ++y) {
    for (int x = 0; x < renderingSize.x; ++x) {
      const uint8_t pixel = *bitmap_.getAddr8(x, y);
      size_t index = pixel / static_cast<size_t>(256 / grayscaleTable.size());
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

  // TODO(jwmcglynn): Plumb outWarnings.
  RendererUtils::prepareDocumentForRendering(document, verbose_);

  const Vector2i renderingSize = components::LayoutSystem().calculateCanvasScaledDocumentSize(
      registry, components::LayoutSystem::InvalidSizeBehavior::ReturnDefault);

  SkPictureRecorder recorder;
  rootCanvas_ = recorder.beginRecording(toSkia(Boxd::WithSize(renderingSize)));
  currentCanvas_ = rootCanvas_;

  draw(registry);

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

void RendererSkia::draw(Registry& registry) {
  Impl impl(*this, RenderingInstanceView{registry});
  impl.drawUntil(registry, entt::null);
}

}  // namespace donner::svg
