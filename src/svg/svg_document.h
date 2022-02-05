#pragma once

#include "src/svg/components/registry.h"
#include "src/svg/svg_svg_element.h"

namespace donner {

class XMLParser;
class SVGSVGElement;

class SVGDocument {
public:
  SVGDocument();

  Registry& registry() { return *registry_; }
  const Registry& registry() const { return *registry_; }
  Entity rootEntity() const { return svgElement_; }

  SVGSVGElement svgElement();

  bool operator==(const SVGDocument& other) const;

private:
  std::shared_ptr<Registry> registry_;
  Entity svgElement_;
};

}  // namespace donner
