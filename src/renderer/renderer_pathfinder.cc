#include "src/renderer/renderer_pathfinder.h"

#include "src/svg/components/path_component.h"
#include "src/svg/components/rect_component.h"
#include "src/svg/components/sized_element_component.h"
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

void RendererPathfinder::drawPath(const PathSpline& spline) {
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
  PFCanvasStrokePath(canvas_, path);
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

    if (const auto* path = registry.try_get<ComputedPathComponent>(entity)) {
      if (auto maybeSpline = path->spline()) {
        drawPath(*maybeSpline);
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