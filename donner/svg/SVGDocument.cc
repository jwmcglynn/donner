#include "donner/svg/SVGDocument.h"

#include "donner/base/element/ElementTraversalGenerators.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/components/DocumentContext.h"
#include "donner/svg/components/layout/LayoutSystem.h"

namespace donner::svg {

SVGDocument::SVGDocument() : registry_(std::make_unique<Registry>()) {
  components::DocumentContext& ctx =
      registry_->ctx().emplace<components::DocumentContext>(*this, *registry_);
  ctx.rootEntity = SVGSVGElement::Create(*this).entity();
}

Entity SVGDocument::rootEntity() const {
  return registry_->ctx().get<components::DocumentContext>().rootEntity;
}

SVGSVGElement SVGDocument::svgElement() const {
  return SVGSVGElement(EntityHandle(*registry_, rootEntity()));
}

void SVGDocument::setCanvasSize(int width, int height) {
  // TODO: Invalidate render tree?
  assert(width > 0 && height > 0);
  registry_->ctx().get<components::DocumentContext>().canvasSize = Vector2i(width, height);
}

void SVGDocument::useAutomaticCanvasSize() {
  // TODO: Invalidate render tree?
  registry_->ctx().get<components::DocumentContext>().canvasSize = std::nullopt;
}

Vector2i SVGDocument::canvasSize() {
  return components::LayoutSystem().calculateCanvasScaledDocumentSize(
      *registry_, components::LayoutSystem::InvalidSizeBehavior::ReturnDefault);
}

bool SVGDocument::operator==(const SVGDocument& other) const {
  return &registry_->ctx().get<const components::DocumentContext>() ==
         &other.registry_->ctx().get<const components::DocumentContext>();
}

std::optional<SVGElement> SVGDocument::querySelector(std::string_view str) {
  const auto selectorResult = css::parser::SelectorParser::Parse(str);
  if (selectorResult.hasError()) {
    return std::nullopt;
  }

  const css::Selector& selector = selectorResult.result();

  ElementTraversalGenerator<SVGElement> elements =
      allChildrenRecursiveGenerator<SVGElement>(svgElement());
  while (elements.next()) {
    SVGElement childElement = elements.getValue();
    if (selector.matches(childElement).matched) {
      return childElement;
    }
  }

  return std::nullopt;
}

}  // namespace donner::svg
