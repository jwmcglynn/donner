#include "donner/svg/SVGDocument.h"

#include "donner/base/element/ElementTraversalGenerators.h"
#include "donner/base/xml/components/XMLDocumentContext.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

namespace {

SourceRange MutationRange(const xml::XMLMutation& mutation) {
  return mutation.node.getNodeLocation().value_or(
      SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)});
}

}  // namespace

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
  resourceCtx.setProcessingMode(settings.processingMode);
  if (settings.svgParseCallback) {
    resourceCtx.setSvgParseCallback(std::move(settings.svgParseCallback));
  }

  registry_->ctx().emplace<xml::components::XMLNamespaceContext>(*registry_);
}

SVGDocument::SVGDocument() : SVGDocument(Settings()) {}

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

Transform2d SVGDocument::canvasFromDocumentTransform() const {
  return components::LayoutSystem().getCanvasFromDocumentTransform(*registry_);
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

bool SVGDocument::hasSourceStore() const {
  if (!registry_->ctx().contains<xml::components::XMLDocumentContext>()) {
    return false;
  }

  return xmlDocument().hasSourceStore();
}

std::string_view SVGDocument::source() const {
  if (!registry_->ctx().contains<xml::components::XMLDocumentContext>()) {
    return std::string_view();
  }

  return xmlDocument().source();
}

std::uint64_t SVGDocument::sourceVersion() const {
  if (!registry_->ctx().contains<xml::components::XMLDocumentContext>()) {
    return 0;
  }

  return xmlDocument().sourceVersion();
}

xml::ApplySourceEditResult SVGDocument::applySourceEdit(const xml::XMLEditIntent& intent) {
  if (!registry_->ctx().contains<xml::components::XMLDocumentContext>()) {
    xml::ApplySourceEditResult result;
    result.diagnostic = ParseDiagnostic::Error(
        "Cannot apply source edit to SVGDocument without XML source text", intent.range);
    return result;
  }

  xml::ApplySourceEditResult result = xmlDocument().applySourceEdit(intent);
  for (const xml::XMLMutation& mutation : result.mutations) {
    std::optional<ParseDiagnostic> projectionDiagnostic = applyXMLMutation(mutation);
    if (projectionDiagnostic.has_value() && !result.diagnostic.has_value()) {
      result.diagnostic = std::move(projectionDiagnostic);
    }
  }

  return result;
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

xml::XMLDocument SVGDocument::xmlDocument() const {
  return xml::XMLDocument::CreateFromRegistry(registry_);
}

std::optional<ParseDiagnostic> SVGDocument::applyXMLMutation(const xml::XMLMutation& mutation) {
  const EntityHandle handle = mutation.node.entityHandle();
  if (!handle || !handle.all_of<components::ElementTypeComponent>()) {
    return ParseDiagnostic::Error("XML mutation target is not an SVG element",
                                  MutationRange(mutation));
  }

  SVGElement element(handle);
  switch (mutation.kind) {
    case xml::XMLMutation::Kind::AttributeSet:
      if (!mutation.value.has_value()) {
        return ParseDiagnostic::Error("XML AttributeSet mutation is missing a value",
                                      MutationRange(mutation));
      }

      element.setAttribute(mutation.attributeName, *mutation.value);
      return std::nullopt;

    case xml::XMLMutation::Kind::AttributeRemoved:
      element.removeAttribute(mutation.attributeName);
      return std::nullopt;

    case xml::XMLMutation::Kind::SourceDiagnosticChanged: return std::nullopt;

    case xml::XMLMutation::Kind::NodeValueChanged:
    case xml::XMLMutation::Kind::NodeInserted:
    case xml::XMLMutation::Kind::NodeRemoved:
    case xml::XMLMutation::Kind::SubtreeReplaced:
      return ParseDiagnostic::Error("XML mutation kind is not implemented by SVGDocument",
                                    MutationRange(mutation));
  }

  UTILS_UNREACHABLE();
}

}  // namespace donner::svg
