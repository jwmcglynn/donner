#pragma once

#include <memory>

#include "include/core/SkCanvas.h"
#include "include/core/SkGraphics.h"
#include "include/core/SkImageEncoder.h"
#include "include/core/SkString.h"
#include "src/svg/components/registry.h"
#include "src/svg/core/path_spline.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

class RendererSkia {
public:
  RendererSkia(int defaultWidth, int defaultHeight, bool verbose = false);
  ~RendererSkia();

  void draw(SVGDocument& document);

  bool save(const char* filename);

  std::span<const uint8_t> pixelData() const;
  int width() const { return bitmap_.width(); }
  int height() const { return bitmap_.height(); }

  void overrideSize() { overrideSize_ = true; }

private:
  class Impl;

  void draw(Registry& registry, Entity entity);

  int defaultWidth_;
  int defaultHeight_;
  bool overrideSize_ = false;
  bool verbose_;

  SkAutoGraphics ag_;
  SkBitmap bitmap_;
  std::unique_ptr<SkCanvas> canvas_;
};

}  // namespace donner::svg
