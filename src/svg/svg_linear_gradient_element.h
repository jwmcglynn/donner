#pragma once

#include "src/base/length.h"
#include "src/svg/svg_gradient_element.h"

namespace donner::svg {

class SVGLinearGradientElement : public SVGGradientElement {
protected:
  explicit SVGLinearGradientElement(EntityHandle handle) : SVGGradientElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::LinearGradient;
  static constexpr std::string_view Tag = "linearGradient";

  static SVGLinearGradientElement Create(SVGDocument& document);

  void setX1(Lengthd value);
  void setY1(Lengthd value);
  void setX2(Lengthd value);
  void setY2(Lengthd value);

  Lengthd x1() const;
  Lengthd y1() const;
  Lengthd x2() const;
  Lengthd y2() const;
};

}  // namespace donner::svg
