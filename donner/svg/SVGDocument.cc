#include "donner/svg/SVGDocument.h"

#include "donner/base/element/ElementTraversalGenerators.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

SVGDocument::SVGDocument(std::shared_ptr<Registry> registry, Settings settings,
                         EntityHandle ontoEntityHandle)
    : registry_(std::move(registry)) {
  auto& ctx = registry_->ctx().emplace<components::SVGDocumentContext>(
      components::SVGDocumentContext::InternalCtorTag{}, registry_);
  if (ontoEntityHandle) {
    ctx.rootEntity = SVGSVGElement::CreateOn(ontoEntityHandle).entityHandle().entity();
  } else {
    ctx.rootEntity = SVGSVGElement::Create(*this).entityHandle().entity();
  }

  components::ResourceManagerContext& resourceCtx =
      registry_->ctx().emplace<components::ResourceManagerContext>(*registry_);
  resourceCtx.setResourceLoader(std::move(settings.resourceLoader));
  resourceCtx.setExternalFontLoadingEnabled(settings.externalFontLoadingEnabled);
  resourceCtx.setFontRenderMode(settings.renderMode);

  registry_->ctx().emplace<xml::components::XMLNamespaceContext>(*registry_);
}

SVGDocument::SVGDocument() : SVGDocument(Settings{}) {}

SVGDocument::SVGDocument(Settings settings)
    : SVGDocument(std::make_shared<Registry>(), std::move(settings), EntityHandle()) {}

EntityHandle SVGDocument::rootEntityHandle() const {
  return EntityHandle(*registry_,
                      registry_->ctx().get<components::SVGDocumentContext>().rootEntity);
}

SVGSVGElement SVGDocument::svgElement() const {
  return SVGSVGElement(rootEntityHandle());
}

void SVGDocument::setCanvasSize(int width, int height) {
  assert(width > 0 && height > 0);
  components::RenderingContext(*registry_).invalidateRenderTree();
  registry_->ctx().get<components::SVGDocumentContext>().canvasSize = Vector2i(width, height);
}

Transformd SVGDocument::documentFromCanvasTransform() const {
  return components::LayoutSystem().getDocumentFromCanvasTransform(*registry_);
}

void SVGDocument::useAutomaticCanvasSize() {
  components::RenderingContext(*registry_).invalidateRenderTree();
  registry_->ctx().get<components::SVGDocumentContext>().canvasSize = std::nullopt;
}

Vector2i SVGDocument::canvasSize() const {
  return components::LayoutSystem().calculateCanvasScaledDocumentSize(
      *registry_, components::LayoutSystem::InvalidSizeBehavior::ReturnDefault);
}

bool SVGDocument::operator==(const SVGDocument& other) const {
  return &registry_->ctx().get<const components::SVGDocumentContext>() ==
         &other.registry_->ctx().get<const components::SVGDocumentContext>();
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
