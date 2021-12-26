#include "src/renderer/renderer_skia.h"

#include "include/core/SkPath.h"
#include "include/core/SkStream.h"
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

void RendererSkia::drawPath(const PathSpline& spline, const SkPaint& paint) {
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

  canvas_->drawPath(path, paint);
}

bool RendererSkia::save(const char* filename) {
  return RendererUtils::writeRgbaPixelsToPngFile(
      filename,
      std::span<const uint8_t>(static_cast<const uint8_t*>(bitmap_.getPixels()),
                               bitmap_.computeByteSize()),
      width_, height_);
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
            drawPath(*maybeSpline, paint);
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
            paint.setColor(toSkia(solid.color));
            paint.setStyle(SkPaint::Style::kStroke_Style);
            // TODO: Handle units.
            paint.setStrokeWidth(style.strokeWidth.get().value().value);
            drawPath(*maybeSpline, paint);
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
