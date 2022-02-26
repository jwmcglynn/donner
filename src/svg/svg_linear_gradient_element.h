#pragma once

#include <optional>

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

  void setX1(std::optional<Lengthd> value);
  void setY1(std::optional<Lengthd> value);
  void setX2(std::optional<Lengthd> value);
  void setY2(std::optional<Lengthd> value);

  std::optional<Lengthd> x1() const;
  std::optional<Lengthd> y1() const;
  std::optional<Lengthd> x2() const;
  std::optional<Lengthd> y2() const;
};

}  // namespace donner::svg
