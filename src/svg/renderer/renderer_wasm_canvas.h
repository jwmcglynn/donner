#pragma once
/// @file

#include <memory>
#include <string_view>

#include "src/svg/core/path_spline.h"
#include "src/svg/registry/registry.h"
#include "src/svg/renderer/wasm_canvas/canvas.h"
#include "src/svg/svg_document.h"

typedef struct HTMLCanvasElement HTMLCanvasElement;

namespace donner::svg {

class RendererWasmCanvas {
public:
  explicit RendererWasmCanvas(std::string_view canvasId, bool verbose = false);
  ~RendererWasmCanvas();

  void draw(SVGDocument& document);

  int width() const;
  int height() const;

private:
  class Impl;

  void draw(Registry& registry, Entity entity);

  bool verbose_;
  canvas::Canvas canvas_;
};

}  // namespace donner::svg
