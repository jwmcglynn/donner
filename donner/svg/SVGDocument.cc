#include "donner/svg/SVGDocument.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "donner/base/element/ElementTraversalGenerators.h"
#include "donner/base/xml/XMLSourceStore.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/base/xml/components/TreeMutationContext.h"
#include "donner/base/xml/components/XMLDocumentContext.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/svg/ElementFactory.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGQuerySelector.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/FontPaintDependenciesComponent.h"
#include "donner/svg/components/FontResourceGraph.h"
#include "donner/svg/components/NodeLifetimeCollector.h"
#include "donner/svg/components/NodeLifetimeComponent.h"
#include "donner/svg/components/ParsedPayloadResourceBudget.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/ScopedRenderInvalidationRestore.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/TreeMutation.h"
#include "donner/svg/components/animation/AnimationStateComponent.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/animation/AnimationTimeline.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/SelectorContent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextInvalidation.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {

namespace {

SourceRange FallbackRange();

bool CheckedAddProjectionSize(std::size_t& total, std::size_t value) {
  if (value > std::numeric_limits<std::size_t>::max() - total) {
    return false;
  }
  total += value;
  return true;
}

std::optional<std::size_t> ProjectedContentBytes(std::size_t contentBytes, std::size_t chunkCount) {
  constexpr std::size_t kEstimatedBytesPerChunk = 64;
  if (chunkCount > std::numeric_limits<std::size_t>::max() / kEstimatedBytesPerChunk) {
    return std::nullopt;
  }
  if (!CheckedAddProjectionSize(contentBytes, chunkCount * kEstimatedBytesPerChunk)) {
    return std::nullopt;
  }
  return contentBytes;
}

std::optional<std::size_t> AttributePayloadBytes(const xml::XMLNode& node) {
  std::size_t sourceBytes = 0;
  std::size_t attributeCount = 0;
  for (const xml::XMLQualifiedNameRef& name : node.attributes()) {
    const std::optional<RcString> value = node.getAttribute(name);
    if (!value.has_value() || !CheckedAddProjectionSize(sourceBytes, name.namespacePrefix.size()) ||
        !CheckedAddProjectionSize(sourceBytes, name.name.size()) ||
        !CheckedAddProjectionSize(sourceBytes, value->size())) {
      return std::nullopt;
    }
    ++attributeCount;
  }
  return components::ParsedPayloadResourceBudget::estimateAttributeBytes(sourceBytes,
                                                                         attributeCount);
}

std::optional<ParseDiagnostic> ReserveProjectedAttributes(EntityHandle handle,
                                                          const xml::XMLNode& node) {
  auto& budget = handle.registry()->ctx().get<components::ParsedPayloadResourceBudget>();
  const std::optional<std::size_t> bytes = AttributePayloadBytes(node);
  if (bytes.has_value() &&
      budget.reserve(handle.entity(), *bytes,
                     components::ParsedPayloadResourceBudget::Category::Attribute)) {
    return std::nullopt;
  }
  if (!bytes.has_value()) {
    budget.recordRejection();
  }
  return ParseDiagnostic::Error("Attributes exceed the document parsed-payload budget",
                                node.getNodeLocation().value_or(FallbackRange()));
}

std::optional<std::size_t> StylesheetPayloadBytes(std::size_t projectedSourceBytes,
                                                  const css::Stylesheet::SecurityStats& stats) {
  constexpr std::size_t kEstimatedBytesPerComponentValue = 64;
  constexpr std::size_t kEstimatedBytesPerDeclaration = 128;
  constexpr std::size_t kEstimatedBytesPerRule = 128;
  constexpr std::size_t kMaximum = std::numeric_limits<std::size_t>::max();
  if (stats.componentValues > kMaximum / kEstimatedBytesPerComponentValue ||
      stats.declarations > kMaximum / kEstimatedBytesPerDeclaration ||
      stats.rules > kMaximum / kEstimatedBytesPerRule ||
      !CheckedAddProjectionSize(projectedSourceBytes,
                                stats.componentValues * kEstimatedBytesPerComponentValue) ||
      !CheckedAddProjectionSize(projectedSourceBytes,
                                stats.declarations * kEstimatedBytesPerDeclaration) ||
      !CheckedAddProjectionSize(projectedSourceBytes, stats.rules * kEstimatedBytesPerRule)) {
    return std::nullopt;
  }
  return projectedSourceBytes;
}

void MarkStylesheetChanged(EntityHandle handle);
std::optional<ParseDiagnostic> ProjectStyleContents(EntityHandle handle, const xml::XMLNode& node);

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

/// The element whose text a NodeValueChanged mutation edits: the mutated element itself, or the
/// parent element of a mutated text or CDATA node.
std::optional<xml::XMLNode> NodeValueMutationElement(const xml::XMLMutation& mutation) {
  if (mutation.node.type() == xml::XMLNode::Type::Element) {
    return mutation.node;
  }
  if (mutation.node.type() == xml::XMLNode::Type::Data ||
      mutation.node.type() == xml::XMLNode::Type::CData) {
    return mutation.node.parentElement();
  }
  return std::nullopt;
}

/// Whether \p handle is a `<title>`, `<desc>` or `<metadata>` element, whose text only the parser
/// projects.
bool IsDescriptiveElement(EntityHandle handle) {
  const auto* elementType = handle.try_get<components::ElementTypeComponent>();
  if (elementType == nullptr) {
    return false;
  }
  switch (elementType->type()) {
    case ElementType::Title:
    case ElementType::Desc:
    case ElementType::Metadata: return true;
    default: return false;
  }
}

/// Whether \p node is a `<title>`, `<desc>` or `<metadata>` element whose text the parser that
/// built the document can parse again.
bool CanReparseDescriptiveText(const xml::XMLNode& node) {
  const EntityHandle handle = node.entityHandle();
  return handle && IsDescriptiveElement(handle) &&
         static_cast<bool>(handle.registry()
                               ->ctx()
                               .get<components::SVGDocumentContext>()
                               .parseEditedElementContents);
}

std::optional<EntityHandle> TextElementHandleForNodeValueMutation(
    const xml::XMLMutation& mutation) {
  if (std::optional<xml::XMLNode> element = NodeValueMutationElement(mutation)) {
    return element->entityHandle();
  }
  return std::nullopt;
}

std::optional<ParseDiagnostic> ApplyNodeValueChanged(std::string_view source,
                                                     const xml::XMLMutation& mutation) {
  if (!mutation.value.has_value()) {
    return ParseDiagnostic::Error("XML NodeValueChanged mutation is missing a value",
                                  MutationRange(source, mutation));
  }

  std::optional<xml::XMLNode> targetNode;
  if (mutation.node.type() == xml::XMLNode::Type::Element) {
    targetNode = mutation.node;
  } else if (mutation.node.type() == xml::XMLNode::Type::Data ||
             mutation.node.type() == xml::XMLNode::Type::CData) {
    targetNode = mutation.node.parentElement();
  }

  std::optional<EntityHandle> targetHandle = TextElementHandleForNodeValueMutation(mutation);
  if (!targetHandle.has_value() || !*targetHandle) {
    return ParseDiagnostic::Error(
        "XML NodeValueChanged mutation target is not SVG text or style content",
        MutationRange(source, mutation));
  }

  if (targetHandle->all_of<components::TextComponent>()) {
    const std::optional<std::size_t> projectedBytes =
        ProjectedContentBytes(mutation.value->size(), 1);
    auto& budget = targetHandle->registry()->ctx().get<components::ParsedPayloadResourceBudget>();
    if (!projectedBytes.has_value() ||
        !budget.reserve(targetHandle->entity(), *projectedBytes,
                        components::ParsedPayloadResourceBudget::Category::ProjectedText)) {
      if (!projectedBytes.has_value()) {
        budget.recordRejection();
      }
      return ParseDiagnostic::Error("Text content exceeds the document parsed-payload budget",
                                    MutationRange(source, mutation));
    }
    auto& text = targetHandle->get<components::TextComponent>();
    text.text = *mutation.value;
    text.textChunks.clear();
    text.textChunks.emplace_back(*mutation.value);
    (void)components::InvalidateTextLayout(*targetHandle);
    return std::nullopt;
  }

  if (targetHandle->all_of<components::StylesheetComponent>() && targetNode.has_value()) {
    if (std::optional<ParseDiagnostic> diagnostic =
            ProjectStyleContents(*targetHandle, *targetNode)) {
      return diagnostic;
    }

    MarkStylesheetChanged(*targetHandle);
    return std::nullopt;
  }

  return ParseDiagnostic::Error(
      "XML NodeValueChanged mutation target is not SVG text or style content",
      MutationRange(source, mutation));
}

bool IsNamespaceDeclaration(const xml::XMLQualifiedNameRef& name) {
  return name.namespacePrefix == "xmlns" || name == xml::XMLQualifiedNameRef("xmlns");
}

/// Element type of a projected node, and whether this projection just built its SVG element.
struct ProjectedElement {
  ElementType type = ElementType::Unknown;  //!< Retained element type.
  bool builtTypedElement = false;           //!< True for a new or promoted element of a known type.
};

/**
 * Build the SVG element for \p node the way the parser does, or update an existing element's
 * namespace membership.
 *
 * A new node gets the element type its tag names in the SVG namespace, or an unknown element. An
 * existing unknown element is rebuilt in place as that type once its tag names one in the SVG
 * namespace, such as after an `xmlns` edit; its entity and XML node keep their identity. A typed
 * element keeps its type when it leaves the SVG namespace.
 *
 * @param handle Entity of \p node.
 * @param node XML element to project.
 */
ProjectedElement BuildProjectedElement(EntityHandle handle, const xml::XMLNode& node) {
  const xml::XMLQualifiedName tagName(node.tagName());
  const bool isSvgNamespace =
      node.getNamespaceUri(tagName.namespacePrefix) == "http://www.w3.org/2000/svg";
  if (auto* current = handle.try_get<components::ElementTypeComponent>();
      current != nullptr && current->parsedType() != ElementType::Unknown) {
    current->setInSvgNamespace(isSvgNamespace);
    return {.type = current->parsedType()};
  }

  if (isSvgNamespace) {
    ProjectedElement built;
    const bool experimentalElementsEnabled =
        handle.registry()->ctx().get<components::SVGDocumentContext>().experimentalElementsEnabled;
    if (details::ElementFactory::VisitType(
            std::string_view(tagName.name), experimentalElementsEnabled, [&]<typename ElementT>() {
              // Remove the unknown element's own components; the factory adds what its type needs.
              handle.remove<components::ElementTypeComponent, components::TransformComponent,
                            components::RenderingBehaviorComponent>();
              (void)details::ElementFactory::CreateOn<ElementT>(handle);
              built = {.type = ElementT::Type, .builtTypedElement = true};
            })) {
      return built;
    }
  }

  if (!handle.all_of<components::ElementTypeComponent>()) {
    (void)details::ElementFactory::CreateUnknownOn(handle, tagName);
  }
  handle.get<components::ElementTypeComponent>().setInSvgNamespace(isSvgNamespace);
  return {.type = ElementType::Unknown};
}

void UpdateNamespaceMembership(const xml::XMLNode& node) {
  if (node.type() != xml::XMLNode::Type::Element) {
    return;
  }
  if (auto* type = node.entityHandle().try_get<components::ElementTypeComponent>()) {
    const xml::XMLQualifiedName name(node.tagName());
    type->setInSvgNamespace(node.getNamespaceUri(name.namespacePrefix) ==
                            "http://www.w3.org/2000/svg");
  }
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    UpdateNamespaceMembership(*child);
  }
}

ProjectedElement EnsureProjectedElementComponents(EntityHandle handle, const xml::XMLNode& node) {
  const ProjectedElement projected = BuildProjectedElement(handle, node);
  if (projected.type == ElementType::Style) {
    // The parser creates the stylesheet when it parses the contents; style projection updates it.
    [[maybe_unused]] auto& stylesheet = handle.get_or_emplace<components::StylesheetComponent>();
  }
  return projected;
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

void MarkStylesheetChanged(EntityHandle handle) {
  Registry& registry = *handle.registry();
  if (auto* budget = registry.ctx().find<components::StyleResourceBudget>()) {
    budget->releaseAll();
  }
  registry.clear<components::ComputedStyleComponent>();

  components::RenderTreeState& renderState = GetRenderTreeState(handle);
  renderState.needsFullRebuild = true;
  renderState.needsFullStyleRecompute = true;
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

void MarkChildRemoved(EntityHandle parentHandle) {
  components::RenderTreeState& renderState = GetRenderTreeState(parentHandle);
  renderState.needsFullRebuild = true;
  // Removing a child changes structural selector matches (:nth-child, :empty, :first-child and
  // the sibling combinators) for the elements that remain. Per-entity dirty flags do not track
  // those non-local dependencies, so the whole tree has to be restyled.
  renderState.needsFullStyleRecompute = true;

  parentHandle.get_or_emplace<components::DirtyFlagsComponent>().mark(
      components::DirtyFlagsComponent::All);
}

void MarkDirtySubtree(EntityHandle handle, uint16_t flags) {
  handle.get_or_emplace<components::DirtyFlagsComponent>().mark(flags);

  donner::components::ForAllChildrenRecursive(handle, [flags](EntityHandle descendant) {
    descendant.get_or_emplace<components::DirtyFlagsComponent>().mark(flags);
  });
}

void MarkChildInserted(EntityHandle parentHandle, EntityHandle childHandle) {
  components::RenderTreeState& renderState = GetRenderTreeState(parentHandle);
  renderState.needsFullRebuild = true;
  renderState.needsFullStyleRecompute = true;

  parentHandle.get_or_emplace<components::DirtyFlagsComponent>().mark(
      components::DirtyFlagsComponent::All);
  MarkDirtySubtree(childHandle, components::DirtyFlagsComponent::All);
}

SourceRange FallbackRange() {
  return SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)};
}

xml::XMLNode EnsureXMLSubtreeForSVGElement(xml::XMLDocument& document, const SVGElement& element) {
  std::optional<xml::XMLNode> node = xml::XMLNode::TryCast(element.entityHandle());
  const bool createMissingDescendants = !node.has_value();
  if (!node.has_value()) {
    node = xml::XMLNode::CreateElementNodeOn(document, element.entityHandle(), element.tagName());
  }

  for (std::optional<SVGElement> child = element.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (createMissingDescendants || xml::XMLNode::TryCast(child->entityHandle()).has_value()) {
      (void)EnsureXMLSubtreeForSVGElement(document, *child);
    }
  }

  return *node;
}

std::optional<ParseDiagnostic> ProjectTextContents(EntityHandle handle, const xml::XMLNode& node) {
  if (!handle.all_of<components::TextComponent>()) {
    return std::nullopt;
  }

  std::size_t contentBytes = 0;
  std::size_t chunkCount = 0;
  bool foundContentChild = false;
  const std::size_t maximumChunks =
      handle.registry()->ctx().get<components::SVGDocumentContext>().maximumContentProjectionChunks;
  for (std::optional<xml::XMLNode> child = node.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (child->type() == xml::XMLNode::Type::Data || child->type() == xml::XMLNode::Type::CData) {
      foundContentChild = true;
      ++chunkCount;
      if (const auto value = child->value();
          value && !CheckedAddProjectionSize(contentBytes, value->size())) {
        return ParseDiagnostic::Error("Text content size overflow",
                                      node.getNodeLocation().value_or(FallbackRange()));
      }
    } else if (child->type() == xml::XMLNode::Type::Element) {
      ++chunkCount;
    }
    if (chunkCount > maximumChunks) {
      return ParseDiagnostic::Error("Text content has too many chunks",
                                    node.getNodeLocation().value_or(FallbackRange()));
    }
  }
  if (!foundContentChild) {
    if (const std::optional<RcString> value = node.value(); value.has_value()) {
      contentBytes = value->size();
      if (!value->empty()) {
        ++chunkCount;
      }
    }
  }
  if (chunkCount > maximumChunks) {
    return ParseDiagnostic::Error("Text content has too many chunks",
                                  node.getNodeLocation().value_or(FallbackRange()));
  }

  const std::optional<std::size_t> projectedBytes = ProjectedContentBytes(contentBytes, chunkCount);
  auto& budget = handle.registry()->ctx().get<components::ParsedPayloadResourceBudget>();
  if (!projectedBytes.has_value() ||
      !budget.reserve(handle.entity(), *projectedBytes,
                      components::ParsedPayloadResourceBudget::Category::ProjectedText)) {
    if (!projectedBytes.has_value()) {
      budget.recordRejection();
    }
    return ParseDiagnostic::Error("Text content exceeds the document parsed-payload budget",
                                  node.getNodeLocation().value_or(FallbackRange()));
  }

  std::string combined;
  combined.reserve(contentBytes);
  SmallVector<RcString, 1> textChunks;

  for (std::optional<xml::XMLNode> child = node.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (child->type() == xml::XMLNode::Type::Data || child->type() == xml::XMLNode::Type::CData) {
      const RcString value = child->value().value_or(RcString(""));
      combined.append(value.data(), value.size());

      if (textChunks.empty()) {
        textChunks.emplace_back(value);
      } else if (textChunks.back().empty()) {
        textChunks.back() = value;
      } else {
        textChunks.emplace_back(value);
      }
    } else if (child->type() == xml::XMLNode::Type::Element) {
      if (textChunks.empty()) {
        textChunks.emplace_back(RcString(""));
      }
      textChunks.emplace_back(RcString(""));
    }
  }
  if (!foundContentChild) {
    const RcString value = node.value().value_or(RcString(""));
    combined.append(value.data(), value.size());
    if (!value.empty()) {
      textChunks.emplace_back(value);
    }
  }

  auto& text = handle.get<components::TextComponent>();
  text.text = RcString(combined);
  text.textChunks = std::move(textChunks);
  return std::nullopt;
}

std::optional<ParseDiagnostic> ProjectStyleContents(EntityHandle handle, const xml::XMLNode& node) {
  if (!handle.all_of<components::ElementTypeComponent>() ||
      handle.get<components::ElementTypeComponent>().type() != ElementType::Style) {
    return std::nullopt;
  }

  auto* stylesheet = handle.try_get<components::StylesheetComponent>();
  if (stylesheet == nullptr) {
    return std::nullopt;
  }
  auto& budget = handle.registry()->ctx().get<components::ParsedPayloadResourceBudget>();
  if (!stylesheet->isCssType()) {
    (void)budget.reserve(handle.entity(), 0,
                         components::ParsedPayloadResourceBudget::Category::Stylesheet);
    return std::nullopt;
  }

  std::size_t contentBytes = 0;
  std::size_t chunkCount = 0;
  bool foundContentChild = false;
  const std::size_t maximumChunks =
      handle.registry()->ctx().get<components::SVGDocumentContext>().maximumContentProjectionChunks;
  for (std::optional<xml::XMLNode> child = node.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (child->type() == xml::XMLNode::Type::Data || child->type() == xml::XMLNode::Type::CData) {
      foundContentChild = true;
      ++chunkCount;
      if (const auto value = child->value();
          value && !CheckedAddProjectionSize(contentBytes, value->size())) {
        return ParseDiagnostic::Error("Stylesheet content size overflow", FileOffset::Offset(0));
      }
      if (chunkCount > maximumChunks) {
        return ParseDiagnostic::Error("Stylesheet has too many content chunks",
                                      FileOffset::Offset(0));
      }
    }
  }
  if (!foundContentChild) {
    if (const std::optional<RcString> value = node.value(); value.has_value()) {
      contentBytes = value->size();
      chunkCount = value->empty() ? 0 : 1;
    }
  }
  if (chunkCount > maximumChunks) {
    return ParseDiagnostic::Error("Stylesheet has too many content chunks",
                                  node.getNodeLocation().value_or(FallbackRange()));
  }

  const std::optional<std::size_t> projectedSourceBytes =
      ProjectedContentBytes(contentBytes, chunkCount);
  const std::optional<std::size_t> preflightBytes =
      projectedSourceBytes.has_value()
          ? components::ParsedPayloadResourceBudget::estimateStylesheetPreflightBytes(
                contentBytes, *projectedSourceBytes)
          : std::nullopt;
  if (!preflightBytes.has_value() ||
      !budget.canReserve(handle.entity(), *preflightBytes,
                         components::ParsedPayloadResourceBudget::Category::Stylesheet)) {
    budget.recordRejection();
    return ParseDiagnostic::Error("Stylesheet exceeds the document parsed-payload budget",
                                  node.getNodeLocation().value_or(FallbackRange()));
  }

  std::string combined;
  combined.reserve(contentBytes);
  components::StylesheetSourceMap sourceMap;
  bool foundTextChild = false;
  for (std::optional<xml::XMLNode> child = node.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (child->type() == xml::XMLNode::Type::Data || child->type() == xml::XMLNode::Type::CData) {
      foundTextChild = true;
      if (std::optional<RcString> value = child->value()) {
        const std::size_t cssStartOffset = combined.size();
        combined += *value;
        const std::size_t cssEndOffset = combined.size();
        std::optional<SourceRange> childValueLocation = child->getValueLocation();
        if (!childValueLocation.has_value() && child->type() == xml::XMLNode::Type::Data) {
          childValueLocation = child->getNodeLocation();
        }

        if (childValueLocation.has_value() && childValueLocation->start.offset.has_value() &&
            childValueLocation->end.offset.has_value() &&
            *childValueLocation->end.offset >= *childValueLocation->start.offset &&
            *childValueLocation->end.offset - *childValueLocation->start.offset == value->size()) {
          sourceMap.addSegment(cssStartOffset, cssEndOffset, childValueLocation->start);
        }
      }
    } else {
      return ParseDiagnostic::Error(
          "Unexpected <style> element contents",
          child->getNodeLocation().value_or(node.getNodeLocation().value_or(
              SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)})));
    }
  }

  if (!foundTextChild) {
    combined = node.value().value_or(RcString(""));
    // SVG style projection stores its text on the element after the original XML Data children
    // have been consumed. A source edit therefore reparses the element value directly. Retain
    // that value's source span so style focus and reference ropes can still find its rules.
    if (const std::optional<SourceRange> valueLocation = node.getValueLocation();
        valueLocation.has_value() && valueLocation->start.offset.has_value() &&
        valueLocation->end.offset.has_value() &&
        *valueLocation->end.offset >= *valueLocation->start.offset &&
        *valueLocation->end.offset - *valueLocation->start.offset == combined.size()) {
      sourceMap.addSegment(0, combined.size(), valueLocation->start);
    }
  }
  sourceMap.setCssLength(combined.size());

  components::StylesheetComponent projected;
  projected.type = stylesheet->type;
  projected.isUserAgentStylesheet = stylesheet->isUserAgentStylesheet;
  projected.parseStylesheet(std::string_view(combined), std::move(sourceMap));
  const std::optional<std::size_t> retainedBytes =
      StylesheetPayloadBytes(*projectedSourceBytes, projected.stylesheet.securityStats());
  if (!retainedBytes.has_value() ||
      !budget.reserve(handle.entity(), *retainedBytes,
                      components::ParsedPayloadResourceBudget::Category::Stylesheet)) {
    if (!retainedBytes.has_value()) {
      budget.recordRejection();
    }
    return ParseDiagnostic::Error("Stylesheet exceeds the document parsed-payload budget",
                                  node.getNodeLocation().value_or(FallbackRange()));
  }
  *stylesheet = std::move(projected);
  return std::nullopt;
}

bool IsAttributeMutation(xml::XMLMutation::Kind kind) {
  return kind == xml::XMLMutation::Kind::AttributeSet ||
         kind == xml::XMLMutation::Kind::AttributeRemoved;
}

}  // namespace

SVGDocument::SVGDocument(SVGDocumentHandle documentState, Settings settings,
                         EntityHandle ontoEntityHandle)
    : documentState_(std::move(documentState)) {
  Registry& registry = documentState_->registry();
  // A document built on an XML tree takes its source store holder from the XML context now, while
  // this thread is the only one that can reach the registry; see DocumentState::sourceStoreHolder.
  if (const auto* xmlContext = registry.ctx().find<xml::components::XMLDocumentContext>()) {
    documentState_->setSourceStoreHolder(xmlContext->sourceStoreHolder);
  }
  std::shared_ptr<components::DocumentResourceFamilyBudget> resourceFamily =
      settings.resourceFamilyBudget;
  if (!resourceFamily) {
    if (const auto* existing = registry.ctx().find<components::DocumentResourceFamilyContext>()) {
      resourceFamily = existing->budget;
    } else {
      resourceFamily = std::make_shared<components::DocumentResourceFamilyBudget>();
    }
  }
  if (!registry.ctx().contains<components::DocumentResourceFamilyContext>()) {
    registry.ctx().emplace<components::DocumentResourceFamilyContext>(resourceFamily);
  }
  if (!registry.ctx().contains<components::ParsedPayloadResourceBudget>()) {
    registry.ctx().emplace<components::ParsedPayloadResourceBudget>(
        components::ParsedPayloadResourceBudget::Limits{}, resourceFamily);
  }
  // TreeMutationContext is now always installed (XMLDocument's ctor installs the basic-XML
  // defaults when the registry is created via the XML path); on a fresh SVG-only registry we
  // install it here, then override the individual callbacks with the SVG-specific implementations
  // that layer invalidation and lifetime tracking on top of the tree mutations.
  if (!registry.ctx().contains<donner::components::TreeMutationContext>()) {
    registry.ctx().emplace<donner::components::TreeMutationContext>();
  }
  auto& treeMutations = registry.ctx().get<donner::components::TreeMutationContext>();
  treeMutations.insertBefore = components::TreeMutation::InsertBefore;
  treeMutations.appendChild = components::TreeMutation::AppendChild;
  treeMutations.replaceChild = components::TreeMutation::ReplaceChild;
  treeMutations.removeChild = components::TreeMutation::RemoveChild;
  treeMutations.remove = components::TreeMutation::Remove;
  treeMutations.setValue = components::TreeMutation::SetValue;

  auto& ctx = registry.ctx().emplace<components::SVGDocumentContext>(
      components::SVGDocumentContext::InternalCtorTag{}, documentState_);
  if (ontoEntityHandle) {
    ctx.rootEntity = SVGSVGElement::CreateOn(ontoEntityHandle).unsafeEntityHandle().entity();
  } else {
    ctx.rootEntity = SVGSVGElement::Create(*this).unsafeEntityHandle().entity();
  }
  registry.get_or_emplace<components::NodeLifetimeComponent>(ctx.rootEntity).markAttached();

  components::ResourceManagerContext& resourceCtx =
      registry.ctx().emplace<components::ResourceManagerContext>(registry);
  resourceCtx.setResourceLoader(std::move(settings.resourceLoader));
  resourceCtx.setProcessingMode(settings.processingMode);
  if (settings.svgParseCallback) {
    resourceCtx.setSvgParseCallback(std::move(settings.svgParseCallback));
  }

  registry.ctx().emplace<xml::components::XMLNamespaceContext>(registry);
}

SVGDocument::SVGDocument() : SVGDocument(Settings()) {}

SVGDocument::SVGDocument(Settings settings)
    : SVGDocument(std::make_shared<DocumentState>(), std::move(settings), EntityHandle()) {}

SVGDocumentMutation::SVGDocumentMutation(SVGDocument document, DocumentWriteAccess& access)
    : document_(std::move(document)), access_(&access) {}

DocumentWriteAccess& SVGDocumentMutation::access() const {
  return *access_;
}

void SVGDocumentMutation::setCanvasSize(int width, int height) {
  document_.setCanvasSize(width, height);
}

void SVGDocumentMutation::useAutomaticCanvasSize() {
  document_.useAutomaticCanvasSize();
}

void SVGDocumentMutation::setAttribute(SVGElement element, const xml::XMLQualifiedNameRef& name,
                                       std::string_view value) {
  element.setAttribute(name, value);
}

void SVGDocumentMutation::removeAttribute(SVGElement element,
                                          const xml::XMLQualifiedNameRef& name) {
  element.removeAttribute(name);
}

void SVGDocumentMutation::insertBefore(SVGElement parent, const SVGElement& newNode,
                                       std::optional<SVGElement> referenceNode) {
  parent.insertBefore(newNode, std::move(referenceNode));
}

void SVGDocumentMutation::appendChild(SVGElement parent, const SVGElement& child) {
  parent.appendChild(child);
}

void SVGDocumentMutation::replaceChild(SVGElement parent, const SVGElement& newChild,
                                       const SVGElement& oldChild) {
  parent.replaceChild(newChild, oldChild);
}

void SVGDocumentMutation::removeChild(SVGElement parent, const SVGElement& child) {
  parent.removeChild(child);
}

void SVGDocumentMutation::remove(SVGElement element) {
  element.remove();
}

EntityHandle SVGDocument::rootEntityHandle() const {
  DocumentReadAccess access = documentState_->read();
  Registry& registry = access.registry();
  return EntityHandle(registry, registry.ctx().get<components::SVGDocumentContext>().rootEntity);
}

SVGSVGElement SVGDocument::svgElement() const {
  return SVGSVGElement(rootEntityHandle());
}

void SVGDocument::setCanvasSize(int width, int height) {
  assert(width > 0 && height > 0);
  DocumentMutationBatch mutation(*documentState_, true);
  DocumentWriteAccess& access = mutation.access();
  Registry& registry = access.registry();
  auto& documentContext = registry.ctx().get<components::SVGDocumentContext>();
  if (documentContext.canvasSize == Vector2i(width, height)) {
    // No-op when the stored explicit canvas size is unchanged: do not
    // invalidate the render tree (that invalidation cascades into a full
    // restyle + render-tree rebuild + full recompose on the next rendered
    // frame) and do not commit a mutation revision. Callers (e.g. per-frame
    // viewport sync) cannot cheaply detect this themselves: the canvasSize()
    // getter returns the *derived* canvas-scaled document size, which does
    // not round-trip with the value stored here.
    mutation.cancel();
    return;
  }
  components::RenderingContext(registry).invalidateRenderTree();
  documentContext.canvasSize = Vector2i(width, height);
}

void SVGDocument::setTime(double seconds) {
  if (!std::isfinite(seconds)) {
    return;
  }
  DocumentMutationBatch mutation(*documentState_, true);
  DocumentWriteAccess& access = mutation.access();
  Registry& registry = access.registry();
  auto& documentContext = registry.ctx().get<components::SVGDocumentContext>();
  if (documentContext.documentTime == seconds) {
    // No-op when the time is unchanged: avoid a spurious render-tree invalidation and mutation
    // revision bump.
    mutation.cancel();
    return;
  }
  components::RenderingContext(registry).invalidateRenderTree();
  documentContext.documentTime = seconds;
}

bool SVGDocument::dispatchMouseEvent(EventType type, const SVGElement& target) {
  std::ostringstream name;
  name << type;
  return dispatchAnimationEvent(name.str(), target, eventBubbles(type));
}

bool SVGDocument::dispatchAnimationEvent(std::string_view name, const SVGElement& target,
                                         bool bubbles) {
  DocumentMutationBatch mutation(*documentState_, true);
  Registry& registry = mutation.access().registry();
  if (target.handle_.unsafeRegistry() != &registry) {
    mutation.cancel();
    return false;
  }
  const EntityHandle handle = target.unsafeEntityHandle();
  if (!handle.valid() ||
      registry.get<components::NodeLifetimeComponent>(handle.entity()).treeState !=
          components::NodeLifetimeComponent::TreeState::Attached ||
      registry.ctx().get<components::ResourceManagerContext>().processingMode() !=
          ProcessingMode::DynamicInteractive ||
      !components::AnimationSystem().dispatchEvent(
          registry, handle.entity(), name, bubbles,
          registry.ctx().get<components::SVGDocumentContext>().documentTime)) {
    mutation.cancel();
    return false;
  }
  components::RenderingContext(registry).invalidateRenderTree();
  return true;
}

bool SVGDocument::beginAnimation(const SVGElement& animation, double offset) {
  return scheduleAnimationBoundary(animation, offset, true);
}

bool SVGDocument::endAnimation(const SVGElement& animation, double offset) {
  return scheduleAnimationBoundary(animation, offset, false);
}

namespace {

bool IsAttachedAnimation(EntityHandle handle) {
  return handle.valid() && handle.all_of<components::AnimationTimingComponent>() &&
         handle.get<components::NodeLifetimeComponent>().treeState ==
             components::NodeLifetimeComponent::TreeState::Attached;
}

bool RecordAnimationBoundary(components::AnimationStateComponent* state, bool begin, double time) {
  return state && (begin || state->phase == components::AnimationPhase::Active) &&
         components::InsertAnimationInstance(begin ? state->eventBeginTimes : state->eventEndTimes,
                                             time);
}

bool ValidAnimationRepeatStep(double index, double time, double previous, std::size_t steps) {
  return std::isfinite(index) && index < 9007199254740992.0 && time > previous && steps <= 4096;
}

class AnimationEventCollector {
public:
  AnimationEventCollector(double from, double to) : from_(from), to_(to) {}

  bool interval(const SVGElement& animation, const components::AnimationInterval& interval,
                double duration) {
    return add(animation, SVGAnimationEvent::Type::Begin, interval.begin) &&
           add(animation, SVGAnimationEvent::Type::End, interval.end) &&
           repeats(animation, interval, duration);
  }

  SVGAnimationEventBatch finish() {
    std::stable_sort(events_.begin(), events_.end(),
                     [](const auto& a, const auto& b) { return a.time < b.time; });
    return {std::move(events_), true};
  }

private:
  bool add(const SVGElement& animation, SVGAnimationEvent::Type type, double time,
           uint64_t repeat = 0) {
    if (time <= from_ || time > to_ || !std::isfinite(time)) {
      return true;
    }
    if (events_.size() >= 4096) {
      return false;
    }
    events_.push_back({animation, type, time, repeat});
    return true;
  }

  bool repeats(const SVGElement& animation, const components::AnimationInterval& interval,
               double duration) {
    if (!(duration > 0) || !std::isfinite(duration) || to_ <= interval.begin) {
      return true;
    }
    const double end = std::min(interval.end, interval.begin + interval.repeatDuration);
    if (from_ >= end) {
      return true;
    }
    double index = std::max(1.0, std::floor((from_ - interval.begin) / duration) + 1);
    if (!std::isfinite(index) || index >= 9007199254740992.0) {
      return false;
    }
    double previous = from_;
    std::size_t steps = 0;
    for (double time = interval.begin + index * duration; time <= to_ && time < end;
         time = interval.begin + (++index) * duration) {
      if (!ValidAnimationRepeatStep(index, time, previous, ++steps) ||
          !add(animation, SVGAnimationEvent::Type::Repeat, time, static_cast<uint64_t>(index))) {
        return false;
      }
      previous = time;
    }
    return true;
  }

  double from_;
  double to_;
  std::vector<SVGAnimationEvent> events_;
};

}  // namespace

bool SVGDocument::scheduleAnimationBoundary(const SVGElement& animation, double offset,
                                            bool begin) {
  DocumentMutationBatch mutation(*documentState_, true);
  Registry& registry = mutation.access().registry();
  if (!std::isfinite(offset) || animation.handle_.unsafeRegistry() != &registry ||
      registry.ctx().get<components::ResourceManagerContext>().processingMode() !=
          ProcessingMode::DynamicInteractive) {
    mutation.cancel();
    return false;
  }
  const EntityHandle handle = animation.unsafeEntityHandle();
  if (!IsAttachedAnimation(handle)) {
    mutation.cancel();
    return false;
  }
  const double time = registry.ctx().get<components::SVGDocumentContext>().documentTime;
  const auto animations = components::CollectAnimationEntities(
      registry, components::AnimationCollectionMode::DiscardExcludedState);
  if (std::find(animations.begin(), animations.end(), handle.entity()) == animations.end() ||
      !components::ResolveAnimationTimeline(registry, animations, time)) {
    mutation.cancel();
    return false;
  }
  auto* state = registry.try_get<components::AnimationStateComponent>(handle.entity());
  if (!RecordAnimationBoundary(state, begin, time + offset)) {
    mutation.cancel();
    return false;
  }
  components::RenderingContext(registry).invalidateRenderTree();
  return true;
}

SVGAnimationEventBatch SVGDocument::animationEvents(double from, double to) {
  if (!std::isfinite(from) || !std::isfinite(to) || from > to) {
    return {{}, false};
  }
  DocumentWriteAccess access = documentState_->write();
  Registry& registry = access.registry();
  if (registry.ctx().get<components::ResourceManagerContext>().processingMode() ==
      ProcessingMode::SecureStatic) {
    return {};
  }
  const auto animations = components::CollectAnimationEntities(
      registry, components::AnimationCollectionMode::DiscardExcludedState);
  if (!components::ResolveAnimationTimeline(
          registry, animations,
          registry.ctx().get<components::SVGDocumentContext>().documentTime)) {
    return {{}, false};
  }
  AnimationEventCollector collector(from, to);
  for (Entity entity : animations) {
    const auto& state = registry.get<components::AnimationStateComponent>(entity);
    for (const auto& interval : state.intervals) {
      if (!collector.interval(SVGElement::CreateFromEntityHandle(EntityHandle(registry, entity)),
                              interval, state.simpleDuration)) {
        return {{}, false};
      }
    }
  }
  return collector.finish();
}

void SVGDocument::restartAnimations() {
  DocumentMutationBatch mutation(*documentState_, true);
  Registry& registry = mutation.access().registry();
  registry.clear<components::AnimationStateComponent>();
  registry.ctx().get<components::SVGDocumentContext>().documentTime = 0.0;
  components::RenderingContext(registry).invalidateRenderTree();
}

double SVGDocument::currentTime() const {
  DocumentReadAccess access = documentState_->read();
  return access.registry().ctx().get<components::SVGDocumentContext>().documentTime;
}

Transform2d SVGDocument::canvasFromDocumentTransform() const {
  DocumentReadAccess access = documentState_->read();
  return components::LayoutSystem().getCanvasFromDocumentTransform(access.registry());
}

void SVGDocument::setUserLanguages(std::vector<RcString> languages) {
  DocumentMutationBatch mutation(*documentState_, true);
  DocumentWriteAccess& access = mutation.access();
  Registry& registry = access.registry();
  auto& documentContext = registry.ctx().get<components::SVGDocumentContext>();
  if (documentContext.userLanguages == languages) {
    mutation.cancel();
    return;
  }
  // Changing the language list changes which conditional-processing branches render, so the render
  // tree (and any downstream text layout) must be rebuilt.
  components::RenderingContext(registry).invalidateRenderTree();
  documentContext.userLanguages = std::move(languages);
}

std::vector<RcString> SVGDocument::userLanguages() const {
  DocumentReadAccess access = documentState_->read();
  return access.registry().ctx().get<components::SVGDocumentContext>().userLanguages;
}

void SVGDocument::useAutomaticCanvasSize() {
  DocumentMutationBatch mutation(*documentState_, true);
  DocumentWriteAccess& access = mutation.access();
  Registry& registry = access.registry();
  components::RenderingContext(registry).invalidateRenderTree();
  registry.ctx().get<components::SVGDocumentContext>().canvasSize = std::nullopt;
}

Vector2i SVGDocument::canvasSize() const {
  DocumentReadAccess access = documentState_->read();
  return components::LayoutSystem().calculateCanvasScaledDocumentSize(
      access.registry(), components::LayoutSystem::InvalidSizeBehavior::ReturnDefault);
}

std::optional<Vector2i> SVGDocument::canvasSizeOverride() const {
  DocumentReadAccess access = documentState_->read();
  return access.registry().ctx().get<components::SVGDocumentContext>().canvasSize;
}

bool SVGDocument::operator==(const SVGDocument& other) const {
  return documentState_ == other.documentState_;
}

bool SVGDocument::hasPendingRenderInvalidation() const {
  [[maybe_unused]] DocumentReadAccess access = readAccess();
  const Registry& registry = documentState_->registry();
  const auto* state = registry.ctx().find<components::RenderTreeState>();
  if (state == nullptr || !state->hasBeenBuilt) {
    return false;
  }

  const auto dirtyView = registry.view<const components::DirtyFlagsComponent>();
  if (dirtyView.begin() != dirtyView.end()) {
    return true;
  }
  return state->needsFullRebuild || state->needsFullStyleRecompute;
}

std::size_t SVGDocument::elementCount() const {
  return withReadAccess([](DocumentReadAccess& access) {
    return access.registry().storage<components::ElementTypeComponent>().size();
  });
}

namespace {

static_assert(components::kMaximumFontChildDocuments ==
              components::SubDocumentCache::Limits{}.maximumDocuments);

struct FontRefreshTraversal {
  std::unordered_set<const DocumentState*> active;
  std::unordered_map<const DocumentState*, bool> refreshed;
  components::FontResourceGraphCache cache;
  explicit FontRefreshTraversal(components::FontResourceGraphCache::Stats& stats) : cache(&stats) {}
};

bool RefreshDocumentFonts(SVGDocument& document, FontRefreshTraversal& traversal);

bool UpdateChildFontSnapshot(SVGDocument& nested, components::ChildFontPaintDependencies& child,
                             FontRefreshTraversal& traversal) {
  const auto childAccess = nested.readAccess();
  const auto childHandle = nested.handle();
  auto collection = traversal.cache.collect(childHandle, child.target);
  auto rendered = traversal.cache.collect(childHandle, child.target,
                                          components::FontResourceGraph::Purpose::RenderedFrame);
  const uint64_t revision = nested.fontResourceRevision();
  // A child registry may serve several different referenced subtrees. Its global revision
  // alone does not make every host's pixels stale; compare the faces that this target used.
  const bool changed = child.fontDependencies != collection.dependencies ||
                       child.needsRender != collection.needsRender ||
                       child.resourceLimit != collection.resourceLimit ||
                       child.renderedFontDependencies != rendered.dependencies ||
                       child.renderedNeedsRender != rendered.needsRender ||
                       child.renderedResourceLimit != rendered.resourceLimit;
  child.renderedFontDependencies = std::move(rendered.dependencies);
  child.renderedNeedsRender = rendered.needsRender;
  child.renderedResourceLimit = rendered.resourceLimit;
  child.fontDependencies = std::move(collection.dependencies);
  child.fontResourceRevision = revision;
  child.needsRender = collection.needsRender;
  child.resourceLimit = collection.resourceLimit;
  return changed;
}

bool RefreshChildFontResource(components::ChildFontPaintDependencies& child,
                              FontRefreshTraversal& traversal) {
  const auto childHandle = child.document.lock();
  if (!childHandle || traversal.active.contains(childHandle.get()) ||
      (!traversal.refreshed.contains(childHandle.get()) &&
       traversal.refreshed.size() + traversal.active.size() >=
           components::kMaximumFontChildDocuments + 1)) {
    const bool changed = !child.resourceLimit;
    child.resourceLimit = true;
    return changed;
  }
  SVGDocument nested = SVGDocument::CreateFromHandle(childHandle);
  if (!traversal.refreshed.contains(childHandle.get())) {
    RefreshDocumentFonts(nested, traversal);
  }
  if (!traversal.refreshed.at(childHandle.get())) {
    return false;
  }
  return UpdateChildFontSnapshot(nested, child, traversal);
}

std::vector<Entity> RefreshChildFontResources(Registry& registry, FontRefreshTraversal& traversal) {
  std::vector<Entity> changedOwners;
  // Child documents persist in the source cache; a fresh facade must not hide their pending
  // layouts. Resolve each shared child once, then update every parent rendering host that uses it.
  for (auto view = registry.view<components::FontPaintDependenciesComponent>();
       const Entity host : view) {
    auto& paint = view.get<components::FontPaintDependenciesComponent>(host);
    bool hostChanged = false;
    for (auto& child : paint.children) {
      hostChanged |= RefreshChildFontResource(child, traversal);
    }
    if (hostChanged) {
      registry.get_or_emplace<components::DirtyFlagsComponent>(host).mark(
          components::DirtyFlagsComponent::Paint | components::DirtyFlagsComponent::Filter |
          components::DirtyFlagsComponent::RenderInstance);
      changedOwners.push_back(host);
    }
  }
  return changedOwners;
}

bool RefreshOwnFontResources(Registry& registry, std::vector<Entity>& changedOwners) {
  bool readinessChanged = false;
  auto* engine = registry.ctx().find<TextEngine>();
  if (!changedOwners.empty() || (engine && engine->needsFontResourceRefresh())) {
    if (engine && engine->needsFontResourceRefresh()) {
      const auto* manager = registry.ctx().find<FontManager>();
      const auto before = manager ? manager->faceDependencies() : std::vector<FontFaceDependency>();
      auto changed = engine->refreshFontResources();
      changedOwners.insert(changedOwners.end(), changed.begin(), changed.end());
      readinessChanged = manager && before != manager->faceDependencies();
    }
    components::InvalidateFontResourcePreparation(registry);
  }
  return readinessChanged;
}

bool RefreshDocumentFonts(SVGDocument& document, FontRefreshTraversal& traversal) {
  const auto handle = document.handle();
  if (traversal.refreshed.contains(handle.get())) {
    return traversal.refreshed.at(handle.get());
  }
  traversal.active.insert(handle.get());
  const auto access = document.writeAccess();
  Registry& registry = access.registry();
  traversal.cache.preserveBeforeRefresh(handle);
  std::vector<Entity> changedOwners = RefreshChildFontResources(registry, traversal);
  const bool readinessChanged = RefreshOwnFontResources(registry, changedOwners);
  traversal.cache.finishRefresh(handle, changedOwners);
  traversal.active.erase(handle.get());
  const bool changed = !changedOwners.empty() || readinessChanged;
  traversal.refreshed.emplace(handle.get(), changed);
  return changed;
}

bool HasChildFontResourceLimit(const Registry& registry) {
  for (const auto& [entity, paint] :
       registry.view<const components::FontPaintDependenciesComponent>().each()) {
    if (paint.resourceLimit || std::any_of(paint.children.begin(), paint.children.end(),
                                           [](const auto& child) { return child.resourceLimit; })) {
      return true;
    }
  }
  return false;
}

bool HasPendingChildFonts(const Registry& registry) {
  for (const auto& [entity, paint] :
       registry.view<const components::FontPaintDependenciesComponent>().each()) {
    if (paint.resourceLimit) {
      return true;
    }
    for (const auto& child : paint.children) {
      if (child.resourceLimit || child.needsRender || child.document.expired() ||
          std::any_of(child.fontDependencies.begin(), child.fontDependencies.end(),
                      [](const auto& face) { return face.state != FontFaceLoadState::Loaded; })) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

bool SVGDocument::refreshFontResources() {
  // The caller owns the idle adoption boundary. This does not publish staged global bytes and
  // must not run concurrently with main-document or preview rendering.
  const auto access = writeAccess();
  auto& stats = access.registry().ctx().emplace<components::FontResourceGraphCache::Stats>();
  stats = {};
  FontRefreshTraversal traversal(stats);
  return RefreshDocumentFonts(*this, traversal);
}

uint64_t SVGDocument::fontResourceRevision() const {
  [[maybe_unused]] DocumentReadAccess access = readAccess();
  const auto* manager = documentState_->registry().ctx().find<FontManager>();
  return manager ? manager->fontResourceRevision() : 0;
}

bool SVGDocument::fontResourcesExceeded() const {
  [[maybe_unused]] DocumentReadAccess access = readAccess();
  const auto* manager = documentState_->registry().ctx().find<FontManager>();
  const auto* work =
      documentState_->registry().ctx().find<components::FontResourceGraphCache::Stats>();
  return (manager && manager->fontDependenciesOverflowed()) || (work && work->resourceLimit) ||
         HasChildFontResourceLimit(documentState_->registry());
}

bool SVGDocument::hasUnresolvedFontResources() const {
  [[maybe_unused]] DocumentReadAccess access = readAccess();
  const auto* manager = documentState_->registry().ctx().find<FontManager>();
  return (manager && manager->hasUnresolvedDependencies()) ||
         HasPendingChildFonts(documentState_->registry());
}

namespace {

bool IsLiveFontTarget(const Registry& registry, Entity entity, uint32_t generation) {
  if (!registry.valid(entity) || !registry.all_of<components::ElementTypeComponent>(entity)) {
    return false;
  }
  const auto* lifetime = registry.try_get<components::NodeLifetimeComponent>(entity);
  return lifetime && lifetime->generation == generation &&
         lifetime->treeState == components::NodeLifetimeComponent::TreeState::Attached;
}

FontResourcePreflight ClassifyFontPreflight(components::FontResourceGraph::Collection collection) {
  using Status = FontResourcePreflight::Status;
  Status status = Status::Ready;
  if (collection.resourceLimit) {
    status = Status::ResourceLimit;
  } else if (collection.needsRender) {
    status = Status::NeedsRender;
  } else {
    for (const auto& dependency : collection.dependencies) {
      if (dependency.state == FontFaceLoadState::Failed) {
        status = Status::Unavailable;
        break;
      }
      if (dependency.state != FontFaceLoadState::Loaded) {
        status = Status::PendingFonts;
      }
    }
  }
  return {.status = status, .dependencies = std::move(collection.dependencies)};
}

FontResourcePreflight PrepareFontResourcesForTarget(Registry& registry, Entity target) {
  components::ScopedRenderInvalidationRestore restoreInvalidation(registry);
  auto* context = registry.ctx().find<components::RenderingContext>();
  if (!context) {
    context = &registry.ctx().emplace<components::RenderingContext>(registry);
  }
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  context->instantiateRenderTree(false, warnings);
  auto collection = components::FontResourceGraph(registry).collect(registry, target);
  if (!collection.resourceLimit) {
    if (auto* engine = registry.ctx().find<TextEngine>()) {
      for (const Entity root : collection.textRoots) {
        (void)engine->ensureComputedTextGeometryComponent(EntityHandle(registry, root));
      }
      collection = components::FontResourceGraph(registry).collect(registry, target);
    }
  }
  return ClassifyFontPreflight(std::move(collection));
}

}  // namespace

FontResourcePreflight SVGDocument::renderedFontResources() const {
  [[maybe_unused]] DocumentReadAccess access = readAccess();
  const Registry& registry = documentState_->registry();
  const Entity root = registry.ctx().get<components::SVGDocumentContext>().rootEntity;
  auto collection = components::FontResourceGraph(registry).collect(
      registry, root, components::FontResourceGraph::Purpose::RenderedFrame);
  const auto* scope = registry.ctx().find<components::RenderedFontResourceScope>();
  collection.needsRender |= !scope || !scope->complete || hasPendingRenderInvalidation();
  const auto* work = registry.ctx().find<components::FontResourceGraphCache::Stats>();
  collection.resourceLimit |= work && work->resourceLimit;
  return ClassifyFontPreflight(std::move(collection));
}

std::vector<FontFaceDependency> SVGDocument::renderedFontDependencies() const {
  return renderedFontResources().dependencies;
}

std::vector<FontFaceDependency> SVGDocument::fontDependenciesForElement(
    const SVGElement& element) const {
  if (element.handle_.unsafeRegistry() != &documentState_->registry()) {
    return {};
  }
  [[maybe_unused]] DocumentReadAccess access = readAccess();
  const Registry& registry = documentState_->registry();
  const Entity target = element.handle_.entity();
  if (!IsLiveFontTarget(registry, target, element.handle_.generation())) {
    return {};
  }
  return components::FontResourceGraph(registry).collect(registry, target).dependencies;
}

FontResourcePreflight SVGDocument::preflightFontResourcesForElement(const SVGElement& element) {
  using Status = FontResourcePreflight::Status;
  if (element.handle_.unsafeRegistry() != &documentState_->registry()) {
    return {.status = Status::InvalidTarget};
  }
  const Entity target = element.handle_.entity();
  const uint32_t generation = element.handle_.generation();
  {
    [[maybe_unused]] DocumentReadAccess targetAccess = element.handle_.readAccess();
    if (!IsLiveFontTarget(documentState_->registry(), target, generation)) {
      return {.status = Status::InvalidTarget};
    }
  }
  [[maybe_unused]] DocumentWriteAccess access = writeAccess();
  Registry& registry = documentState_->registry();
  if (!IsLiveFontTarget(registry, target, generation)) {
    return {.status = Status::InvalidTarget};
  }
  if (const auto* state = registry.ctx().find<components::RenderTreeState>();
      state && state->hasBeenBuilt &&
      (state->needsFullRebuild || state->needsFullStyleRecompute ||
       !registry.view<const components::DirtyFlagsComponent>().empty())) {
    return {.status = Status::NeedsRender};
  }

  return PrepareFontResourcesForTarget(registry, target);
}

namespace {

/**
 * The store currently in \p state's source store holder, or null for a document without source
 * text. It reads the holder on every call, so it finds a store that xml::XMLDocument::setSource
 * installed after the document was built.
 *
 * @param state Document whose source store to find.
 */
const xml::XMLSourceStore* CurrentSourceStore(const DocumentState& state) {
  const std::shared_ptr<xml::components::XMLSourceStoreHolder>& holder = state.sourceStoreHolder();
  return holder != nullptr ? holder->store.get() : nullptr;
}

}  // namespace

// The three source accessors below read the store through DocumentState rather than the registry
// context, so a thread may read the source while another holds the document's write access.
bool SVGDocument::hasSourceStore() const {
  return CurrentSourceStore(*documentState_) != nullptr;
}

std::string_view SVGDocument::source() const {
  const xml::XMLSourceStore* store = CurrentSourceStore(*documentState_);
  return store != nullptr ? store->source() : std::string_view();
}

std::uint64_t SVGDocument::sourceVersion() const {
  const xml::XMLSourceStore* store = CurrentSourceStore(*documentState_);
  return store != nullptr ? store->sourceVersion() : 0;
}

xml::ApplySourceEditResult SVGDocument::applySourceEdit(const xml::XMLEditIntent& intent) {
  if (!documentState_->registry().ctx().contains<xml::components::XMLDocumentContext>()) {
    xml::ApplySourceEditResult result;
    result.diagnostic = ParseDiagnostic::Error(
        "Cannot apply source edit to SVGDocument without XML source text", intent.range);
    return result;
  }

  // §concurrent-dom: applySourceEdit drives a tree-shaped mutation (insertions, removes, attribute
  // changes) through `xmlDocument().applySourceEdit` and `applyXMLMutation`; acquire write access
  // up front so all of that runs under one guard under ThreadingMode::ConcurrentDom.
  [[maybe_unused]] DocumentWriteAccess access = writeAccess();

  xml::ApplySourceEditResult result;
  {
    // Defer detached-node collection across the reparse. ReplaceChildrenFromParsedNode removes
    // every child and then reuses the matching ones by re-appending, but each removeChild routes
    // through TreeMutation::RemoveChild, which collects detached nodes synchronously. Without the
    // deferral a child queued for reuse is destroyed mid-reparse and the reparse's still-live
    // XMLNode handle trips an entt "set does not contain entity" assert. Reused children are
    // re-attached by the subsequent appendChild; genuinely-removed children are swept by the
    // Collect below once the deferral ends.
    DetachedNodeCollectionDeferral collectionDeferral =
        documentState_->deferDetachedNodeCollection();
    result = xmlDocument().applySourceEdit(intent);
    applyXMLMutations(result);
  }
  components::NodeLifetimeCollector::Collect(documentState_->registry());

  return result;
}

xml::ApplySourceEditResult SVGDocument::setElementAttribute(const SVGElement& element,
                                                            const xml::XMLQualifiedNameRef& name,
                                                            std::string_view value) {
  // §concurrent-dom: this is a mutation entry point. Acquire write access up front so the implicit
  // `EntityHandle` conversions of `element.handle_` below (resolve() through the guarded path)
  // don't fire the scoped-access assert under ThreadingMode::ConcurrentDom.
  [[maybe_unused]] DocumentWriteAccess access = writeAccess();
  if (hasSourceStore()) {
    std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.handle_);
    if (xmlNode.has_value()) {
      xml::ApplySourceEditResult result = xmlDocument().setAttribute(*xmlNode, name, value);
      applyXMLMutations(result);
      return result;
    }
  }

  xml::ApplySourceEditResult result;
  SVGElement mutableElement(element.handle_);
  if (std::optional<ParseDiagnostic> diagnostic =
          mutableElement.setAttributeFromXMLMutation(name, value)) {
    result.diagnostic = std::move(diagnostic);
  }
  return result;
}

xml::ApplySourceEditResult SVGDocument::removeElementAttribute(
    const SVGElement& element, const xml::XMLQualifiedNameRef& name) {
  // §concurrent-dom: this is a mutation entry point. Acquire write access up front so the implicit
  // `EntityHandle` conversions of `element.handle_` below don't fire the scoped-access assert under
  // ThreadingMode::ConcurrentDom.
  [[maybe_unused]] DocumentWriteAccess access = writeAccess();
  if (hasSourceStore()) {
    std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.handle_);
    if (xmlNode.has_value()) {
      xml::ApplySourceEditResult result = xmlDocument().removeAttribute(*xmlNode, name);
      applyXMLMutations(result);
      return result;
    }
  }

  SVGElement mutableElement(element.handle_);
  mutableElement.removeAttributeFromXMLMutation(name);
  return xml::ApplySourceEditResult();
}

xml::ApplySourceEditResult SVGDocument::insertElement(const SVGElement& parent,
                                                      const SVGElement& element,
                                                      std::optional<SVGElement> referenceElement) {
  // §concurrent-dom: this is a mutation entry point. Acquire write access up front so the implicit
  // `EntityHandle` conversions of `parent.handle_`, `element.handle_`, and
  // `referenceElement->handle_` below don't fire the scoped-access assert under
  // ThreadingMode::ConcurrentDom.
  [[maybe_unused]] DocumentWriteAccess access = writeAccess();
  if (hasSourceStore()) {
    std::optional<xml::XMLNode> parentNode = xml::XMLNode::TryCast(parent.handle_);
    if (parentNode.has_value()) {
      xml::ApplySourceEditResult result;
      std::optional<xml::XMLNode> referenceNode;
      if (referenceElement.has_value()) {
        referenceNode = xml::XMLNode::TryCast(referenceElement->handle_);
        if (!referenceNode.has_value()) {
          result.scope = xml::ReparseScope::ElementSubtree;
          result.diagnostic =
              ParseDiagnostic::Error("Cannot insert before an element without XML source identity",
                                     parentNode->getNodeLocation().value_or(FallbackRange()));
          return result;
        }
      }

      xml::XMLDocument document = xmlDocument();
      xml::XMLNode insertedNode = EnsureXMLSubtreeForSVGElement(document, element);
      const std::optional<SVGElement> oldParent = element.parentElement();
      result = document.insertNode(*parentNode, insertedNode, referenceNode);
      applyXMLMutations(result);

      if (result.applied) {
        if (oldParent.has_value() && *oldParent != parent) {
          MarkChildRemoved(oldParent->entityHandle());
        }
        MarkChildInserted(parent.entityHandle(), element.entityHandle());
      }

      return result;
    }
  }

  return xml::ApplySourceEditResult();
}

xml::ApplySourceEditResult SVGDocument::removeElement(const SVGElement& element) {
  // §concurrent-dom: this is a mutation entry point. Acquire write access up front so the implicit
  // `EntityHandle` conversions of `element.handle_` and `parent.entityHandle()` below don't fire
  // the scoped-access assert under ThreadingMode::ConcurrentDom.
  [[maybe_unused]] DocumentWriteAccess access = writeAccess();
  // The document always has its root <svg>: layout, rendering and hit testing read the root's
  // components unconditionally, so removing it would leave every later frame on a dead entity.
  if (element == svgElement()) {
    xml::ApplySourceEditResult result;
    result.scope = xml::ReparseScope::ElementSubtree;
    const std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.handle_);
    result.diagnostic = ParseDiagnostic::Error(
        "Cannot remove the document root element",
        xmlNode.has_value() ? xmlNode->getNodeLocation().value_or(FallbackRange())
                            : FallbackRange());
    return result;
  }

  const std::optional<SVGElement> parent = element.parentElement();

  if (hasSourceStore()) {
    std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.handle_);
    if (xmlNode.has_value()) {
      xml::ApplySourceEditResult result = xmlDocument().removeNode(*xmlNode);
      applyXMLMutations(result);
      if (result.applied && parent.has_value()) {
        MarkChildRemoved(parent->entityHandle());
      }
      return result;
    }
  }

  if (parent.has_value()) {
    MarkChildRemoved(parent->entityHandle());
  }
  components::TreeMutation::Remove(element.handle_);
  return xml::ApplySourceEditResult();
}

xml::ApplySourceEditResult SVGDocument::revertElementInsertions(
    std::span<const SVGElement> elements, std::span<const xml::XMLSourceDelta> insertions) {
  [[maybe_unused]] DocumentWriteAccess access = writeAccess();
  xml::ApplySourceEditResult result;
  result.scope = xml::ReparseScope::ElementSubtree;
  if (elements.size() != insertions.size() || !hasSourceStore()) {
    result.diagnostic = ParseDiagnostic::Error(
        "Cannot revert element insertions without matching source edits", FallbackRange());
    return result;
  }

  std::vector<xml::XMLDocument::NodeInsertion> nodes;
  std::vector<SVGElement> parents;
  nodes.reserve(elements.size());
  parents.reserve(elements.size());
  for (std::size_t i = 0; i < elements.size(); ++i) {
    std::optional<xml::XMLNode> node = xml::XMLNode::TryCast(elements[i].handle_);
    std::optional<SVGElement> parent = elements[i].parentElement();
    if (!node.has_value() || !parent.has_value()) {
      result.diagnostic = ParseDiagnostic::Error(
          "Cannot revert an element without XML source identity or a parent", FallbackRange());
      return result;
    }
    nodes.push_back(xml::XMLDocument::NodeInsertion{.node = *node, .insertion = insertions[i]});
    parents.push_back(*parent);
  }

  result = xmlDocument().revertNodeInsertions(nodes);
  applyXMLMutations(result);
  for (std::size_t i = 0; i < elements.size(); ++i) {
    if (!elements[i].parentElement().has_value()) {
      MarkChildRemoved(parents[i].entityHandle());
    }
  }
  return result;
}

xml::ApplySourceEditResult SVGDocument::setElementTextContent(const SVGElement& element,
                                                              std::string_view text) {
  // §concurrent-dom: this is a mutation entry point. Acquire write access up front so the implicit
  // `EntityHandle` conversions of `element.handle_` below don't fire the scoped-access assert
  // under ThreadingMode::ConcurrentDom.
  [[maybe_unused]] DocumentWriteAccess access = writeAccess();
  xml::ApplySourceEditResult result;
  result.scope = xml::ReparseScope::TextNode;
  if (!hasSourceStore()) {
    return result;
  }

  std::optional<xml::XMLNode> elementNode = xml::XMLNode::TryCast(element.handle_);
  if (!elementNode.has_value()) {
    result.diagnostic = ParseDiagnostic::Error(
        "Cannot set text content on an element without XML source identity", FallbackRange());
    return result;
  }

  // Text-only content: replacing the tracked text range under an element with
  // element children (`<tspan>`) would splice only the leading chunk while the
  // component mirror carries the full text. Callers that need the source
  // mirrored clear element children first (the editor's SetTextContent flows
  // do); otherwise the edit stays component-only, matching the pre-structured
  // behavior, and is reported as unapplied without a diagnostic.
  if (element.firstChild().has_value()) {
    return result;
  }

  result = xmlDocument().setElementText(*elementNode, text);
  const EntityHandle handle = elementNode->entityHandle();
  const bool typedText =
      handle.all_of<components::TextComponent>() || IsDescriptiveElement(handle) ||
      handle.get<components::ElementTypeComponent>().type() == ElementType::Style;
  // This setter replaces all text; its parent-value receipt supersedes any old raw snapshot.
  if (result.applied && !result.diagnostic && typedText) {
    components::ResetSelectorTextContent(handle);
  }
  applyXMLMutations(result);
  return result;
}

std::optional<SVGElement> SVGDocument::querySelector(std::string_view str) {
  const auto selectorResult = css::parser::SelectorParser::Parse(str);
  if (selectorResult.hasError()) {
    return std::nullopt;
  }

  const css::Selector& selector = selectorResult.result();
  DocumentReadAccess access = documentState_->read();
  Registry& registry = access.registry();
  EntityHandle root(registry, registry.ctx().get<components::SVGDocumentContext>().rootEntity);
  return details::QuerySelectorSearch(selector, root);
}

xml::XMLDocument SVGDocument::xmlDocument() const {
  return xml::XMLDocument::CreateFromRegistry(documentState_->sharedRegistry());
}

void SVGDocument::applyXMLMutations(xml::ApplySourceEditResult& result) {
  std::unordered_set<Entity> projectedRoots;
  for (const xml::XMLMutation& mutation : result.mutations) {
    // Subtree projection already consumes the final child/value state in this XML edit batch.
    bool covered = false;
    for (std::optional<xml::XMLNode> node = mutation.node; node; node = node->parentElement()) {
      if (projectedRoots.contains(node->entityHandle().entity())) {
        covered = true;
        break;
      }
    }
    if (covered && mutation.kind == xml::XMLMutation::Kind::NodeValueChanged) {
      continue;
    }
    std::optional<ParseDiagnostic> projectionDiagnostic = applyXMLMutation(mutation);
    if (projectionDiagnostic.has_value() && !result.diagnostic.has_value()) {
      result.diagnostic = std::move(projectionDiagnostic);
    }
    if (mutation.kind == xml::XMLMutation::Kind::SubtreeReplaced) {
      projectedRoots.insert(mutation.node.entityHandle().entity());
    }
  }
}

std::optional<ParseDiagnostic> SVGDocument::applyXMLMutation(const xml::XMLMutation& mutation) {
  if (mutation.kind == xml::XMLMutation::Kind::SourceDiagnosticChanged) {
    // Never an edit failure: the wrapped XML result already carries this edit's own failure,
    // while this mutation also fires when an unrelated older span shifts under a successful
    // edit or finally clears. Propagating it would mark clean edits failed.
    return std::nullopt;
  }

  if (mutation.kind == xml::XMLMutation::Kind::NodeValueChanged) {
    return applyNodeValueXMLMutation(mutation);
  }

  if (mutation.kind == xml::XMLMutation::Kind::NodeRemoved) {
    MarkSubtreeReplaced(rootEntityHandle());
    return std::nullopt;
  }

  if (mutation.kind == xml::XMLMutation::Kind::NodeInserted) {
    if (std::optional<ParseDiagnostic> diagnostic =
            projectXMLSubtree(mutation.node, /*contentsReparsed=*/true)) {
      return diagnostic;
    }
    MarkSubtreeReplaced(mutation.node.entityHandle());
    return std::nullopt;
  }

  const EntityHandle handle = mutation.node.entityHandle();
  if (!handle || !handle.all_of<components::ElementTypeComponent>()) {
    return ParseDiagnostic::Error("XML mutation target is not an SVG element",
                                  MutationRange(source(), mutation));
  }

  if (IsAttributeMutation(mutation.kind)) {
    return applyAttributeXMLMutation(mutation, handle);
  }
  if (mutation.kind == xml::XMLMutation::Kind::SubtreeReplaced) {
    if (std::optional<ParseDiagnostic> diagnostic =
            projectXMLSubtree(mutation.node, /*contentsReparsed=*/true)) {
      return diagnostic;
    }
    MarkSubtreeReplaced(handle);
    return std::nullopt;
  }

  UTILS_UNREACHABLE();
}

std::optional<ParseDiagnostic> SVGDocument::applyNodeValueXMLMutation(
    const xml::XMLMutation& mutation) {
  const std::optional<xml::XMLNode> element = NodeValueMutationElement(mutation);
  std::optional<ParseDiagnostic> diagnostic;
  if (mutation.value.has_value() && element.has_value() && CanReparseDescriptiveText(*element)) {
    diagnostic = reparseDescriptiveText(*element);
  } else {
    diagnostic = ApplyNodeValueChanged(source(), mutation);
  }
  if (element) {
    if (diagnostic) {
      components::RejectSelectorContentProjection(element->entityHandle());
    } else {
      components::AcceptSelectorTextValueProjection(element->entityHandle());
      components::TreeMutation::InvalidateTextContentStyles(element->entityHandle());
    }
  }
  return diagnostic;
}

std::optional<ParseDiagnostic> SVGDocument::reparseDescriptiveText(const xml::XMLNode& node) {
  if (!CanReparseDescriptiveText(node)) {
    return std::nullopt;
  }
  return documentState_->registry()
      .ctx()
      .get<components::SVGDocumentContext>()
      .parseEditedElementContents(source(), SVGElement(node.entityHandle()), node);
}

std::optional<ParseDiagnostic> SVGDocument::applyAttributeXMLMutation(
    const xml::XMLMutation& mutation, EntityHandle handle) {
  if (IsNamespaceDeclaration(mutation.attributeName)) {
    UpdateNamespaceMembership(mutation.node);
    MarkSubtreeReplaced(handle);
    return projectXMLSubtree(mutation.node, /*contentsReparsed=*/false);
  }
  SVGElement element(handle);
  if (mutation.kind == xml::XMLMutation::Kind::AttributeSet) {
    if (!mutation.value.has_value()) {
      return ParseDiagnostic::Error("XML AttributeSet mutation is missing a value",
                                    MutationRange(source(), mutation));
    }
    if (std::optional<ParseDiagnostic> diagnostic =
            ReserveProjectedAttributes(handle, mutation.node)) {
      diagnostic->range = MutationRange(source(), mutation);
      return diagnostic;
    }
    if (std::optional<ParseDiagnostic> diagnostic =
            element.setAttributeFromXMLMutation(mutation.attributeName, *mutation.value)) {
      diagnostic->range = MutationRange(source(), mutation);
      return diagnostic;
    }
    return std::nullopt;
  }

  if (std::optional<ParseDiagnostic> diagnostic =
          ReserveProjectedAttributes(handle, mutation.node)) {
    diagnostic->range = MutationRange(source(), mutation);
    return diagnostic;
  }
  element.removeAttributeFromXMLMutation(mutation.attributeName);
  return std::nullopt;
}

std::optional<ParseDiagnostic> SVGDocument::projectXMLAttributes(const xml::XMLNode& node,
                                                                 bool builtTypedElement) {
  const EntityHandle handle = node.entityHandle();
  SVGElement element(handle);
  // A newly built element needs what only the parser applies, such as an image's `href` or a
  // title's text; the generic pass below reports attribute diagnostics.
  const auto& parseBuiltElement =
      handle.registry()->ctx().get<components::SVGDocumentContext>().parseBuiltElement;
  if (builtTypedElement && parseBuiltElement) {
    if (std::optional<ParseDiagnostic> diagnostic = parseBuiltElement(source(), element, node)) {
      return diagnostic;
    }
  }

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
  return std::nullopt;
}

std::optional<ParseDiagnostic> SVGDocument::projectXMLSubtree(const xml::XMLNode& node,
                                                              bool contentsReparsed) {
  std::optional<ParseDiagnostic> diagnostic = projectXMLSubtreeContents(node, contentsReparsed);
  if (diagnostic && contentsReparsed && node.type() == xml::XMLNode::Type::Element) {
    components::RejectSelectorContentProjection(node.entityHandle());
  }
  return diagnostic;
}

std::optional<ParseDiagnostic> SVGDocument::projectXMLSubtreeContents(const xml::XMLNode& node,
                                                                      bool contentsReparsed) {
  if (node.type() != xml::XMLNode::Type::Element) {
    return std::nullopt;
  }

  const EntityHandle handle = node.entityHandle();
  const ProjectedElement projected = EnsureProjectedElementComponents(handle, node);

  if (std::optional<ParseDiagnostic> diagnostic = ReserveProjectedAttributes(handle, node)) {
    return diagnostic;
  }

  if (std::optional<ParseDiagnostic> diagnostic =
          projectXMLAttributes(node, projected.builtTypedElement)) {
    return diagnostic;
  }
  // A subtree reparse syncs a kept element's children and value from source, so a kept descriptive
  // element parses its text again. Elsewhere, such as under an ancestor reprojected for an `xmlns`
  // edit, an existing element keeps the text its parse gave it.
  if (!projected.builtTypedElement && contentsReparsed) {
    if (std::optional<ParseDiagnostic> diagnostic = reparseDescriptiveText(node)) {
      return diagnostic;
    }
  }

  if (std::optional<ParseDiagnostic> diagnostic = ProjectTextContents(handle, node)) {
    return diagnostic;
  }
  if (std::optional<ParseDiagnostic> diagnostic = ProjectStyleContents(handle, node)) {
    return diagnostic;
  }

  components::CompleteSelectorContentProjection(handle, contentsReparsed);
  for (std::optional<xml::XMLNode> child = node.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (std::optional<ParseDiagnostic> diagnostic = projectXMLSubtree(*child, contentsReparsed)) {
      return diagnostic;
    }
  }

  return std::nullopt;
}

}  // namespace donner::svg
