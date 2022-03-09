#pragma once

#include "src/svg/registry/registry.h"
#include "src/svg/svg_svg_element.h"

namespace donner::svg {

class XMLParser;
class SVGSVGElement;

class SVGDocument {
public:
  SVGDocument();

  Registry& registry() { return *registry_; }
  const Registry& registry() const { return *registry_; }
  Entity rootEntity() const;

  SVGSVGElement svgElement();

  void setCanvasSize(int width, int height);
  void useAutomaticCanvasSize();

  bool operator==(const SVGDocument& other) const;

private:
  std::shared_ptr<Registry> registry_;
};

}  // namespace donner::svg
