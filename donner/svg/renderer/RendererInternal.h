#pragma once
/// @file

#include <memory>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {

/// @cond INTERNAL
class RendererImplementation : public RendererInterface {
public:
  ~RendererImplementation() override = default;

  void draw(SVGDocument& document) override = 0;

  [[nodiscard]] int width() const override = 0;

  [[nodiscard]] int height() const override = 0;
};

std::unique_ptr<RendererImplementation> CreateRendererImplementation(bool verbose);
/// @endcond

}  // namespace donner::svg
