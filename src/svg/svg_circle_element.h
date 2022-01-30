#pragma once

#include <optional>

#include "src/base/length.h"
#include "src/svg/svg_element.h"

namespace donner {

class SVGCircleElement : public SVGGraphicsElement {
protected:
  SVGCircleElement(Registry& registry, Entity entity) : SVGGraphicsElement(registry, entity) {}

public:
  static constexpr ElementType Type = ElementType::Circle;
  static constexpr std::string_view Tag = "circle";

  static SVGCircleElement Create(SVGDocument& document);

  void setCx(Lengthd value);
  void setCy(Lengthd value);
  void setR(Lengthd value);

  Lengthd cx() const;
  Lengthd cy() const;
  Lengthd r() const;
};

}  // namespace donner
