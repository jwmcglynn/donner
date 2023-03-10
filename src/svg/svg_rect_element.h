#pragma once
/// @file

#include "src/base/length.h"
#include "src/svg/core/path_spline.h"
#include "src/svg/svg_geometry_element.h"

namespace donner::svg {

class SVGRectElement : public SVGGeometryElement {
protected:
  explicit SVGRectElement(EntityHandle handle) : SVGGeometryElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Rect;
  static constexpr std::string_view Tag = "rect";

  static SVGRectElement Create(SVGDocument& document);

  void setX(Lengthd value);
  void setY(Lengthd value);
  void setWidth(Lengthd value);
  void setHeight(Lengthd value);
  void setRx(std::optional<Lengthd> value);
  void setRy(std::optional<Lengthd> value);

  Lengthd x() const;
  Lengthd y() const;
  Lengthd width() const;
  Lengthd height() const;
  std::optional<Lengthd> rx() const;
  std::optional<Lengthd> ry() const;

  Lengthd computedX() const;
  Lengthd computedY() const;
  Lengthd computedWidth() const;
  Lengthd computedHeight() const;
  Lengthd computedRx() const;
  Lengthd computedRy() const;

  std::optional<PathSpline> computedSpline() const;

private:
  void invalidate() const;
  void compute() const;
};

}  // namespace donner::svg
