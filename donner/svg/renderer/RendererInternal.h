#pragma once
/// @file

#include <memory>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {

class RendererImplementation : public RendererInterface {
public:
  ~RendererImplementation() override = default;

  virtual void draw(SVGDocument& document) = 0;

  [[nodiscard]] virtual int width() const = 0;

  [[nodiscard]] virtual int height() const = 0;
};

std::unique_ptr<RendererImplementation> CreateRendererImplementation(bool verbose);

}  // namespace donner::svg
