#pragma once

#include "src/svg/svg_element.h"

namespace donner {

class SVGDefsElement : public SVGGraphicsElement {
protected:
  SVGDefsElement(Registry& registry, Entity entity) : SVGGraphicsElement(registry, entity) {}

public:
  static constexpr ElementType Type = ElementType::Defs;
  static constexpr std::string_view Tag = "defs";

  static SVGDefsElement Create(SVGDocument& document);
};

}  // namespace donner
