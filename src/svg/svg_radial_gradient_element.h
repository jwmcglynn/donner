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

  void setCx(std::optional<Lengthd> value);
  void setCy(std::optional<Lengthd> value);
  void setR(std::optional<Lengthd> value);
  void setFx(std::optional<Lengthd> value);
  void setFy(std::optional<Lengthd> value);
  void setFr(std::optional<Lengthd> value);

  std::optional<Lengthd> cx() const;
  std::optional<Lengthd> cy() const;
  std::optional<Lengthd> r() const;
  std::optional<Lengthd> fx() const;
  std::optional<Lengthd> fy() const;
  std::optional<Lengthd> fr() const;
};

}  // namespace donner::svg
