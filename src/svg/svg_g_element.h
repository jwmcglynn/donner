#pragma once

#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGGElement : public SVGGraphicsElement {
protected:
  explicit SVGGElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::G;
  static constexpr std::string_view Tag = "g";

  static SVGGElement Create(SVGDocument& document);
};

}  // namespace donner::svg