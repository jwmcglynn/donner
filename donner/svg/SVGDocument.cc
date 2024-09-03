#include "donner/svg/SVGDocument.h"

#include <filesystem>

#include "donner/base/element/ElementTraversalGenerators.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/components/DocumentContext.h"
#include "donner/svg/components/RenderingContext.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"

namespace donner::svg {

SVGDocument::SVGDocument(Settings settings) : registry_(std::make_unique<Registry>()) {
  components::DocumentContext& ctx = registry_->ctx().emplace<components::DocumentContext>(
      components::DocumentContext::InternalCtorTag{}, *this, *registry_);
  ctx.rootEntity = SVGSVGElement::Create(*this).entity();

  components::ResourceManagerContext& resourceCtx =
      registry_->ctx().emplace<components::ResourceManagerContext>(*registry_);
  resourceCtx.setResourceLoader(std::move(settings.resourceLoader));
}

Entity SVGDocument::rootEntity() const {
  return registry_->ctx().get<components::DocumentContext>().rootEntity;
}

SVGSVGElement SVGDocument::svgElement() const {
  return SVGSVGElement(EntityHandle(*registry_, rootEntity()));
}

void SVGDocument::setCanvasSize(int width, int height) {
  assert(width > 0 && height > 0);
  components::RenderingContext(*registry_).invalidateRenderTree();
  registry_->ctx().get<components::DocumentContext>().canvasSize = Vector2i(width, height);
}

void SVGDocument::useAutomaticCanvasSize() {
  components::RenderingContext(*registry_).invalidateRenderTree();
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
