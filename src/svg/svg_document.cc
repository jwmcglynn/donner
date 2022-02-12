#include "src/svg/svg_document.h"

#include "src/svg/components/document_context.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/svg_svg_element.h"

namespace donner::svg {

SVGDocument::SVGDocument() : registry_(std::make_unique<Registry>()) {
  registry_->set<DocumentContext>(*this, *registry_);

  svgElement_ = SVGSVGElement::Create(*this).entity();
}

SVGSVGElement SVGDocument::svgElement() {
  return SVGSVGElement(EntityHandle(*registry_, rootEntity()));
}

bool SVGDocument::operator==(const SVGDocument& other) const {
  return &registry_->ctx<const DocumentContext>() == &other.registry_->ctx<const DocumentContext>();
}

}  // namespace donner::svg
