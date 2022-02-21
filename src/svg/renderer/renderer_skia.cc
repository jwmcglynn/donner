#include "src/svg/renderer/renderer_skia.h"

// Skia
#include "include/core/SkPath.h"
#include "include/core/SkPathMeasure.h"
#include "include/core/SkStream.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradientShader.h"
//
#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/gradient_component.h"
#include "src/svg/components/linear_gradient_component.h"
#include "src/svg/components/path_component.h"
#include "src/svg/components/path_length_component.h"
#include "src/svg/components/radial_gradient_component.h"
#include "src/svg/components/rect_component.h"
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/components/shadow_entity_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/transform_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/components/viewbox_component.h"
#include "src/svg/renderer/renderer_utils.h"

namespace donner::svg {

namespace {

// The maximum size supported for a rendered image. Unused in release builds.
[[maybe_unused]] static constexpr int kMaxDimension = 8192;

SkPoint toSkia(Vector2d value) {
  return SkPoint::Make(NarrowToFloat(value.x), NarrowToFloat(value.y));
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
  return SkRect::MakeLTRB(box.top_left.x, box.top_left.y, box.bottom_right.x, box.bottom_right.y);
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
        path.moveTo(pt.x, pt.y);
        break;
      }
      case PathSpline::CommandType::CurveTo: {
        auto c0 = points[command.pointIndex];
        auto c1 = points[command.pointIndex + 1];
        auto end = points[command.pointIndex + 2];
        path.cubicTo(c0.x, c0.y, c1.x, c1.y, end.x, end.y);
        break;
      }
      case PathSpline::CommandType::LineTo: {
        auto pt = points[command.pointIndex];
        path.lineTo(pt.x, pt.y);
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
  Impl(RendererSkia& renderer) : renderer_(renderer) {}

  void drawPath(EntityHandle dataHandle, const ComputedPathComponent& path,
                const PropertyRegistry& style, const Boxd& viewbox,
                const FontMetrics& fontMetrics) {
    if (auto fill = style.fill.get()) {
      drawPathFill(dataHandle, path, fill.value(), style, viewbox);
    }

    if (auto stroke = style.stroke.get()) {
      drawPathStroke(dataHandle, path, stroke.value(), style, viewbox, fontMetrics);
    }
  }

  std::optional<SkPaint> createFallbackPaint(const PaintServer::ElementReference& ref,
                                             css::RGBA currentColor, float opacity) {
    if (ref.fallback) {
      SkPaint paint;
      paint.setAntiAlias(true);
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
                                 const Transformd& transform, bool numbersArePercent) {
    return transform.transformPosition(Vector2d(
        toPercent(x, numbersArePercent).toPixels(viewbox, FontMetrics(), Lengthd::Extent::X),
        toPercent(y, numbersArePercent).toPixels(viewbox, FontMetrics(), Lengthd::Extent::Y)));
  }

  static bool CircleContainsPoint(Vector2d center, double radius, Vector2d point) {
    return (point - center).lengthSquared() <= radius * radius;
  }

  Transformd ResolveTransform(const ComputedTransformComponent* maybeTransformComponent,
                              const Boxd& viewbox, const FontMetrics& fontMetrics) {
    if (maybeTransformComponent) {
      return maybeTransformComponent->rawCssTransform.compute(viewbox, fontMetrics);
    } else {
      return Transformd();
    }
  }

  std::optional<SkPaint> instantiatePaintReference(EntityHandle dataHandle,
                                                   const PaintServer::ElementReference& ref,
                                                   const Boxd& pathBounds, const Boxd& viewbox,
                                                   css::RGBA currentColor, float opacity) {
    if (auto resolvedRef = ref.reference.resolve(*dataHandle.registry())) {
      const EntityHandle target = resolvedRef->handle;

      if (const auto [gradient, gradientStops] =
              target.try_get<GradientComponent, ComputedGradientComponent>();
          gradient && gradientStops) {
        // Apply gradientUnits and gradientTransform.
        const bool objectBoundingBox = gradient->gradientUnits == GradientUnits::ObjectBoundingBox;

        const ComputedTransformComponent* maybeTransformComponent =
            TransformComponent::ComputedTransform(target, FontMetrics());

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

          transform = ResolveTransform(maybeTransformComponent, pathBounds, FontMetrics());

          // Note that this applies *before* transform.
          transform *= Transformd::Translate(pathBounds.top_left);

          // TODO: Can numbersArePercent be represented by the transform instead?
          numbersArePercent = true;
        } else {
          transform = ResolveTransform(maybeTransformComponent, viewbox, FontMetrics());
        }

        const Boxd& bounds = objectBoundingBox ? pathBounds : viewbox;

        std::vector<SkScalar> pos;
        std::vector<SkColor> color;
        for (const GradientStop& stop : gradientStops->stops) {
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
          paint.setColor(color[0]);
          return paint;
        }

        if (const auto* linearGradient = target.try_get<LinearGradientComponent>()) {
          const SkPoint points[] = {
              toSkia(resolveGradientCoords(linearGradient->x1, linearGradient->y1, bounds,
                                           transform, numbersArePercent)),
              toSkia(resolveGradientCoords(linearGradient->x2, linearGradient->y2, bounds,
                                           transform, numbersArePercent))};

          SkPaint paint;
          paint.setAntiAlias(true);
          paint.setShader(SkGradientShader::MakeLinear(points, color.data(), pos.data(), numStops,
                                                       toSkia(gradient->spreadMethod)));
          return paint;
        } else {
          const auto& radialGradient = target.get<RadialGradientComponent>();
          const Vector2d center = resolveGradientCoords(radialGradient.cx, radialGradient.cy,
                                                        bounds, transform, numbersArePercent);
          const SkScalar radius = resolveGradientCoord(radialGradient.r, bounds, numbersArePercent);

          Vector2d focalCenter = resolveGradientCoords(
              radialGradient.fx.value_or(radialGradient.cx),
              radialGradient.fy.value_or(radialGradient.cy), bounds, transform, numbersArePercent);
          const SkScalar focalRadius =
              resolveGradientCoord(radialGradient.fr, bounds, numbersArePercent);

          if (NearZero(radius)) {
            SkPaint paint;
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
          paint.setAntiAlias(true);
          if (NearZero(focalRadius) && focalCenter == center) {
            paint.setShader(SkGradientShader::MakeRadial(toSkia(center), radius, color.data(),
                                                         pos.data(), numStops,
                                                         toSkia(gradient->spreadMethod)));
          } else {
            paint.setShader(SkGradientShader::MakeTwoPointConical(
                toSkia(focalCenter), focalRadius, toSkia(center), radius, color.data(), pos.data(),
                numStops, toSkia(gradient->spreadMethod)));
          }
          return paint;
        }
      } else {
        // TODO: <pattern> paint types.
      }
    }

    return createFallbackPaint(ref, currentColor, opacity);
  }

  void drawPathFillWithSkPaint(const ComputedPathComponent& path, SkPaint& skPaint,
                               const PropertyRegistry& style) {
    SkPath skPath = toSkia(path.spline);
    if (style.fillRule.get() == FillRule::EvenOdd) {
      skPath.setFillType(SkPathFillType::kEvenOdd);
    }

    skPaint.setAntiAlias(true);
    skPaint.setStyle(SkPaint::Style::kFill_Style);
    renderer_.canvas_->drawPath(skPath, skPaint);
  }

  void drawPathFill(EntityHandle dataHandle, const ComputedPathComponent& path,
                    const PaintServer& paint, const PropertyRegistry& style, const Boxd& viewbox) {
    const float fillOpacity = NarrowToFloat(style.fillOpacity.get().value());

    if (paint.is<PaintServer::Solid>()) {
      const PaintServer::Solid& solid = paint.get<PaintServer::Solid>();

      SkPaint skPaint;
      skPaint.setColor(toSkia(solid.color.resolve(style.color.getRequired().rgba(), fillOpacity)));

      drawPathFillWithSkPaint(path, skPaint, style);
    } else if (paint.is<PaintServer::ElementReference>()) {
      const PaintServer::ElementReference& ref = paint.get<PaintServer::ElementReference>();

      std::optional<SkPaint> skPaint =
          instantiatePaintReference(dataHandle, ref, path.spline.bounds(), viewbox,
                                    style.color.getRequired().rgba(), fillOpacity);
      if (skPaint) {
        drawPathFillWithSkPaint(path, skPaint.value(), style);
      }

    } else if (paint.is<PaintServer::None>()) {
      // Do nothing.

    } else {
      // TODO: Other paint types.
    }
  }

  struct StrokeConfig {
    double strokeWidth;
    double miterLimit;
  };

  void drawPathStrokeWithSkPaint(EntityHandle dataHandle, const ComputedPathComponent& path,
                                 const StrokeConfig& config, SkPaint& skPaint,
                                 const PropertyRegistry& style, const Boxd& viewbox,
                                 const FontMetrics& fontMetrics) {
    const SkPath skPath = toSkia(path.spline);

    if (style.strokeDasharray.hasValue()) {
      double dashUnitsScale = 1.0;
      if (const auto* pathLength = dataHandle.try_get<PathLengthComponent>();
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
          skiaDashes.push_back(dash.toPixels(viewbox, fontMetrics) * dashUnitsScale);
        }
      }

      skPaint.setPathEffect(SkDashPathEffect::Make(
          skiaDashes.data(), skiaDashes.size(),
          style.strokeDashoffset.get().value().toPixels(viewbox, fontMetrics) * dashUnitsScale));
    }

    skPaint.setAntiAlias(true);
    skPaint.setStyle(SkPaint::Style::kStroke_Style);

    skPaint.setStrokeWidth(config.strokeWidth);
    skPaint.setStrokeCap(toSkia(style.strokeLinecap.get().value()));
    skPaint.setStrokeJoin(toSkia(style.strokeLinejoin.get().value()));
    skPaint.setStrokeMiter(config.miterLimit);

    renderer_.canvas_->drawPath(skPath, skPaint);
  }

  void drawPathStroke(EntityHandle dataHandle, const ComputedPathComponent& path,
                      const PaintServer& paint, const PropertyRegistry& style, const Boxd& viewbox,
                      const FontMetrics& fontMetrics) {
    const StrokeConfig config = {
        .strokeWidth = style.strokeWidth.get().value().toPixels(viewbox, fontMetrics),
        .miterLimit = style.strokeMiterlimit.get().value()};
    const double strokeOpacity = style.strokeOpacity.get().value();

    if (config.strokeWidth <= 0.0) {
      return;
    }

    if (paint.is<PaintServer::Solid>()) {
      const PaintServer::Solid& solid = paint.get<PaintServer::Solid>();

      SkPaint skPaint;
      skPaint.setColor(
          toSkia(solid.color.resolve(style.color.getRequired().rgba(), strokeOpacity)));

      drawPathStrokeWithSkPaint(dataHandle, path, config, skPaint, style, viewbox, fontMetrics);
    } else if (paint.is<PaintServer::ElementReference>()) {
      const PaintServer::ElementReference& ref = paint.get<PaintServer::ElementReference>();

      std::optional<SkPaint> skPaint = instantiatePaintReference(
          dataHandle, ref, path.spline.strokeMiterBounds(config.strokeWidth, config.miterLimit),
          viewbox, style.color.getRequired().rgba(), NarrowToFloat((strokeOpacity)));
      if (skPaint) {
        drawPathStrokeWithSkPaint(dataHandle, path, config, skPaint.value(), style, viewbox,
                                  fontMetrics);
      }

    } else if (paint.is<PaintServer::None>()) {
      // Do nothing.
    } else {
      // TODO: Other paint types.
    }
  }

private:
  RendererSkia& renderer_;
};

RendererSkia::RendererSkia(int defaultWidth, int defaultHeight, bool verbose)
    : defaultWidth_(defaultWidth), defaultHeight_(defaultHeight), verbose_(verbose) {}

RendererSkia::~RendererSkia() {}

void RendererSkia::draw(SVGDocument& document) {
  Registry& registry = document.registry();
  const Entity rootEntity = document.rootEntity();

  RendererUtils::prepareDocumentForRendering(document, Vector2d(defaultWidth_, defaultHeight_));

  auto& computedSizeComponent = registry.get<ComputedSizedElementComponent>(rootEntity);
  Vector2d renderingSize = computedSizeComponent.bounds.size();

  if (overrideSize_) {
    renderingSize = Vector2d(defaultWidth_, defaultHeight_);

    const Vector2d origin = computedSizeComponent.bounds.top_left;
    computedSizeComponent.bounds = Boxd(origin, origin + renderingSize);
  } else if (renderingSize.x < 1 || renderingSize.y < 1 || renderingSize.x > kMaxDimension ||
             renderingSize.y > kMaxDimension) {
    // Invalid size, override so that we don't run out of memory.
    renderingSize = Vector2d(Clamp(renderingSize.x, 1.0, static_cast<double>(kMaxDimension)),
                             Clamp(renderingSize.y, 1.0, static_cast<double>(kMaxDimension)));

    const Vector2d origin = computedSizeComponent.bounds.top_left;
    computedSizeComponent.bounds = Boxd(origin, origin + renderingSize);
  }

  // TODO: How should we convert float to integers? Should it be rounded?
  const int width = static_cast<int>(renderingSize.x);
  const int height = static_cast<int>(renderingSize.y);

  bitmap_.allocPixels(SkImageInfo::MakeN32(width, height, SkAlphaType::kUnpremul_SkAlphaType));
  canvas_ = std::make_unique<SkCanvas>(bitmap_);

  draw(registry, rootEntity);
}

bool RendererSkia::save(const char* filename) {
  return RendererUtils::writeRgbaPixelsToPngFile(filename, pixelData(), bitmap_.width(),
                                                 bitmap_.height());
}

std::span<const uint8_t> RendererSkia::pixelData() const {
  return std::span<const uint8_t>(static_cast<const uint8_t*>(bitmap_.getPixels()),
                                  bitmap_.computeByteSize());
}

void RendererSkia::draw(Registry& registry, Entity root) {
  Impl impl(*this);

  std::function<void(Transformd, Entity)> drawEntity = [&](Transformd transform,
                                                           Entity treeEntity) {
    const auto* shadowComponent = registry.try_get<ShadowEntityComponent>(treeEntity);
    const Entity styleEntity = treeEntity;
    const Entity dataEntity = shadowComponent ? shadowComponent->lightEntity : treeEntity;
    bool shouldRestore = false;

    if (const auto* behavior = registry.try_get<RenderingBehaviorComponent>(dataEntity)) {
      if (behavior->behavior == RenderingBehavior::Nonrenderable) {
        if (verbose_) {
          std::cout << "Skipping nonrenderable entity " << dataEntity << std::endl;
        }
        return;
      }
    }

    if (verbose_) {
      std::cout << "Rendering " << TypeToString(registry.get<TreeComponent>(treeEntity).type())
                << " " << treeEntity << (shadowComponent ? " (shadow)" : "") << std::endl;
    }

    if (const auto* sizedElement = registry.try_get<ComputedSizedElementComponent>(dataEntity)) {
      const EntityHandle handle(registry, dataEntity);
      transform = sizedElement->computeTransform(handle) * transform;

      if (auto clipRect = sizedElement->clipRect(handle)) {
        canvas_->save();
        shouldRestore = true;

        canvas_->clipRect(toSkia(clipRect.value()));
      }
    }

    if (const auto* tc = registry.try_get<ComputedTransformComponent>(dataEntity)) {
      transform = tc->transform * transform;
    }

    canvas_->setMatrix(toSkia(transform));

    const ComputedStyleComponent& styleComponent =
        registry.get<ComputedStyleComponent>(styleEntity);

    if (const auto* path = registry.try_get<ComputedPathComponent>(dataEntity)) {
      impl.drawPath(EntityHandle(registry, dataEntity), *path, styleComponent.properties(),
                    styleComponent.viewbox(), FontMetrics());
    }

    const TreeComponent& tree = registry.get<TreeComponent>(treeEntity);
    for (auto cur = tree.firstChild(); cur != entt::null;
         cur = registry.get<TreeComponent>(cur).nextSibling()) {
      drawEntity(transform, cur);
    }

    if (shouldRestore) {
      canvas_->restore();
    }
  };

  drawEntity(Transformd(), root);
}

}  // namespace donner::svg
