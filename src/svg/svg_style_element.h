#pragma once

#include "src/base/rc_string.h"
#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGStyleElement : public SVGElement {
protected:
  SVGStyleElement(Registry& registry, Entity entity) : SVGElement(registry, entity) {}

public:
  static constexpr ElementType Type = ElementType::Style;
  static constexpr std::string_view Tag = "style";

  static SVGStyleElement Create(SVGDocument& document);

  void setType(RcString type);
  void setContents(std::string_view style);

  bool isCssType() const;
};

}  // namespace donner::svg
