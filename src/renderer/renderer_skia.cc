#include "src/renderer/renderer_skia.h"

#include "include/core/SkPath.h"
#include "include/core/SkStream.h"
#include "include/effects/SkDashPathEffect.h"
#include "src/renderer/renderer_utils.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/path_component.h"
#include "src/svg/components/rect_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/transform_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/components/viewbox_component.h"

namespace donner {

namespace {

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

SkPaint::Cap toSkia(svg::StrokeLinecap lineCap) {
  switch (lineCap) {
    case svg::StrokeLinecap::Butt: return SkPaint::Cap::kButt_Cap;
    case svg::StrokeLinecap::Round: return SkPaint::Cap::kRound_Cap;
    case svg::StrokeLinecap::Square: return SkPaint::Cap::kSquare_Cap;
  }
}

SkPaint::Join toSkia(svg::StrokeLinejoin lineJoin) {
  // TODO: Implement MiterClip and Arcs. For now, fallback to Miter, which is the default linejoin,
  // since the feature is not implemented.
  switch (lineJoin) {
    case svg::StrokeLinejoin::Miter: return SkPaint::Join::kMiter_Join;
    case svg::StrokeLinejoin::MiterClip: return SkPaint::Join::kMiter_Join;
    case svg::StrokeLinejoin::Round: return SkPaint::Join::kRound_Join;
    case svg::StrokeLinejoin::Bevel: return SkPaint::Join::kBevel_Join;
    case svg::StrokeLinejoin::Arcs: return SkPaint::Join::kMiter_Join;
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
RendererSkia::RendererSkia(int width, int height) : width_(width), height_(height) {
  bitmap_.allocPixels(SkImageInfo::MakeN32Premul(width, height));
  canvas_ = std::make_unique<SkCanvas>(bitmap_);
}

RendererSkia::~RendererSkia() {}

void RendererSkia::draw(SVGDocument& document) {
  RendererUtils::prepareDocumentForRendering(document, Vector2d(width_, height_));
  draw(document.registry(), document.rootEntity());
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
  std::function<void(Transformd, Entity)> drawEntity = [&](Transformd transform, Entity entity) {
    if (const auto* tc = registry.try_get<TransformComponent>(entity)) {
      transform = tc->transform * transform;
    }

    canvas_->setMatrix(toSkia(transform));

    if (const auto* path = registry.try_get<ComputedPathComponent>(entity)) {
      if (auto maybeSpline = path->spline()) {
        const svg::PropertyRegistry& style =
            registry.get<ComputedStyleComponent>(entity).properties();
        if (auto fill = style.fill.get()) {
          if (fill.value().is<svg::PaintServer::Solid>()) {
            const svg::PaintServer::Solid& solid = fill.value().get<svg::PaintServer::Solid>();

            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(toSkia(solid.color));
            paint.setStyle(SkPaint::Style::kFill_Style);

            canvas_->drawPath(toSkia(*maybeSpline), paint);
          } else if (fill.value().is<svg::PaintServer::None>()) {
            // Do nothing.
          } else {
            // TODO: Other paint types.
          }
        }

        if (auto stroke = style.stroke.get()) {
          if (stroke.value().is<svg::PaintServer::Solid>()) {
            const svg::PaintServer::Solid& solid = stroke.value().get<svg::PaintServer::Solid>();

            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setStyle(SkPaint::Style::kStroke_Style);

            paint.setColor(toSkia(solid.color.withOpacity(style.strokeOpacity.get().value())));
            paint.setStrokeWidth(style.strokeWidth.get().value().value);
            paint.setStrokeCap(toSkia(style.strokeLinecap.get().value()));
            paint.setStrokeJoin(toSkia(style.strokeLinejoin.get().value()));
            paint.setStrokeMiter(style.strokeMiterlimit.get().value());

            const SkPath skiaPath = toSkia(*maybeSpline);

            if (style.strokeDasharray.get().has_value()) {
              // TODO: Avoid the copying on property access, if possible, and try to cache the
              // computed SkDashPathEffect.
              const std::vector<Lengthd> dashes = style.strokeDasharray.get().value();
              std::vector<SkScalar> skiaDashes;
              skiaDashes.reserve(dashes.size());
              for (const Lengthd& dash : dashes) {
                skiaDashes.push_back(dash.value);
              }

              paint.setPathEffect(
                  SkDashPathEffect::Make(skiaDashes.data(), skiaDashes.size(),
                                         style.strokeDashoffset.get().value().value));
            }

            canvas_->drawPath(skiaPath, paint);
          } else if (stroke.value().is<svg::PaintServer::None>()) {
            // Do nothing.
          } else {
            // TODO: Other paint types.
          }
        }
      }
    }

    const TreeComponent& tree = registry.get<TreeComponent>(entity);
    for (auto cur = tree.firstChild(); cur != entt::null;
         cur = registry.get<TreeComponent>(cur).nextSibling()) {
      drawEntity(transform, cur);
    }
  };

  // Get initial transform.
  Boxd initialSize({0, 0}, {width_, height_});
  Transformd transform;
  if (const auto* sizedComponent = registry.try_get<SizedElementComponent>(root)) {
    initialSize.top_left.x = sizedComponent->x.value;
    initialSize.top_left.y = sizedComponent->y.value;

    if (sizedComponent->width) {
      initialSize.bottom_right.x = sizedComponent->width->value;
    }

    if (sizedComponent->height) {
      initialSize.bottom_right.y = sizedComponent->height->value;
    }
  }

  if (const auto* viewbox = registry.try_get<ViewboxComponent>(root)) {
    transform = viewbox->computeTransform(initialSize);
  }

  drawEntity(transform, root);
}

}  // namespace donner
