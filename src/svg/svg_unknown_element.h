#pragma once
/// @file

#include "src/svg/svg_graphics_element.h"

namespace donner::svg {

class SVGUnknownElement : public SVGGraphicsElement {
protected:
  explicit SVGUnknownElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Unknown;

  static SVGUnknownElement Create(SVGDocument& document, const XMLQualifiedNameRef& xmlTypeName);
};

}  // namespace donner::svg
