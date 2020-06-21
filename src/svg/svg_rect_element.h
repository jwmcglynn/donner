#pragma once

#include <optional>

#include "src/base/length.h"
#include "src/svg/svg_element.h"

namespace donner {

class SVGRectElement : public SVGGraphicsElement {
protected:
  SVGRectElement(Registry& registry, Entity entity) : SVGGraphicsElement(registry, entity) {}

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
};

}  // namespace donner