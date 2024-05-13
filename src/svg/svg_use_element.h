#pragma once
/// @file

#include "src/base/length.h"
#include "src/base/rc_string.h"
#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGUseElement : public SVGElement {
protected:
  explicit SVGUseElement(EntityHandle handle) : SVGElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Use;
  static constexpr std::string_view Tag{"use"};

  static SVGUseElement Create(SVGDocument& document);

  void setHref(RcString value);
  void setX(Lengthd value);
  void setY(Lengthd value);
  void setWidth(std::optional<Lengthd> value);
  void setHeight(std::optional<Lengthd> value);

  RcString href() const;
  Lengthd x() const;
  Lengthd y() const;
  std::optional<Lengthd> width() const;
  std::optional<Lengthd> height() const;
};

}  // namespace donner::svg
