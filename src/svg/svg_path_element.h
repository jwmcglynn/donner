#pragma once

#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGPathElement : public SVGGraphicsElement {
protected:
  SVGPathElement(Registry& registry, Entity entity) : SVGGraphicsElement(registry, entity) {}

public:
  static constexpr ElementType Type = ElementType::Path;
  static constexpr std::string_view Tag = "path";

  static SVGPathElement Create(SVGDocument& document);

  std::string_view d() const;
  std::optional<ParseError> setD(std::string_view d);

  std::optional<double> pathLength() const;
  void setPathLength(std::optional<double> value);
};

}  // namespace donner::svg
