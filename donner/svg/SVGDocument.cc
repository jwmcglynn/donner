#include "donner/svg/SVGDocument.h"

#include <string>

#include "donner/base/element/ElementTraversalGenerators.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/base/xml/components/TreeMutationContext.h"
#include "donner/base/xml/components/XMLDocumentContext.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGQuerySelector.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/NodeLifetimeCollector.h"
#include "donner/svg/components/NodeLifetimeComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/TreeMutation.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/ClipPathComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

namespace {

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
    auto& text = targetHandle->get<components::TextComponent>();
    text.text = *mutation.value;
    text.textChunks.clear();
    text.textChunks.emplace_back(*mutation.value);
    InvalidateTextGeometry(*targetHandle);
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

ElementType ElementTypeForTag(const xml::XMLQualifiedNameRef& tagName) {
  if (tagName.name == "a") {
    return ElementType::A;
  }
  if (tagName.name == "circle") {
    return ElementType::Circle;
  }
  if (tagName.name == "clipPath") {
    return ElementType::ClipPath;
  }
  if (tagName.name == "defs") {
    return ElementType::Defs;
  }
  if (tagName.name == "ellipse") {
    return ElementType::Ellipse;
  }
  if (tagName.name == "feGaussianBlur") {
    return ElementType::FeGaussianBlur;
  }
  if (tagName.name == "filter") {
    return ElementType::Filter;
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
  if (tagName.name == "style") {
    return ElementType::Style;
  }
  if (tagName.name == "text") {
    return ElementType::Text;
  }
  if (tagName.name == "tspan") {
    return ElementType::TSpan;
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
    case ElementType::Rect:
    case ElementType::Text:
    case ElementType::TSpan: return true;

    default: return false;
  }
}

void EnsureProjectedElementComponents(EntityHandle handle,
                                      const xml::XMLQualifiedNameRef& tagName) {
  const ElementType projectedType = ElementTypeForTag(tagName);
  ElementType type = projectedType;
  if (!handle.all_of<components::ElementTypeComponent>()) {
    handle.emplace<components::ElementTypeComponent>(type);
  } else {
    type = handle.get<components::ElementTypeComponent>().type();
    if (type == ElementType::Unknown && projectedType != ElementType::Unknown) {
      type = projectedType;
      handle.emplace_or_replace<components::ElementTypeComponent>(type);
    }
  }

  [[maybe_unused]] auto& transform = handle.get_or_emplace<components::TransformComponent>();
  if (type == ElementType::Defs) {
    auto& behavior = handle.get_or_emplace<components::RenderingBehaviorComponent>(
        components::RenderingBehavior::Nonrenderable);
    behavior.behavior = components::RenderingBehavior::Nonrenderable;
  }
  if (type == ElementType::Filter) {
    auto& behavior = handle.get_or_emplace<components::RenderingBehaviorComponent>(
        components::RenderingBehavior::Nonrenderable);
    behavior.behavior = components::RenderingBehavior::Nonrenderable;
    [[maybe_unused]] auto& filter = handle.get_or_emplace<components::FilterComponent>();
  }
  if (type == ElementType::ClipPath) {
    [[maybe_unused]] auto& clipPath = handle.get_or_emplace<components::ClipPathComponent>();
    auto& behavior = handle.get_or_emplace<components::RenderingBehaviorComponent>(
        components::RenderingBehavior::Nonrenderable);
    behavior.behavior = components::RenderingBehavior::Nonrenderable;
    behavior.inheritsParentTransform = false;
  }
  if (type == ElementType::FeGaussianBlur) {
    [[maybe_unused]] auto& primitive =
        handle.get_or_emplace<components::FilterPrimitiveComponent>();
    [[maybe_unused]] auto& gaussianBlur =
        handle.get_or_emplace<components::FEGaussianBlurComponent>();
    auto& behavior = handle.get_or_emplace<components::RenderingBehaviorComponent>(
        components::RenderingBehavior::Nonrenderable);
    behavior.behavior = components::RenderingBehavior::Nonrenderable;
  }
  if (UsesNoTraverseChildren(type) && !handle.all_of<components::RenderingBehaviorComponent>()) {
    handle.emplace<components::RenderingBehaviorComponent>(
        components::RenderingBehavior::NoTraverseChildren);
  }

  if (type == ElementType::A || type == ElementType::Text || type == ElementType::TSpan) {
    // `<a>` is a transparent text-content group: when nested in text its children participate in
    // the text layout (like `<tspan>`), so it needs the text components. Unlike `<tspan>` it is
    // NOT in UsesNoTraverseChildren — outside of text it groups arbitrary graphics like `<g>`.
    [[maybe_unused]] auto& text = handle.get_or_emplace<components::TextComponent>();
    [[maybe_unused]] auto& positioning =
        handle.get_or_emplace<components::TextPositioningComponent>();
  }
  if (type == ElementType::Text) {
    handle.get_or_emplace<components::TextRootComponent>();
  }
  if (type == ElementType::Style) {
    [[maybe_unused]] auto& stylesheet = handle.get_or_emplace<components::StylesheetComponent>();
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

void MarkStylesheetChanged(EntityHandle handle) {
  Registry& registry = *handle.registry();
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
  if (!node.has_value()) {
    node = xml::XMLNode::CreateElementNodeOn(document, element.entityHandle(), element.tagName());
  }

  for (std::optional<SVGElement> child = element.firstChild(); child.has_value();
       child = child->nextSibling()) {
    (void)EnsureXMLSubtreeForSVGElement(document, *child);
  }

  return *node;
}

void ProjectTextContents(EntityHandle handle, const xml::XMLNode& node) {
  if (!handle.all_of<components::TextComponent>()) {
    return;
  }

  auto& text = handle.get<components::TextComponent>();
  text.text = RcString("");
  text.textChunks.clear();

  for (std::optional<xml::XMLNode> child = node.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (child->type() == xml::XMLNode::Type::Data || child->type() == xml::XMLNode::Type::CData) {
      const RcString value = child->value().value_or(RcString(""));
      if (text.text.empty()) {
        text.text = value;
      } else {
        text.text = text.text + value;
      }

      if (text.textChunks.empty()) {
        text.textChunks.emplace_back(value);
      } else if (text.textChunks.back().empty()) {
        text.textChunks.back() = value;
      } else {
        text.textChunks.emplace_back(value);
      }
    } else if (child->type() == xml::XMLNode::Type::Element) {
      if (text.textChunks.empty()) {
        text.textChunks.emplace_back(RcString(""));
      }
      text.textChunks.emplace_back(RcString(""));
    }
  }
}

std::optional<ParseDiagnostic> ProjectStyleContents(EntityHandle handle, const xml::XMLNode& node) {
  if (!handle.all_of<components::ElementTypeComponent>() ||
      handle.get<components::ElementTypeComponent>().type() != ElementType::Style) {
    return std::nullopt;
  }

  auto* stylesheet = handle.try_get<components::StylesheetComponent>();
  if (stylesheet == nullptr || !stylesheet->isCssType()) {
    return std::nullopt;
  }

  std::string combined;
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
  }

  stylesheet->parseStylesheet(std::string_view(combined), std::move(sourceMap));
  return std::nullopt;
}

}  // namespace

SVGDocument::SVGDocument(SVGDocumentHandle documentState, Settings settings,
                         EntityHandle ontoEntityHandle)
    : documentState_(std::move(documentState)) {
  Registry& registry = documentState_->registry();
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

Transform2d SVGDocument::canvasFromDocumentTransform() const {
  DocumentReadAccess access = documentState_->read();
  return components::LayoutSystem().getCanvasFromDocumentTransform(access.registry());
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

bool SVGDocument::hasSourceStore() const {
  if (!documentState_->registry().ctx().contains<xml::components::XMLDocumentContext>()) {
    return false;
  }

  return xmlDocument().hasSourceStore();
}

std::string_view SVGDocument::source() const {
  if (!documentState_->registry().ctx().contains<xml::components::XMLDocumentContext>()) {
    return std::string_view();
  }

  return xmlDocument().source();
}

std::uint64_t SVGDocument::sourceVersion() const {
  if (!documentState_->registry().ctx().contains<xml::components::XMLDocumentContext>()) {
    return 0;
  }

  return xmlDocument().sourceVersion();
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
    for (const xml::XMLMutation& mutation : result.mutations) {
      std::optional<ParseDiagnostic> projectionDiagnostic = applyXMLMutation(mutation);
      if (projectionDiagnostic.has_value() && !result.diagnostic.has_value()) {
        result.diagnostic = std::move(projectionDiagnostic);
      }
    }
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
      for (const xml::XMLMutation& mutation : result.mutations) {
        std::optional<ParseDiagnostic> projectionDiagnostic = applyXMLMutation(mutation);
        if (projectionDiagnostic.has_value() && !result.diagnostic.has_value()) {
          result.diagnostic = std::move(projectionDiagnostic);
        }
      }
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
      for (const xml::XMLMutation& mutation : result.mutations) {
        std::optional<ParseDiagnostic> projectionDiagnostic = applyXMLMutation(mutation);
        if (projectionDiagnostic.has_value() && !result.diagnostic.has_value()) {
          result.diagnostic = std::move(projectionDiagnostic);
        }
      }
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
      for (const xml::XMLMutation& mutation : result.mutations) {
        std::optional<ParseDiagnostic> projectionDiagnostic = applyXMLMutation(mutation);
        if (projectionDiagnostic.has_value() && !result.diagnostic.has_value()) {
          result.diagnostic = std::move(projectionDiagnostic);
        }
      }

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
  const std::optional<SVGElement> parent = element.parentElement();

  if (hasSourceStore()) {
    std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.handle_);
    if (xmlNode.has_value()) {
      xml::ApplySourceEditResult result = xmlDocument().removeNode(*xmlNode);
      for (const xml::XMLMutation& mutation : result.mutations) {
        std::optional<ParseDiagnostic> projectionDiagnostic = applyXMLMutation(mutation);
        if (projectionDiagnostic.has_value() && !result.diagnostic.has_value()) {
          result.diagnostic = std::move(projectionDiagnostic);
        }
      }
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
  DocumentReadAccess access = documentState_->read();
  Registry& registry = access.registry();
  EntityHandle root(registry, registry.ctx().get<components::SVGDocumentContext>().rootEntity);
  return details::QuerySelectorSearch(selector, root);
}

xml::XMLDocument SVGDocument::xmlDocument() const {
  return xml::XMLDocument::CreateFromRegistry(documentState_->sharedRegistry());
}

std::optional<ParseDiagnostic> SVGDocument::applyXMLMutation(const xml::XMLMutation& mutation) {
  if (mutation.kind == xml::XMLMutation::Kind::SourceDiagnosticChanged) {
    return mutation.diagnostic;
  }

  if (mutation.kind == xml::XMLMutation::Kind::NodeValueChanged) {
    return ApplyNodeValueChanged(source(), mutation);
  }

  if (mutation.kind == xml::XMLMutation::Kind::NodeRemoved) {
    MarkSubtreeReplaced(rootEntityHandle());
    return std::nullopt;
  }

  if (mutation.kind == xml::XMLMutation::Kind::NodeInserted) {
    if (std::optional<ParseDiagnostic> diagnostic = projectXMLSubtree(mutation.node)) {
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

    case xml::XMLMutation::Kind::SourceDiagnosticChanged: UTILS_UNREACHABLE();

    case xml::XMLMutation::Kind::NodeValueChanged: UTILS_UNREACHABLE();

    case xml::XMLMutation::Kind::NodeInserted: UTILS_UNREACHABLE();

    case xml::XMLMutation::Kind::NodeRemoved: UTILS_UNREACHABLE();

    case xml::XMLMutation::Kind::SubtreeReplaced:
      if (std::optional<ParseDiagnostic> diagnostic = projectXMLSubtree(mutation.node)) {
        return diagnostic;
      }
      MarkSubtreeReplaced(handle);
      return std::nullopt;
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

  ProjectTextContents(handle, node);
  if (std::optional<ParseDiagnostic> diagnostic = ProjectStyleContents(handle, node)) {
    return diagnostic;
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
