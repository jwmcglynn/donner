#pragma once

#include "src/base/length.h"
#include "src/svg/svg_gradient_element.h"

namespace donner::svg {

class SVGRadialGradientElement : public SVGGradientElement {
protected:
  explicit SVGRadialGradientElement(EntityHandle handle) : SVGGradientElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::RadialGradient;
  static constexpr std::string_view Tag = "radialGradient";

  static SVGRadialGradientElement Create(SVGDocument& document);

  void setCx(Lengthd value);
  void setCy(Lengthd value);
  void setR(Lengthd value);
  void setFx(std::optional<Lengthd> value);
  void setFy(std::optional<Lengthd> value);
  void setFr(Lengthd value);

  Lengthd cx() const;
  Lengthd cy() const;
  Lengthd r() const;
  std::optional<Lengthd> fx() const;
  std::optional<Lengthd> fy() const;
  Lengthd fr() const;
};

}  // namespace donner::svg
