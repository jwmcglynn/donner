#include "src/svg/svg_document.h"

#include "src/svg/components/document_context.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/svg_svg_element.h"

namespace donner {

SVGDocument::SVGDocument() {
  svgElement_ = registry_.create();
  registry_.set<DocumentContext>(*this);

  registry_.emplace<TreeComponent>(svgElement_, ElementType::SVG, "svg", svgElement_);
}

SVGSVGElement SVGDocument::svgElement() {
  return SVGSVGElement(registry_, rootEntity());
}

bool SVGDocument::operator==(const SVGDocument& other) const {
  return &registry_.ctx<const DocumentContext>() == &other.registry_.ctx<const DocumentContext>();
}

}  // namespace donner
