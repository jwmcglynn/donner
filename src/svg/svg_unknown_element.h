#pragma once

#include "src/svg/svg_element.h"

namespace donner {

class SVGUnknownElement : public SVGGraphicsElement {
protected:
  SVGUnknownElement(Registry& registry, Entity entity) : SVGGraphicsElement(registry, entity) {}

public:
  static constexpr ElementType Type = ElementType::Unknown;

  static SVGUnknownElement Create(SVGDocument& document, RcString typeString);
};

}  // namespace donner