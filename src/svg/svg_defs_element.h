#pragma once

#include "src/svg/svg_graphics_element.h"

namespace donner::svg {

class SVGDefsElement : public SVGGraphicsElement {
protected:
  explicit SVGDefsElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Defs;
  static constexpr std::string_view Tag = "defs";

  static SVGDefsElement Create(SVGDocument& document);
};

}  // namespace donner::svg
