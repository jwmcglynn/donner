#include "src/svg/svg_document.h"

#include "src/svg/components/document_context.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/svg_svg_element.h"

namespace donner::svg {

SVGDocument::SVGDocument() : registry_(std::make_unique<Registry>()) {
  DocumentContext& ctx = registry_->set<DocumentContext>(*this, *registry_);
  ctx.rootEntity = SVGSVGElement::Create(*this).entity();
}

Entity SVGDocument::rootEntity() const {
  return registry_->ctx<DocumentContext>().rootEntity;
}

SVGSVGElement SVGDocument::svgElement() {
  return SVGSVGElement(EntityHandle(*registry_, rootEntity()));
}

void SVGDocument::setCanvasSize(int width, int height) {
  // TODO: Invalidate render tree?
  assert(width > 0 && height > 0);
  registry_->ctx<DocumentContext>().canvasSize = Vector2i(width, height);
}

void SVGDocument::useAutomaticCanvasSize() {
  // TODO: Invalidate render tree?
  registry_->ctx<DocumentContext>().canvasSize = std::nullopt;
}

bool SVGDocument::operator==(const SVGDocument& other) const {
  return &registry_->ctx<const DocumentContext>() == &other.registry_->ctx<const DocumentContext>();
}

}  // namespace donner::svg
