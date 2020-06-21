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

  void draw(const SVGDocument& document);
  void draw(const Registry& registry, Entity entity);
  void drawPath(const PathSpline& spline);

  void render();

private:
  static const void* LoadGLFunction(const char* name, void* userdata);

  PFGLRendererRef renderer_;
  PFCanvasRef canvas_;
};

}  // namespace donner
