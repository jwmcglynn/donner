#pragma once

#include "src/svg/components/registry.h"
#include "src/svg/svg_svg_element.h"

namespace donner {

class XMLParser;
class SVGSVGElement;

class SVGDocument {
public:
  SVGDocument();

  Registry& registry() { return registry_; }
  SVGSVGElement svgElement();

private:
  friend class XMLParser;

  Registry registry_;
  Entity svg_element_;
};

}  // namespace donner