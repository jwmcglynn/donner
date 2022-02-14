#include "src/svg/renderer/renderer_skia.h"

#include "include/core/SkPath.h"
#include "include/core/SkPathMeasure.h"
#include "include/core/SkStream.h"
#include "include/effects/SkDashPathEffect.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/path_component.h"
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

SkColor toSkia(const css::Color color) {
  // TODO: We need to resolve currentColor before getting here.
  return SkColorSetARGB(color.rgba().a, color.rgba().r, color.rgba().g, color.rgba().b);
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
        auto pt = points[command.point_index];
        path.moveTo(pt.x, pt.y);
        break;
      }
      case PathSpline::CommandType::CurveTo: {
        auto c0 = points[command.point_index];
        auto c1 = points[command.point_index + 1];
        auto end = points[command.point_index + 2];
        path.cubicTo(c0.x, c0.y, c1.x, c1.y, end.x, end.y);
        break;
      }
      case PathSpline::CommandType::LineTo: {
        auto pt = points[command.point_index];
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

}  // namespace

RendererSkia::RendererSkia(int defaultWidth, int defaultHeight, bool verbose)
    : defaultWidth_(defaultWidth), defaultHeight_(defaultHeight), verbose_(verbose) {}

RendererSkia::~RendererSkia() {}

void RendererSkia::draw(SVGDocument& document) {
  Registry& registry = document.registry();
  const Entity rootEntity = document.rootEntity();

  const Vector2d calculatedSize =
      registry.get_or_emplace<SizedElementComponent>(rootEntity)
          .calculatedSize(registry, rootEntity, Vector2d(defaultWidth_, defaultHeight_));

  // TODO: How should we convert float to integers? Should it be rounded?
  const int width = static_cast<int>(calculatedSize.x);
  const int height = static_cast<int>(calculatedSize.y);
  // TODO: This shouldn't crash if the number comes from within the SVG itself.
  assert(width > 0 && width < kMaxDimension);
  assert(height > 0 && height < kMaxDimension);

  bitmap_.allocPixels(SkImageInfo::MakeN32Premul(width, height));
  canvas_ = std::make_unique<SkCanvas>(bitmap_);

  RendererUtils::prepareDocumentForRendering(document, Vector2d(width, height));
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
  std::function<void(Transformd, Entity)> drawEntity = [&](Transformd transform,
                                                           Entity treeEntity) {
    const auto* shadowComponent = registry.try_get<ShadowEntityComponent>(treeEntity);
    const Entity styleEntity = treeEntity;
    const Entity dataEntity = shadowComponent ? shadowComponent->lightEntity : treeEntity;

    if (const auto* behavior = registry.try_get<RenderingBehaviorComponent>(dataEntity)) {
      if (behavior->nonrenderable) {
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

    if (const auto* tc = registry.try_get<ViewboxTransformComponent>(dataEntity)) {
      transform = tc->transform * transform;
    }

    if (const auto* tc = registry.try_get<ComputedTransformComponent>(dataEntity)) {
      transform = tc->transform * transform;
    }

    canvas_->setMatrix(toSkia(transform));

    const ComputedStyleComponent& styleComponent =
        registry.get<ComputedStyleComponent>(styleEntity);

    if (const auto* path = registry.try_get<ComputedPathComponent>(dataEntity)) {
      if (path->spline) {
        const PropertyRegistry& style = styleComponent.properties();

        if (auto fill = style.fill.get()) {
          if (fill.value().is<PaintServer::Solid>()) {
            const PaintServer::Solid& solid = fill.value().get<PaintServer::Solid>();

            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(toSkia(solid.color.withOpacity(style.fillOpacity.get().value())));
            paint.setStyle(SkPaint::Style::kFill_Style);

            SkPath skiaPath = toSkia(*path->spline);
            if (style.fillRule.get() == FillRule::EvenOdd) {
              skiaPath.setFillType(SkPathFillType::kEvenOdd);
            }
            canvas_->drawPath(skiaPath, paint);
          } else if (fill.value().is<PaintServer::None>()) {
            // Do nothing.
          } else {
            // TODO: Other paint types.
          }
        }

        if (auto stroke = style.stroke.get()) {
          if (stroke.value().is<PaintServer::Solid>()) {
            const PaintServer::Solid& solid = stroke.value().get<PaintServer::Solid>();

            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setStyle(SkPaint::Style::kStroke_Style);

            paint.setColor(toSkia(solid.color.withOpacity(style.strokeOpacity.get().value())));
            paint.setStrokeWidth(style.strokeWidth.get().value().value);
            paint.setStrokeCap(toSkia(style.strokeLinecap.get().value()));
            paint.setStrokeJoin(toSkia(style.strokeLinejoin.get().value()));
            paint.setStrokeMiter(style.strokeMiterlimit.get().value());

            const SkPath skiaPath = toSkia(*path->spline);

            if (style.strokeDasharray.get().has_value()) {
              double dashUnitsScale = 1.0;
              if (path->userPathLength && !NearZero(path->userPathLength.value())) {
                // If the user specifies a path length, we need to scale between the user's length
                // and computed length.
                const double skiaLength = SkPathMeasure(skiaPath, false).getLength();
                dashUnitsScale = skiaLength / path->userPathLength.value();
              }

              // TODO: Avoid the copying on property access, if possible, and try to cache the
              // computed SkDashPathEffect.
              const std::vector<Lengthd> dashes = style.strokeDasharray.get().value();
              std::vector<SkScalar> skiaDashes;
              skiaDashes.reserve(dashes.size());
              for (const Lengthd& dash : dashes) {
                skiaDashes.push_back(dash.value * dashUnitsScale);
              }

              paint.setPathEffect(SkDashPathEffect::Make(
                  skiaDashes.data(), skiaDashes.size(),
                  style.strokeDashoffset.get().value().value * dashUnitsScale));
            }

            canvas_->drawPath(skiaPath, paint);
          } else if (stroke.value().is<PaintServer::None>()) {
            // Do nothing.
          } else {
            // TODO: Other paint types.
          }
        }
      }
    }

    const TreeComponent& tree = registry.get<TreeComponent>(treeEntity);
    for (auto cur = tree.firstChild(); cur != entt::null;
         cur = registry.get<TreeComponent>(cur).nextSibling()) {
      drawEntity(transform, cur);
    }
  };

  drawEntity(Transformd(), root);
}

}  // namespace donner::svg
