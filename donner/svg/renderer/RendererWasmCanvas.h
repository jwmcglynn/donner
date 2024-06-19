#pragma once
/// @file

#include <memory>
#include <string_view>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/core/PathSpline.h"
#include "donner/svg/registry/Registry.h"
#include "donner/svg/renderer/wasm_canvas/Canvas.h"

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
