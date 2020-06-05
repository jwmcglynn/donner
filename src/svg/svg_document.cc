#include "src/svg/svg_document.h"

#include "src/svg/components/tree_component.h"
#include "src/svg/svg_svg_element.h"

namespace donner {

SVGDocument::SVGDocument() {
  svg_element_ = registry_.create();
  registry_.emplace<TreeComponent>(svg_element_, ElementType::SVG, svg_element_);
}

SVGSVGElement SVGDocument::svgElement() {
  return SVGSVGElement(registry_, svg_element_);
}

}  // namespace donner