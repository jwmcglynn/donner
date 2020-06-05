#pragma once

#include "src/svg/svg_element.h"

namespace donner {

class SVGPathElement : public SVGGraphicsElement {
protected:
  SVGPathElement(Registry& registry, Entity entity) : SVGGraphicsElement(registry, entity) {}

public:
  static constexpr ElementType Type = ElementType::Path;
  static constexpr std::string_view Tag = "path";

  static SVGPathElement Create(SVGDocument& document);

  std::string_view d() const;
  std::optional<ParseError> setD(std::string_view d);
};

}  // namespace donner