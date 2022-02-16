#pragma once

#include "src/base/length.h"
#include "src/svg/svg_geometry_element.h"

namespace donner::svg {

class SVGLineElement : public SVGGeometryElement {
protected:
  explicit SVGLineElement(EntityHandle handle) : SVGGeometryElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Line;
  static constexpr std::string_view Tag = "line";

  static SVGLineElement Create(SVGDocument& document);

  void setX1(Lengthd value);
  void setY1(Lengthd value);
  void setX2(Lengthd value);
  void setY2(Lengthd value);

  Lengthd x1() const;
  Lengthd y1() const;
  Lengthd x2() const;
  Lengthd y2() const;

private:
  void invalidate() const;
};

}  // namespace donner::svg
