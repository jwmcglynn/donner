#include "src/renderer/renderer_pathfinder.h"

#include "src/svg/components/path_component.h"
#include "src/svg/components/rect_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/style_component.h"
#include "src/svg/components/transform_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/components/viewbox_component.h"

namespace donner {

namespace {

static PFVector2F toPF(const Vector2d& vec) {
  return PFVector2F{float(vec.x), float(vec.y)};
}

static PFTransform2F toPF(const Transformd& transform) {
  return PFTransform2F{PFMatrix2x2F{float(transform.data[0]), float(transform.data[2]),
                                    float(transform.data[1]), float(transform.data[3])},
                       PFVector2F{float(transform.data[4]), float(transform.data[5])}};
}

static PFColorU toPF(const css::Color color) {
  // TODO: We need to resolve currentColor before getting here.
  return PFColorU{color.rgba().r, color.rgba().g, color.rgba().b, color.rgba().a};
}

}  // namespace

const void* RendererPathfinder::LoadGLFunction(const char* name, void* userdata) {
  GetProcAddressFunction f = reinterpret_cast<GetProcAddressFunction>(userdata);
  return reinterpret_cast<const void*>(f(name));
}

RendererPathfinder::RendererPathfinder(GetProcAddressFunction getProcAddress, int width, int height)
    : width_(width), height_(height) {
  PFGLLoadWith(RendererPathfinder::LoadGLFunction, reinterpret_cast<void*>(getProcAddress));

  const PFVector2I windowSize = {width, height};
  PFGLDestFramebufferRef destFramebuffer = PFGLDestFramebufferCreateFullWindow(&windowSize);

  const PFRendererOptions options = {PFColorF{1.0, 1.0, 1.0, 1.0},
                                     PF_RENDERER_OPTIONS_FLAGS_HAS_BACKGROUND_COLOR};
  renderer_ = PFGLRendererCreate(PFGLDeviceCreate(PF_GL_VERSION_GL3, 0),
                                 PFFilesystemResourceLoaderLocate(), destFramebuffer, &options);

  const PFVector2F canvasSize = {float(width), float(height)};
  canvas_ = PFCanvasCreate(PFCanvasFontContextCreateWithSystemSource(), &canvasSize);
}

RendererPathfinder::~RendererPathfinder() {
  if (canvas_) {
    PFCanvasDestroy(canvas_);
  }
  PFGLRendererDestroy(renderer_);
}

void RendererPathfinder::draw(SVGDocument& document) {
  computePaths(document.registry());
  draw(document.registry(), document.rootEntity());
}

void RendererPathfinder::drawPath(const PathSpline& spline, bool fill, bool stroke) {
  PFPathRef path = PFPathCreate();
  const std::vector<Vector2d>& points = spline.points();

  for (const PathSpline::Command& command : spline.commands()) {
    switch (command.type) {
      case PathSpline::CommandType::MoveTo: {
        auto pt = toPF(points[command.point_index]);
        PFPathMoveTo(path, &pt);
        break;
      }
      case PathSpline::CommandType::CurveTo: {
        auto c0 = toPF(points[command.point_index]);
        auto c1 = toPF(points[command.point_index + 1]);
        auto end = toPF(points[command.point_index + 2]);
        PFPathBezierCurveTo(path, &c0, &c1, &end);
        break;
      }
      case PathSpline::CommandType::LineTo: {
        auto pt = toPF(points[command.point_index]);
        PFPathLineTo(path, &pt);
        break;
      }
      case PathSpline::CommandType::ClosePath: {
        PFPathClosePath(path);
        break;
      }
    }
  }

  PFCanvasSetLineWidth(canvas_, 1.0f);

  if (fill) {
    PFPathRef clonedPath = nullptr;

    if (stroke) {
      clonedPath = PFPathClone(path);
    }

    PFCanvasFillPath(canvas_, path);
    path = clonedPath;
  }

  if (stroke) {
    PFCanvasStrokePath(canvas_, path);
  }
}

void RendererPathfinder::render() {
  PFSceneRef scene = PFCanvasCreateScene(canvas_);
  canvas_ = nullptr;

  PFSceneProxyRef sceneProxy = PFSceneProxyCreateFromSceneAndRayonExecutor(scene);
  PFSceneProxyBuildAndRenderGL(sceneProxy, renderer_, PFBuildOptionsCreate());
  PFSceneProxyDestroy(sceneProxy);
}

void RendererPathfinder::computePaths(Registry& registry) {
  auto view = registry.view<RectComponent>();
  for (auto entity : view) {
    auto [rect] = view.get(entity);
    rect.computePath(registry.get_or_emplace<ComputedPathComponent>(entity));
  }
}

void RendererPathfinder::draw(Registry& registry, Entity root) {
  std::function<void(Transformd, Entity)> drawEntity = [&](Transformd transform, Entity entity) {
    if (const auto* tc = registry.try_get<TransformComponent>(entity)) {
      transform = tc->transform * transform;
    }

    PFTransform2F pfTransform = toPF(transform);
    PFCanvasSetTransform(canvas_, &pfTransform);

    bool paintFill = true;
    bool paintStroke = true;

    const StyleComponent& style = registry.get_or_emplace<StyleComponent>(entity);
    {
      std::optional<svg::PaintServer> fill = style.properties.fill.get();

      if (fill.has_value()) {
        if (fill.value().is<svg::PaintServer::Solid>()) {
          const svg::PaintServer::Solid& solid = fill.value().get<svg::PaintServer::Solid>();

          PFColorU solidColor = toPF(solid.color);
          PFFillStyleRef fillStyle = PFFillStyleCreateColor(&solidColor);
          PFCanvasSetFillStyle(canvas_, fillStyle);
          PFFillStyleDestroy(fillStyle);
        } else if (fill.value().is<svg::PaintServer::None>()) {
          paintFill = false;
        } else {
          // TODO: Other paint types.
          paintFill = false;
        }
      } else {
        paintFill = false;
      }
    }

    {
      std::optional<svg::PaintServer> stroke = style.properties.stroke.get();

      if (stroke.has_value()) {
        if (stroke.value().is<svg::PaintServer::Solid>()) {
          const svg::PaintServer::Solid& solid = stroke.value().get<svg::PaintServer::Solid>();

          PFColorU solidColor = toPF(solid.color);
          PFFillStyleRef fillStyle = PFFillStyleCreateColor(&solidColor);
          PFCanvasSetStrokeStyle(canvas_, fillStyle);
          PFFillStyleDestroy(fillStyle);
        } else if (stroke.value().is<svg::PaintServer::None>()) {
          paintStroke = false;
        } else {
          // TODO: Other paint types.
          paintStroke = false;
        }
      } else {
        paintStroke = false;
      }
    }

    if (paintFill || paintStroke) {
      if (const auto* path = registry.try_get<ComputedPathComponent>(entity)) {
        if (auto maybeSpline = path->spline()) {
          drawPath(*maybeSpline, paintFill, paintStroke);
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