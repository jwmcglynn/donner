#pragma once
/// @file

#include <memory>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkGraphics.h"
#include "include/core/SkImageEncoder.h"
#include "include/core/SkString.h"
#include "src/svg/core/path_spline.h"
#include "src/svg/registry/registry.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

class RendererSkia {
public:
  explicit RendererSkia(bool verbose = false);
  ~RendererSkia();

  // Copy is disallowed, move is allowed.
  RendererSkia(const RendererSkia&) = delete;
  RendererSkia& operator=(const RendererSkia&) = delete;
  RendererSkia(RendererSkia&&) noexcept;
  RendererSkia& operator=(RendererSkia&&) noexcept;

  void draw(SVGDocument& document);

  std::string drawIntoAscii(SVGDocument& document);

  sk_sp<SkPicture> drawIntoSkPicture(SVGDocument& document);

  bool save(const char* filename);

  std::span<const uint8_t> pixelData() const;
  int width() const { return bitmap_.width(); }
  int height() const { return bitmap_.height(); }

  void setAntialias(bool antialias) { antialias_ = antialias; }

private:
  class Impl;

  void draw(Registry& registry, Entity entity);

  bool verbose_;

  SkAutoGraphics ag_;
  SkBitmap bitmap_;
  SkCanvas* rootCanvas_ = nullptr;
  SkCanvas* currentCanvas_ = nullptr;
  bool antialias_ = true;
};

}  // namespace donner::svg
