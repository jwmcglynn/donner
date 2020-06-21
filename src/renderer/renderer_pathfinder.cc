#include "src/renderer/renderer_pathfinder.h"

#include "src/svg/components/path_component.h"
#include "src/svg/components/tree_component.h"

namespace donner {

namespace {

static PFVector2F toPF(const Vector2d& vec) {
  return PFVector2F{float(vec.x), float(vec.y)};
}

}  // namespace

const void* RendererPathfinder::LoadGLFunction(const char* name, void* userdata) {
  GetProcAddressFunction f = reinterpret_cast<GetProcAddressFunction>(userdata);
  return reinterpret_cast<const void*>(f(name));
}

RendererPathfinder::RendererPathfinder(GetProcAddressFunction getProcAddress, int width,
                                       int height) {
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

void RendererPathfinder::draw(const SVGDocument& document) {
  draw(document.registry(), document.rootEntity());
}

void RendererPathfinder::draw(const Registry& registry, Entity entity) {
  if (registry.has<PathComponent>(entity)) {
    const PathComponent& path = registry.get<PathComponent>(entity);
    if (auto maybeSpline = path.spline()) {
      drawPath(*maybeSpline);
    }
  }

  const TreeComponent& tree = registry.get<TreeComponent>(entity);
  for (auto cur = tree.firstChild(); cur != entt::null;
       cur = registry.get<TreeComponent>(cur).nextSibling()) {
    draw(registry, cur);
  }
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

  PFCanvasSetLineWidth(canvas_, 10.0f);
  PFCanvasStrokePath(canvas_, path);
}

void RendererPathfinder::render() {
  PFSceneRef scene = PFCanvasCreateScene(canvas_);
  canvas_ = nullptr;

  PFSceneProxyRef sceneProxy = PFSceneProxyCreateFromSceneAndRayonExecutor(scene);
  PFSceneProxyBuildAndRenderGL(sceneProxy, renderer_, PFBuildOptionsCreate());
  PFSceneProxyDestroy(sceneProxy);
}

}  // namespace donner