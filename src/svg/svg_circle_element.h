#pragma once

#include "src/base/length.h"
#include "src/svg/svg_geometry_element.h"

namespace donner::svg {

class SVGCircleElement : public SVGGeometryElement {
protected:
  explicit SVGCircleElement(EntityHandle handle) : SVGGeometryElement(handle) {}

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

  Lengthd computedCx() const;
  Lengthd computedCy() const;
  Lengthd computedR() const;

private:
  void invalidate() const;
  void compute() const;
};

}  // namespace donner::svg
