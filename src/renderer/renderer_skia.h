#pragma once

#include <memory>

#include "include/core/SkCanvas.h"
#include "include/core/SkGraphics.h"
#include "include/core/SkImageEncoder.h"
#include "include/core/SkString.h"
#include "src/svg/components/registry.h"
#include "src/svg/core/path_spline.h"
#include "src/svg/svg_document.h"

namespace donner {

class RendererSkia {
public:
  RendererSkia(int width, int height);
  ~RendererSkia();

  void draw(SVGDocument& document);
  void drawPath(const PathSpline& spline, const SkPaint& paint);

  bool save(const char* filename);

private:
  void draw(Registry& registry, Entity entity);

  int width_;
  int height_;

  SkAutoGraphics ag_;
  SkBitmap bitmap_;
  std::unique_ptr<SkCanvas> canvas_;
};

}  // namespace donner