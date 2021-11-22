#pragma once

#include <pathfinder/pathfinder.h>

#include "src/svg/components/registry.h"
#include "src/svg/core/path_spline.h"
#include "src/svg/svg_document.h"

namespace donner {

class RendererPathfinder {
public:
  using GLFunction = void (*)();
  using GetProcAddressFunction = GLFunction (*)(const char*);

  RendererPathfinder(GetProcAddressFunction getProcAddressFunction, int width, int height);
  ~RendererPathfinder();

  void draw(SVGDocument& document);
  void drawPath(const PathSpline& spline, bool fill, bool stroke);

  void render();

private:
  void computePaths(Registry& registry);
  void draw(Registry& registry, Entity entity);

  static const void* LoadGLFunction(const char* name, void* userdata);

  int width_;
  int height_;
  PFGLRendererRef renderer_;
  PFCanvasRef canvas_;
};

}  // namespace donner
