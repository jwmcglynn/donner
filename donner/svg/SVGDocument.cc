#include "donner/svg/SVGDocument.h"

#include "donner/base/element/ElementTraversalGenerators.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/base/xml/components/XMLDocumentContext.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

namespace {

SourceRange MutationRange(std::string_view source, const xml::XMLMutation& mutation) {
  if (!mutation.attributeName.name.empty()) {
    std::optional<SourceRange> attributeRange =
        mutation.node.getAttributeLocation(source, mutation.attributeName);
    if (attributeRange.has_value()) {
      return *attributeRange;
    }
  }

  return mutation.node.getNodeLocation().value_or(
      SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)});
}

void InvalidateTextGeometry(EntityHandle handle) {
  Registry& registry = *handle.registry();
  Entity current = handle.entity();
  while (current != entt::null) {
    if (registry.any_of<components::TextRootComponent>(current)) {
      registry.remove<components::ComputedTextGeometryComponent>(current);
      registry.get_or_emplace<components::DirtyFlagsComponent>(current).mark(
          components::DirtyFlagsComponent::TextGeometry |
          components::DirtyFlagsComponent::RenderInstance);
      return;
    }

    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (tree == nullptr) {
      return;
    }

    current = tree->parent();
  }
}

std::optional<EntityHandle> TextElementHandleForNodeValueMutation(
    const xml::XMLMutation& mutation) {
  if (mutation.node.type() == xml::XMLNode::Type::Element) {
    return mutation.node.entityHandle();
  }

  if (mutation.node.type() == xml::XMLNode::Type::Data ||
      mutation.node.type() == xml::XMLNode::Type::CData) {
    if (std::optional<xml::XMLNode> parent = mutation.node.parentElement()) {
      return parent->entityHandle();
    }
  }

  return std::nullopt;
}

std::optional<ParseDiagnostic> ApplyNodeValueChanged(std::string_view source,
                                                     const xml::XMLMutation& mutation) {
  if (!mutation.value.has_value()) {
    return ParseDiagnostic::Error("XML NodeValueChanged mutation is missing a value",
                                  MutationRange(source, mutation));
  }

  std::optional<EntityHandle> targetHandle = TextElementHandleForNodeValueMutation(mutation);
  if (!targetHandle.has_value() || !*targetHandle ||
      !targetHandle->all_of<components::TextComponent>()) {
    return ParseDiagnostic::Error("XML NodeValueChanged mutation target is not SVG text content",
                                  MutationRange(source, mutation));
  }

  auto& text = targetHandle->get<components::TextComponent>();
  text.text = *mutation.value;
  text.textChunks.clear();
  text.textChunks.emplace_back(*mutation.value);
  InvalidateTextGeometry(*targetHandle);
  return std::nullopt;
}

ElementType ElementTypeForTag(const xml::XMLQualifiedNameRef& tagName) {
  if (tagName.name == "circle") {
    return ElementType::Circle;
  }
  if (tagName.name == "ellipse") {
    return ElementType::Ellipse;
  }
  if (tagName.name == "g") {
    return ElementType::G;
  }
  if (tagName.name == "line") {
    return ElementType::Line;
  }
  if (tagName.name == "path") {
    return ElementType::Path;
  }
  if (tagName.name == "polygon") {
    return ElementType::Polygon;
  }
  if (tagName.name == "polyline") {
    return ElementType::Polyline;
  }
  if (tagName.name == "rect") {
    return ElementType::Rect;
  }

  return ElementType::Unknown;
}

bool UsesNoTraverseChildren(ElementType type) {
  switch (type) {
    case ElementType::Circle:
    case ElementType::Ellipse:
    case ElementType::Line:
    case ElementType::Path:
    case ElementType::Polygon:
    case ElementType::Polyline:
    case ElementType::Rect: return true;

    default: return false;
  }
}

void EnsureProjectedElementComponents(EntityHandle handle,
                                      const xml::XMLQualifiedNameRef& tagName) {
  if (!handle.all_of<components::ElementTypeComponent>()) {
    const ElementType type = ElementTypeForTag(tagName);
    handle.emplace<components::ElementTypeComponent>(type);
    handle.emplace<components::TransformComponent>();
    if (UsesNoTraverseChildren(type)) {
      handle.emplace<components::RenderingBehaviorComponent>(
          components::RenderingBehavior::NoTraverseChildren);
    }
  }
}

SourceRange AttributeRange(std::string_view source, const xml::XMLNode& node,
                           const xml::XMLQualifiedNameRef& name) {
  return node.getAttributeLocation(source, name)
      .value_or(node.getNodeLocation().value_or(
          SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)}));
}

components::RenderTreeState& GetRenderTreeState(EntityHandle handle) {
  Registry& registry = *handle.registry();
  if (!registry.ctx().contains<components::RenderTreeState>()) {
    registry.ctx().emplace<components::RenderTreeState>();
  }
  return registry.ctx().get<components::RenderTreeState>();
}

void MarkSubtreeReplaced(EntityHandle handle) {
  components::RenderTreeState& renderState = GetRenderTreeState(handle);
  renderState.needsFullRebuild = true;
  renderState.needsFullStyleRecompute = true;

  if (handle.all_of<components::ElementTypeComponent>()) {
    handle.get_or_emplace<components::DirtyFlagsComponent>().mark(
        components::DirtyFlagsComponent::All);
  }

  donner::components::ForAllChildrenRecursive(handle, [&](EntityHandle descendant) {
    if (descendant.entity() != handle.entity() &&
        descendant.all_of<components::ElementTypeComponent>()) {
      descendant.get_or_emplace<components::DirtyFlagsComponent>().mark(
          components::DirtyFlagsComponent::All);
    }
  });
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
  if (mutation.kind == xml::XMLMutation::Kind::NodeValueChanged) {
    return ApplyNodeValueChanged(source(), mutation);
  }

  const EntityHandle handle = mutation.node.entityHandle();
  if (!handle || !handle.all_of<components::ElementTypeComponent>()) {
    return ParseDiagnostic::Error("XML mutation target is not an SVG element",
                                  MutationRange(source(), mutation));
  }

  SVGElement element(handle);
  switch (mutation.kind) {
    case xml::XMLMutation::Kind::AttributeSet:
      if (!mutation.value.has_value()) {
        return ParseDiagnostic::Error("XML AttributeSet mutation is missing a value",
                                      MutationRange(source(), mutation));
      }

      if (std::optional<ParseDiagnostic> diagnostic =
              element.setAttributeFromXMLMutation(mutation.attributeName, *mutation.value)) {
        diagnostic->range = MutationRange(source(), mutation);
        return diagnostic;
      }

      return std::nullopt;

    case xml::XMLMutation::Kind::AttributeRemoved:
      element.removeAttributeFromXMLMutation(mutation.attributeName);
      return std::nullopt;

    case xml::XMLMutation::Kind::SourceDiagnosticChanged: return std::nullopt;

    case xml::XMLMutation::Kind::NodeValueChanged: UTILS_UNREACHABLE();

    case xml::XMLMutation::Kind::SubtreeReplaced:
      if (std::optional<ParseDiagnostic> diagnostic = projectXMLSubtree(mutation.node)) {
        return diagnostic;
      }
      MarkSubtreeReplaced(handle);
      return std::nullopt;

    case xml::XMLMutation::Kind::NodeInserted:
    case xml::XMLMutation::Kind::NodeRemoved:
      return ParseDiagnostic::Error("XML mutation kind is not implemented by SVGDocument",
                                    MutationRange(source(), mutation));
  }

  UTILS_UNREACHABLE();
}

std::optional<ParseDiagnostic> SVGDocument::projectXMLSubtree(const xml::XMLNode& node) {
  if (node.type() != xml::XMLNode::Type::Element) {
    return std::nullopt;
  }

  const EntityHandle handle = node.entityHandle();
  EnsureProjectedElementComponents(handle, node.tagName());

  SVGElement element(handle);
  for (const xml::XMLQualifiedNameRef& attributeName : node.attributes()) {
    std::optional<RcString> value = node.getAttribute(attributeName);
    if (!value.has_value()) {
      continue;
    }

    if (std::optional<ParseDiagnostic> diagnostic =
            element.setAttributeFromXMLMutation(attributeName, *value)) {
      diagnostic->range = AttributeRange(source(), node, attributeName);
      return diagnostic;
    }
  }

  for (std::optional<xml::XMLNode> child = node.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (std::optional<ParseDiagnostic> diagnostic = projectXMLSubtree(*child)) {
      return diagnostic;
    }
  }

  return std::nullopt;
}

}  // namespace donner::svg
