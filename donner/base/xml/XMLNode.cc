#include "donner/base/xml/XMLNode.h"

#include <algorithm>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "donner/base/FileOffset.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLEscape.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/base/xml/components/TreeMutationContext.h"
#include "donner/base/xml/components/XMLDocumentContext.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"
#include "donner/base/xml/components/XMLValueComponent.h"

namespace donner::xml {

using components::XMLDocumentContext;
using components::XMLNamespaceContext;
using donner::components::AttributesComponent;
using donner::components::TreeComponent;

namespace {

// TODO: Move to its own header
struct XMLNodeTypeComponent {
  explicit XMLNodeTypeComponent(XMLNode::Type type) : type_(type) {}

  XMLNode::Type type() const { return type_; }

private:
  XMLNode::Type type_;
};

// TODO: Move to its own header
struct SourceOffsetComponent {
  std::optional<FileOffset> startOffset;
  std::optional<FileOffset> endOffset;
  std::optional<SourceAnchorSpan> anchorSpan;
  std::optional<SourceAnchorSpan> openingTagSpan;
  std::optional<SourceAnchorSpan> closingTagSpan;
  std::optional<SourceAnchorSpan> valueSpan;
  std::uint64_t anchorSourceVersion = 0;
};

struct ProjectionRecordBudget {
  std::size_t liveRecords = 0;
};

/// Owns an existing source span after its projected XML node can be collected.
struct ProjectedTextRecord {
  std::shared_ptr<XMLSourceStore> store;
  std::shared_ptr<ProjectionRecordBudget> budget;
  SourceAnchorSpan span;
  Entity originalNode;
  XMLNode::Type type;
  RcString value;

  ProjectedTextRecord(std::shared_ptr<XMLSourceStore> sourceStore,
                      std::shared_ptr<ProjectionRecordBudget> recordBudget,
                      SourceAnchorSpan sourceSpan, Entity node, XMLNode::Type nodeType,
                      RcString text)
      : store(std::move(sourceStore)),
        budget(std::move(recordBudget)),
        span(sourceSpan),
        originalNode(node),
        type(nodeType),
        value(std::move(text)) {
    ++budget->liveRecords;
  }
  ProjectedTextRecord(const ProjectedTextRecord&) = delete;
  ProjectedTextRecord& operator=(const ProjectedTextRecord&) = delete;
  ProjectedTextRecord(ProjectedTextRecord&&) noexcept = default;
  ProjectedTextRecord& operator=(ProjectedTextRecord&& other) noexcept {
    std::swap(store, other.store);
    std::swap(budget, other.budget);
    std::swap(span, other.span);
    std::swap(originalNode, other.originalNode);
    std::swap(type, other.type);
    std::swap(value, other.value);
    return *this;
  }
  ~ProjectedTextRecord() {
    if (store) {
      store->invalidateAnchor(span.start);
      store->invalidateAnchor(span.end);
    }
    if (budget) {
      --budget->liveRecords;
    }
  }
};
static_assert(!std::is_copy_constructible_v<ProjectedTextRecord>);
static_assert(std::is_nothrow_move_constructible_v<ProjectedTextRecord>);
static_assert(std::is_nothrow_move_assignable_v<ProjectedTextRecord>);

struct ProjectedTextRecordsComponent {
  ProjectedTextRecordsComponent() = default;
  ProjectedTextRecordsComponent(const ProjectedTextRecordsComponent&) = delete;
  ProjectedTextRecordsComponent& operator=(const ProjectedTextRecordsComponent&) = delete;
  ProjectedTextRecordsComponent(ProjectedTextRecordsComponent&&) noexcept = default;
  ProjectedTextRecordsComponent& operator=(ProjectedTextRecordsComponent&&) noexcept = default;
  std::vector<ProjectedTextRecord> records;
};
static_assert(!std::is_copy_constructible_v<ProjectedTextRecordsComponent>);
static_assert(std::is_nothrow_move_constructible_v<ProjectedTextRecordsComponent>);
static_assert(std::is_nothrow_move_assignable_v<ProjectedTextRecordsComponent>);

/// Only a surviving raw XML handle needs this link to invalidate its parent-owned record.
struct ProjectedTextParentComponent {
  Entity parent;
};

void ClearTextProjectionProvenance(EntityHandle handle) {
  if (const auto* link = handle.try_get<ProjectedTextParentComponent>();
      link && handle.registry()->valid(link->parent)) {
    if (auto* records = handle.registry()->try_get<ProjectedTextRecordsComponent>(link->parent)) {
      const Entity original = handle.entity();
      std::erase_if(records->records,
                    [original](const auto& record) { return record.originalNode == original; });
    }
  }
  handle.remove<ProjectedTextParentComponent>();
}

/// Serialize an XMLQualifiedNameRef to XML syntax (ns:name or just name).
void AppendQualifiedName(std::string& out, const XMLQualifiedNameRef& qname) {
  if (!qname.namespacePrefix.empty()) {
    out.append(qname.namespacePrefix);
    out.push_back(':');
  }
  out.append(qname.name);
}

/// Returns true if any direct child is a non-Data/non-CData element (i.e. an Element node).
/// Used to decide whether to apply block-level indentation.
bool HasElementChildren(const XMLNode& node) {
  for (std::optional<XMLNode> child = node.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Element) {
      return true;
    }
  }
  return false;
}

XMLSourceStore* GetSourceStore(EntityHandle handle) {
  return handle.registry()->ctx().get<XMLDocumentContext>().sourceStoreHolder->store.get();
}

void InvalidateAnchors(XMLSourceStore& sourceStore, SourceAnchorSpan span) {
  sourceStore.invalidateAnchor(span.start);
  sourceStore.invalidateAnchor(span.end);
}

void InvalidateOptionalAnchors(XMLSourceStore& sourceStore, std::optional<SourceAnchorSpan>& span) {
  if (span.has_value()) {
    InvalidateAnchors(sourceStore, *span);
    span = std::nullopt;
  }
}

SourceAnchorId ToAnchorId(std::uint32_t value) {
  return SourceAnchorId{value};
}

SourceAnchorSpan FullSpan(const AttributesComponent::AttributeSourceAnchors& anchors) {
  return SourceAnchorSpan{
      .start = ToAnchorId(anchors.fullStartAnchorId),
      .end = ToAnchorId(anchors.fullEndAnchorId),
  };
}

SourceAnchorSpan ValueSpan(const AttributesComponent::AttributeSourceAnchors& anchors) {
  return SourceAnchorSpan{
      .start = ToAnchorId(anchors.valueStartAnchorId),
      .end = ToAnchorId(anchors.valueEndAnchorId),
  };
}

void InvalidateAttributeAnchors(XMLSourceStore& sourceStore,
                                const AttributesComponent::AttributeSourceAnchors& anchors) {
  sourceStore.invalidateAnchor(ToAnchorId(anchors.fullStartAnchorId));
  sourceStore.invalidateAnchor(ToAnchorId(anchors.fullEndAnchorId));
  sourceStore.invalidateAnchor(ToAnchorId(anchors.valueStartAnchorId));
  sourceStore.invalidateAnchor(ToAnchorId(anchors.valueEndAnchorId));
}

std::optional<SourceRange> ResolveSourceRange(XMLSourceStore& sourceStore, SourceAnchorSpan span) {
  std::optional<ResolvedSourceSpan> resolved = sourceStore.resolveSpan(span);
  if (!resolved.has_value()) {
    return std::nullopt;
  }

  return SourceRange{
      FileOffset::Offset(resolved->start),
      FileOffset::Offset(resolved->end),
  };
}

std::optional<SourceRange> ResolveAuxiliarySourceRange(
    EntityHandle handle, const std::optional<SourceAnchorSpan>& span) {
  XMLSourceStore* sourceStore = GetSourceStore(handle);
  if (sourceStore == nullptr || !span.has_value()) {
    return std::nullopt;
  }

  return ResolveSourceRange(*sourceStore, *span);
}

void SetAuxiliarySourceAnchorSpan(EntityHandle handle, std::optional<SourceAnchorSpan>& target,
                                  SourceRange range, SourceAnchorBias startBias,
                                  SourceAnchorBias endBias) {
  XMLSourceStore* sourceStore = GetSourceStore(handle);
  if (sourceStore == nullptr || !range.start.offset.has_value() || !range.end.offset.has_value() ||
      *range.end.offset < *range.start.offset) {
    if (sourceStore != nullptr) {
      InvalidateOptionalAnchors(*sourceStore, target);
    }
    target = std::nullopt;
    return;
  }

  InvalidateOptionalAnchors(*sourceStore, target);
  target = sourceStore->createSpan(*range.start.offset, *range.end.offset, startBias, endBias);
}

void UpdateSourceAnchorSpan(EntityHandle handle, SourceOffsetComponent& offset) {
  XMLSourceStore* sourceStore = GetSourceStore(handle);
  if (sourceStore == nullptr || !offset.startOffset.has_value() || !offset.endOffset.has_value() ||
      !offset.startOffset->offset.has_value() || !offset.endOffset->offset.has_value()) {
    return;
  }

  if (offset.anchorSpan.has_value()) {
    InvalidateAnchors(*sourceStore, *offset.anchorSpan);
    offset.anchorSpan = std::nullopt;
  }

  const XMLNode::Type nodeType = handle.get<XMLNodeTypeComponent>().type();
  const SourceAnchorBias startBias =
      nodeType == XMLNode::Type::Data ? SourceAnchorBias::Before : SourceAnchorBias::After;
  const SourceAnchorBias endBias =
      nodeType == XMLNode::Type::Data ? SourceAnchorBias::After : SourceAnchorBias::Before;
  offset.anchorSpan = sourceStore->createSpan(offset.startOffset->offset.value(),
                                              offset.endOffset->offset.value(), startBias, endBias);
  if (offset.anchorSpan.has_value()) {
    offset.anchorSourceVersion = sourceStore->sourceVersion();
  }
}

/// Attach line and column numbers to a node offset recorded by the parser. The parser stores
/// plain byte offsets because resolving line numbers for every node costs more than the rest of
/// the parse; node offsets are the ones surfaced in diagnostics, so they are resolved on read.
std::optional<FileOffset> AddLineInfo(std::optional<FileOffset> offset,
                                      XMLSourceStore* sourceStore) {
  if (sourceStore == nullptr || !offset.has_value()) {
    return offset;
  }

  return sourceStore->resolveLineInfo(*offset);
}

std::optional<FileOffset> ResolveStartOffset(const SourceOffsetComponent& offset,
                                             XMLSourceStore* sourceStore) {
  if (sourceStore != nullptr && offset.anchorSpan.has_value() &&
      sourceStore->sourceVersion() != offset.anchorSourceVersion) {
    std::optional<std::size_t> resolved = sourceStore->resolveAnchor(offset.anchorSpan->start);
    if (!resolved.has_value()) {
      return std::nullopt;
    }

    return FileOffset::Offset(*resolved);
  }

  return AddLineInfo(offset.startOffset, sourceStore);
}

std::optional<FileOffset> ResolveEndOffset(const SourceOffsetComponent& offset,
                                           XMLSourceStore* sourceStore) {
  if (sourceStore != nullptr && offset.anchorSpan.has_value() &&
      sourceStore->sourceVersion() != offset.anchorSourceVersion) {
    std::optional<std::size_t> resolved = sourceStore->resolveAnchor(offset.anchorSpan->end);
    if (!resolved.has_value()) {
      return std::nullopt;
    }

    return FileOffset::Offset(*resolved);
  }

  return AddLineInfo(offset.endOffset, sourceStore);
}

}  // namespace

XMLNode::XMLNode(EntityHandle handle) : handle_(handle) {}

XMLNode XMLNode::CreateDocumentNode(XMLDocument& document) {
  const Entity entity = CreateEntity(document.registry(), Type::Document);
  return XMLNode(EntityHandle(document.registry(), entity));
}

namespace {

std::optional<ResolvedSourceSpan> ResolvedNodeSpan(EntityHandle node) {
  const auto* source = node.try_get<SourceOffsetComponent>();
  if (source == nullptr) {
    return std::nullopt;
  }
  XMLSourceStore* store = GetSourceStore(node);
  const auto start = ResolveStartOffset(*source, store);
  const auto end = ResolveEndOffset(*source, store);
  if (!start || !end || !start->offset || !end->offset || *start->offset >= *end->offset) {
    return std::nullopt;
  }
  return ResolvedSourceSpan{*start->offset, *end->offset};
}

bool SourceSpansOverlap(const std::optional<ResolvedSourceSpan>& first,
                        const ResolvedSourceSpan& second) {
  return first.has_value() && first->start < second.end && second.start < first->end;
}

void PruneProjectedTextRecords(ProjectedTextRecordsComponent& component, XMLSourceStore* store) {
  std::erase_if(component.records, [store](const auto& record) {
    if (record.store.get() != store) {
      return true;
    }
    const auto span = store->resolveSpan(record.span);
    return !span.has_value() || span->start >= span->end;
  });
}

/// A retained incarnation supersedes any older projected text overlapping its source span.
void ClearOverlappingTextProjection(EntityHandle child, Entity parent) {
  if (parent == entt::null || !child.registry()->valid(parent) ||
      !child.all_of<XMLNodeTypeComponent>()) {
    return;
  }
  const XMLNode::Type type = child.get<XMLNodeTypeComponent>().type();
  if (type != XMLNode::Type::Data && type != XMLNode::Type::CData) {
    return;
  }
  auto* records = child.registry()->try_get<ProjectedTextRecordsComponent>(parent);
  if (records == nullptr) {
    return;
  }
  XMLSourceStore* store = GetSourceStore(child);
  PruneProjectedTextRecords(*records, store);
  if (const auto span = ResolvedNodeSpan(child); span.has_value()) {
    std::erase_if(records->records, [&](const auto& record) {
      return SourceSpansOverlap(store->resolveSpan(record.span), *span);
    });
  }
}

std::shared_ptr<ProjectionRecordBudget> GetProjectionRecordBudget(Registry& registry) {
  using Budget = std::shared_ptr<ProjectionRecordBudget>;
  if (!registry.ctx().contains<Budget>()) {
    registry.ctx().emplace<Budget>(std::make_shared<ProjectionRecordBudget>());
  }
  return registry.ctx().get<Budget>();
}

void CaptureProjectedText(EntityHandle child, EntityHandle parent) {
  auto& context = child.registry()->ctx().get<XMLDocumentContext>();
  const auto store = context.sourceStoreHolder->store;
  auto* source = child.try_get<SourceOffsetComponent>();
  if (!store || !source || !source->anchorSpan) {
    return;
  }
  const auto span = store->resolveSpan(*source->anchorSpan);
  if (!span || span->start >= span->end) {
    return;
  }
  auto& records = parent.get_or_emplace<ProjectedTextRecordsComponent>();
  PruneProjectedTextRecords(records, store.get());
  const auto budget = GetProjectionRecordBudget(*child.registry());
  if (budget->liveRecords >= context.maximumSourceEditTreeNodes) {
    return;
  }
  // Transfer the existing anchors. Clearing the child retires its other source anchors;
  // parent destruction or record invalidation retires this pair exactly once.
  const SourceAnchorSpan anchors = *std::exchange(source->anchorSpan, std::nullopt);
  records.records.emplace_back(store, budget, anchors, child.entity(),
                               child.get<XMLNodeTypeComponent>().type(),
                               child.get<components::XMLValueComponent>().value);
}

}  // namespace

XMLNode XMLNode::CreateElementNode(XMLDocument& document, const XMLQualifiedNameRef& tagName) {
  const Entity entity = CreateEntity(document.registry(), Type::Element, tagName);
  return XMLNode(EntityHandle(document.registry(), entity));
}

XMLNode XMLNode::CreateElementNodeOn(XMLDocument& document, EntityHandle handle,
                                     const XMLQualifiedNameRef& tagName) {
  UTILS_RELEASE_ASSERT_MSG(handle.registry() == &document.registry(),
                           "Cannot create XMLNode on an entity from another document");

  if (handle.all_of<TreeComponent>()) {
    UTILS_RELEASE_ASSERT(handle.get<TreeComponent>().tagName() == tagName);
  } else {
    handle.emplace<TreeComponent>(tagName);
  }

  if (handle.all_of<XMLNodeTypeComponent>()) {
    UTILS_RELEASE_ASSERT(handle.get<XMLNodeTypeComponent>().type() == Type::Element);
  } else {
    handle.emplace<XMLNodeTypeComponent>(Type::Element);
  }

  return XMLNode(handle);
}

XMLNode XMLNode::CreateDataNode(XMLDocument& document, const RcStringOrRef& value) {
  const Entity entity = CreateEntity(document.registry(), Type::Data);
  auto& xmlValue = document.registry().emplace<components::XMLValueComponent>(entity);
  xmlValue.value = RcString(value);
  return XMLNode(EntityHandle(document.registry(), entity));
}

XMLNode XMLNode::CreateCDataNode(XMLDocument& document, const RcStringOrRef& value) {
  const Entity entity = CreateEntity(document.registry(), Type::CData);
  auto& xmlValue = document.registry().emplace<components::XMLValueComponent>(entity);
  xmlValue.value = RcString(value);
  return XMLNode(EntityHandle(document.registry(), entity));
}

XMLNode XMLNode::CreateCommentNode(XMLDocument& document, const RcStringOrRef& value) {
  const Entity entity = CreateEntity(document.registry(), Type::Comment);
  auto& xmlValue = document.registry().emplace<components::XMLValueComponent>(entity);
  xmlValue.value = RcString(value);
  return XMLNode(EntityHandle(document.registry(), entity));
}

XMLNode XMLNode::CreateDocTypeNode(XMLDocument& document, const RcStringOrRef& value) {
  const Entity entity = CreateEntity(document.registry(), Type::DocType);
  auto& xmlValue = document.registry().emplace<components::XMLValueComponent>(entity);
  xmlValue.value = RcString(value);
  return XMLNode(EntityHandle(document.registry(), entity));
}

XMLNode XMLNode::CreateProcessingInstructionNode(XMLDocument& document, const RcStringOrRef& target,
                                                 const RcStringOrRef& value) {
  const Entity entity = CreateEntity(document.registry(), Type::ProcessingInstruction, target);
  auto& xmlValue = document.registry().emplace<components::XMLValueComponent>(entity);
  xmlValue.value = RcString(value);
  return XMLNode(EntityHandle(document.registry(), entity));
}

XMLNode XMLNode::CreateXMLDeclarationNode(XMLDocument& document) {
  const Entity entity = CreateEntity(document.registry(), Type::XMLDeclaration);
  return XMLNode(EntityHandle(document.registry(), entity));
}

std::optional<XMLNode> XMLNode::TryCast(EntityHandle handle) {
  if (handle.all_of<TreeComponent, XMLNodeTypeComponent>()) {
    return std::make_optional(XMLNode(handle));
  } else {
    return std::nullopt;
  }
}

XMLNode::XMLNode(const XMLNode& other) = default;
XMLNode::XMLNode(XMLNode&& other) noexcept {
  *this = std::move(other);
}

XMLNode& XMLNode::operator=(const XMLNode& other) = default;
XMLNode& XMLNode::operator=(XMLNode&& other) noexcept {
  handle_ = other.handle_;
  other.handle_ = EntityHandle();
  return *this;
}

XMLNode::Type XMLNode::type() const {
  return handle_.get<XMLNodeTypeComponent>().type();
}

const XMLQualifiedName& XMLNode::tagName() const {
  return handle_.get<TreeComponent>().tagName();
}

std::optional<RcString> XMLNode::value() const {
  if (const auto* xmlValue = handle_.try_get<components::XMLValueComponent>()) {
    return xmlValue->value;
  } else {
    return std::nullopt;
  }
}

void XMLNode::setValue(const RcStringOrRef& value) {
  ClearTextProjectionProvenance(handle_);
  handle_.registry()->ctx().get<donner::components::TreeMutationContext>().setValue(handle_, value);
}

bool XMLNode::hasAttribute(const XMLQualifiedNameRef& name) const {
  return handle_.get_or_emplace<AttributesComponent>().hasAttribute(name);
}

std::optional<RcString> XMLNode::getAttribute(const XMLQualifiedNameRef& name) const {
  return handle_.get_or_emplace<AttributesComponent>().getAttribute(name);
}

std::optional<SourceRange> XMLNode::getNodeLocation() const {
  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    std::optional<FileOffset> start = ResolveStartOffset(*offset, GetSourceStore(handle_));
    std::optional<FileOffset> end = ResolveEndOffset(*offset, GetSourceStore(handle_));
    if (start.has_value() && end.has_value()) {
      return SourceRange{*start, *end};
    }
  }

  return std::nullopt;
}

std::optional<SourceRange> XMLNode::getOpeningTagLocation() const {
  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    return ResolveAuxiliarySourceRange(handle_, offset->openingTagSpan);
  }

  return std::nullopt;
}

std::optional<SourceRange> XMLNode::getClosingTagLocation() const {
  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    return ResolveAuxiliarySourceRange(handle_, offset->closingTagSpan);
  }

  return std::nullopt;
}

std::optional<SourceRange> XMLNode::getValueLocation() const {
  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    return ResolveAuxiliarySourceRange(handle_, offset->valueSpan);
  }

  return std::nullopt;
}

std::optional<SourceRange> XMLNode::getAttributeLocation(std::string_view xmlInput,
                                                         const XMLQualifiedNameRef& name) const {
  if (std::optional<XMLAttributeSourceLocation> sourceLocation = getAttributeSourceLocation(name)) {
    return sourceLocation->fullRange;
  }

  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    std::optional<FileOffset> start = ResolveStartOffset(*offset, GetSourceStore(handle_));
    if (start.has_value()) {
      if (auto maybeLocation = XMLParser::GetAttributeLocation(xmlInput, start.value(), name)) {
        return maybeLocation;
      }
    }
  }

  return std::nullopt;
}

std::optional<XMLAttributeSourceLocation> XMLNode::getAttributeSourceLocation(
    const XMLQualifiedNameRef& name) const {
  const auto* attributes = handle_.try_get<AttributesComponent>();
  if (attributes == nullptr) {
    return std::nullopt;
  }

  std::optional<AttributesComponent::AttributeSourceAnchors> anchors =
      attributes->getAttributeSourceAnchors(name);
  XMLSourceStore* sourceStore = GetSourceStore(handle_);
  if (!anchors.has_value() || !anchors->isValid() || sourceStore == nullptr) {
    return std::nullopt;
  }

  std::optional<SourceRange> fullRange = ResolveSourceRange(*sourceStore, FullSpan(*anchors));
  std::optional<SourceRange> valueRange = ResolveSourceRange(*sourceStore, ValueSpan(*anchors));
  if (!fullRange.has_value() || !valueRange.has_value()) {
    return std::nullopt;
  }

  return XMLAttributeSourceLocation{
      .fullRange = *fullRange,
      .valueRange = *valueRange,
      .quote = anchors->quote,
  };
}

void XMLNode::setAttributeSourceLocation(const XMLQualifiedNameRef& name, SourceRange fullRange,
                                         SourceRange valueRange, char quote) {
  auto& attributes = handle_.get_or_emplace<AttributesComponent>();

  // Every step below - the existence check, the read of the previous anchors, the write of the new
  // ones, and the clear on each failure path - concerns the same attribute. Resolve it once; the
  // attribute set is not modified in between, so the slot stays valid for the whole function.
  std::optional<AttributesComponent::AttributeSourceAnchors>* anchorSlot =
      attributes.attributeSourceAnchorsSlot(name);
  if (anchorSlot == nullptr) {
    return;
  }

  XMLSourceStore* sourceStore = GetSourceStore(handle_);

  // Drop whatever anchors the attribute currently has, releasing them from the source store.
  const auto clearAnchors = [sourceStore, anchorSlot]() {
    if (sourceStore != nullptr && anchorSlot->has_value()) {
      InvalidateAttributeAnchors(*sourceStore, **anchorSlot);
    }
    anchorSlot->reset();
  };

  if (sourceStore == nullptr || !fullRange.start.offset.has_value() ||
      !fullRange.end.offset.has_value() || !valueRange.start.offset.has_value() ||
      !valueRange.end.offset.has_value()) {
    clearAnchors();
    return;
  }

  const std::size_t fullStart = *fullRange.start.offset;
  const std::size_t fullEnd = *fullRange.end.offset;
  const std::size_t valueStart = *valueRange.start.offset;
  const std::size_t valueEnd = *valueRange.end.offset;
  if (fullEnd < fullStart || valueEnd < valueStart || valueStart < fullStart ||
      valueEnd > fullEnd) {
    clearAnchors();
    return;
  }

  std::optional<SourceAnchorSpan> fullSpan = sourceStore->createSpan(
      fullStart, fullEnd, SourceAnchorBias::After, SourceAnchorBias::Before);
  if (!fullSpan.has_value()) {
    clearAnchors();
    return;
  }

  std::optional<SourceAnchorSpan> valueSpan = sourceStore->createSpan(
      valueStart, valueEnd, SourceAnchorBias::Before, SourceAnchorBias::After);
  if (!valueSpan.has_value()) {
    InvalidateAnchors(*sourceStore, *fullSpan);
    clearAnchors();
    return;
  }

  if (anchorSlot->has_value()) {
    InvalidateAttributeAnchors(*sourceStore, **anchorSlot);
  }

  *anchorSlot = AttributesComponent::AttributeSourceAnchors{
      .fullStartAnchorId = fullSpan->start.value,
      .fullEndAnchorId = fullSpan->end.value,
      .valueStartAnchorId = valueSpan->start.value,
      .valueEndAnchorId = valueSpan->end.value,
      .quote = (quote == '\'' || quote == '"') ? quote : '"',
  };
}

void XMLNode::clearAttributeSourceLocation(const XMLQualifiedNameRef& name) {
  auto* attributes = handle_.try_get<AttributesComponent>();
  if (attributes == nullptr) {
    return;
  }

  XMLSourceStore* sourceStore = GetSourceStore(handle_);
  if (sourceStore != nullptr) {
    if (std::optional<AttributesComponent::AttributeSourceAnchors> anchors =
            attributes->getAttributeSourceAnchors(name)) {
      InvalidateAttributeAnchors(*sourceStore, *anchors);
    }
  }

  attributes->clearAttributeSourceAnchors(name);
}

SmallVector<XMLQualifiedNameRef, 10> XMLNode::attributes() const {
  return handle_.get_or_emplace<AttributesComponent>().attributes();
}

std::optional<RcString> XMLNode::getNamespaceUri(const RcString& prefix) const {
  return handle_.registry()->ctx().get<XMLNamespaceContext>().getNamespaceUri(
      *handle_.registry(), handle_.entity(), prefix);
}

void XMLNode::setAttribute(const XMLQualifiedNameRef& name, std::string_view value) {
  return handle_.get_or_emplace<AttributesComponent>().setAttribute(*handle_.registry(), name,
                                                                    RcString(value));
}

void XMLNode::removeAttribute(const XMLQualifiedNameRef& name) {
  clearAttributeSourceLocation(name);
  handle_.get_or_emplace<AttributesComponent>().removeAttribute(*handle_.registry(), name);
}

namespace {

/**
 * Returns true if \p entity holds XML node data, i.e. it is part of the authored XML document.
 *
 * @param registry Registry holding the tree.
 * @param entity Entity to test, which may be \c entt::null.
 * @return Whether \p entity is an XML node.
 */
bool IsXmlNodeEntity(const Registry& registry, Entity entity) {
  return entity != entt::null && registry.all_of<TreeComponent, XMLNodeTypeComponent>(entity);
}

/**
 * Advance from \p entity along \p step until reaching an entity that is part of the authored XML
 * document, or running off the end of the tree.
 *
 * Tree storage is shared with the layers above XML, which attach entities that hold no XML node
 * data: elements created through the SVG DOM before they are projected, and the shadow-tree
 * entities rendering attaches under their host element. Every XML accessor on those is invalid, so
 * the tree accessors below step over them instead of handing them to callers.
 *
 * @param registry Registry holding the tree.
 * @param entity Entity to start from, which may be \c entt::null.
 * @param step Tree link to follow, e.g. \ref TreeComponent::nextSibling.
 * @return The first XML node at or beyond \p entity, or \c entt::null if there is none.
 */
Entity XmlNodeAtOrBeyond(const Registry& registry, Entity entity,
                         Entity (TreeComponent::*step)() const) {
  while (entity != entt::null) {
    const auto* tree = registry.try_get<TreeComponent>(entity);
    if (tree == nullptr) {
      return entt::null;
    }
    if (registry.all_of<XMLNodeTypeComponent>(entity)) {
      return entity;
    }
    entity = (tree->*step)();
  }
  return entt::null;
}

}  // namespace

std::optional<XMLNode> XMLNode::toOptionalNode(Entity entity) const {
  return entity != entt::null ? std::make_optional(XMLNode(toHandle(entity))) : std::nullopt;
}

std::optional<XMLNode> XMLNode::parentElement() const {
  const Entity parent = handle_.get<TreeComponent>().parent();
  return toOptionalNode(IsXmlNodeEntity(registry(), parent) ? parent : entt::null);
}

std::optional<XMLNode> XMLNode::firstChild() const {
  return toOptionalNode(XmlNodeAtOrBeyond(registry(), handle_.get<TreeComponent>().firstChild(),
                                          &TreeComponent::nextSibling));
}

std::optional<XMLNode> XMLNode::lastChild() const {
  return toOptionalNode(XmlNodeAtOrBeyond(registry(), handle_.get<TreeComponent>().lastChild(),
                                          &TreeComponent::previousSibling));
}

std::optional<XMLNode> XMLNode::previousSibling() const {
  return toOptionalNode(XmlNodeAtOrBeyond(
      registry(), handle_.get<TreeComponent>().previousSibling(), &TreeComponent::previousSibling));
}

std::optional<XMLNode> XMLNode::nextSibling() const {
  return toOptionalNode(XmlNodeAtOrBeyond(registry(), handle_.get<TreeComponent>().nextSibling(),
                                          &TreeComponent::nextSibling));
}

// `TreeMutationContext` is an invariant of any registry created through the document facades
// (XMLDocument's ctor installs the basic XML defaults; SVGDocument overrides them with the
// SVG-specific callbacks). We can therefore call through the context unconditionally instead of
// gating on `find<TreeMutationContext>()` + falling back to a direct TreeComponent path.
void XMLNode::insertBefore(const XMLNode& newNode, std::optional<XMLNode> referenceNode) {
  ClearTextProjectionProvenance(newNode.handle_);
  ClearOverlappingTextProjection(newNode.handle_, handle_.entity());
  auto& mutations = handle_.registry()->ctx().get<donner::components::TreeMutationContext>();
  mutations.insertBefore(handle_, newNode.handle_,
                         referenceNode ? referenceNode->handle_ : EntityHandle());
}

void XMLNode::appendChild(const XMLNode& child) {
  ClearTextProjectionProvenance(child.handle_);
  ClearOverlappingTextProjection(child.handle_, handle_.entity());
  auto& mutations = handle_.registry()->ctx().get<donner::components::TreeMutationContext>();
  mutations.appendChild(handle_, child.handle_);
}

void XMLNode::replaceChild(const XMLNode& newChild, const XMLNode& oldChild) {
  ClearTextProjectionProvenance(newChild.handle_);
  ClearTextProjectionProvenance(oldChild.handle_);
  ClearOverlappingTextProjection(newChild.handle_, handle_.entity());
  ClearOverlappingTextProjection(oldChild.handle_, handle_.entity());
  auto& mutations = handle_.registry()->ctx().get<donner::components::TreeMutationContext>();
  mutations.replaceChild(handle_, newChild.handle_, oldChild.handle_);
}

void XMLNode::removeChild(const XMLNode& child) {
  ClearTextProjectionProvenance(child.handle_);
  ClearOverlappingTextProjection(child.handle_, handle_.entity());
  auto& mutations = handle_.registry()->ctx().get<donner::components::TreeMutationContext>();
  mutations.removeChild(handle_, child.handle_);
}

void XMLNode::remove() {
  ClearTextProjectionProvenance(handle_);
  ClearOverlappingTextProjection(handle_, handle_.get<TreeComponent>().parent());
  auto& mutations = handle_.registry()->ctx().get<donner::components::TreeMutationContext>();
  mutations.remove(handle_);
}

void XMLNode::removeForProjection() {
  const bool isText = type() == Type::Data || type() == Type::CData;
  const auto parent = parentElement();
  ClearTextProjectionProvenance(handle_);
  ClearOverlappingTextProjection(handle_, handle_.get<TreeComponent>().parent());
  if (isText && parent.has_value()) {
    CaptureProjectedText(handle_, parent->handle_);
  }
  // The SVG mutation callback can collect this node synchronously. Do not access it afterward.
  clearSourceLocation();
  if (isText && parent.has_value()) {
    handle_.emplace_or_replace<ProjectedTextParentComponent>(parent->handle_.entity());
  }
  auto& mutations = handle_.registry()->ctx().get<donner::components::TreeMutationContext>();
  mutations.remove(handle_);
}

std::vector<XMLNode::ProjectedTextChild> XMLNode::projectedTextChildren() const {
  std::vector<ProjectedTextChild> result;
  auto* records = handle_.try_get<ProjectedTextRecordsComponent>();
  if (records == nullptr) {
    return result;
  }
  XMLSourceStore* store = GetSourceStore(handle_);
  PruneProjectedTextRecords(*records, store);
  result.reserve(records->records.size());
  for (const auto& record : records->records) {
    const auto span = store->resolveSpan(record.span);
    result.push_back(ProjectedTextChild{record.type, record.value, span->start, span->end});
  }
  std::sort(result.begin(), result.end(),
            [](const auto& left, const auto& right) { return left.start < right.start; });
  return result;
}

std::optional<FileOffset> XMLNode::sourceStartOffset() const {
  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    return ResolveStartOffset(*offset, GetSourceStore(handle_));
  } else {
    return std::nullopt;
  }
}

void XMLNode::setSourceStartOffset(FileOffset offset) {
  auto& sourceOffset = handle_.get_or_emplace<SourceOffsetComponent>();
  sourceOffset.startOffset = offset;
  UpdateSourceAnchorSpan(handle_, sourceOffset);
  ClearOverlappingTextProjection(handle_, handle_.get<TreeComponent>().parent());
}

std::optional<FileOffset> XMLNode::sourceEndOffset() const {
  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    return ResolveEndOffset(*offset, GetSourceStore(handle_));
  } else {
    return std::nullopt;
  }
}

void XMLNode::setSourceEndOffset(FileOffset offset) {
  auto& sourceOffset = handle_.get_or_emplace<SourceOffsetComponent>();
  sourceOffset.endOffset = offset;
  UpdateSourceAnchorSpan(handle_, sourceOffset);
  ClearOverlappingTextProjection(handle_, handle_.get<TreeComponent>().parent());
}

void XMLNode::setOpeningTagLocation(SourceRange range) {
  auto& sourceOffset = handle_.get_or_emplace<SourceOffsetComponent>();
  SetAuxiliarySourceAnchorSpan(handle_, sourceOffset.openingTagSpan, range, SourceAnchorBias::After,
                               SourceAnchorBias::Before);
}

void XMLNode::clearOpeningTagLocation() {
  auto* sourceOffset = handle_.try_get<SourceOffsetComponent>();
  XMLSourceStore* sourceStore = GetSourceStore(handle_);
  if (sourceOffset != nullptr && sourceStore != nullptr) {
    InvalidateOptionalAnchors(*sourceStore, sourceOffset->openingTagSpan);
  } else if (sourceOffset != nullptr) {
    sourceOffset->openingTagSpan = std::nullopt;
  }
}

void XMLNode::setClosingTagLocation(SourceRange range) {
  auto& sourceOffset = handle_.get_or_emplace<SourceOffsetComponent>();
  SetAuxiliarySourceAnchorSpan(handle_, sourceOffset.closingTagSpan, range, SourceAnchorBias::After,
                               SourceAnchorBias::Before);
}

void XMLNode::clearClosingTagLocation() {
  auto* sourceOffset = handle_.try_get<SourceOffsetComponent>();
  XMLSourceStore* sourceStore = GetSourceStore(handle_);
  if (sourceOffset != nullptr && sourceStore != nullptr) {
    InvalidateOptionalAnchors(*sourceStore, sourceOffset->closingTagSpan);
  } else if (sourceOffset != nullptr) {
    sourceOffset->closingTagSpan = std::nullopt;
  }
}

void XMLNode::setValueLocation(SourceRange range) {
  auto& sourceOffset = handle_.get_or_emplace<SourceOffsetComponent>();
  SetAuxiliarySourceAnchorSpan(handle_, sourceOffset.valueSpan, range, SourceAnchorBias::Before,
                               SourceAnchorBias::After);
}

void XMLNode::clearValueLocation() {
  auto* sourceOffset = handle_.try_get<SourceOffsetComponent>();
  XMLSourceStore* sourceStore = GetSourceStore(handle_);
  if (sourceOffset != nullptr && sourceStore != nullptr) {
    InvalidateOptionalAnchors(*sourceStore, sourceOffset->valueSpan);
  } else if (sourceOffset != nullptr) {
    sourceOffset->valueSpan = std::nullopt;
  }
}

void XMLNode::clearSourceLocation() {
  ClearTextProjectionProvenance(handle_);
  handle_.remove<ProjectedTextRecordsComponent>();
  if (auto* attributes = handle_.try_get<AttributesComponent>()) {
    SmallVector<XMLQualifiedNameRef, 10> names = attributes->attributes();
    for (const XMLQualifiedNameRef& name : names) {
      clearAttributeSourceLocation(name);
    }
  }

  auto* sourceOffset = handle_.try_get<SourceOffsetComponent>();
  if (sourceOffset == nullptr) {
    return;
  }

  XMLSourceStore* sourceStore = GetSourceStore(handle_);
  if (sourceStore != nullptr) {
    InvalidateOptionalAnchors(*sourceStore, sourceOffset->anchorSpan);
    InvalidateOptionalAnchors(*sourceStore, sourceOffset->openingTagSpan);
    InvalidateOptionalAnchors(*sourceStore, sourceOffset->closingTagSpan);
    InvalidateOptionalAnchors(*sourceStore, sourceOffset->valueSpan);
  }

  handle_.remove<SourceOffsetComponent>();
}

RcString XMLNode::serializeToString(int indentLevel, bool prettyPrint) const {
  std::string indent(prettyPrint ? static_cast<size_t>(indentLevel) * 2 : 0, ' ');
  std::string out;

  switch (type()) {
    case Type::Document: {
      // Serialize all children of the document node directly.
      for (std::optional<XMLNode> child = firstChild(); child.has_value();
           child = child->nextSibling()) {
        out.append(child->serializeToString(indentLevel, prettyPrint));
        if (prettyPrint) {
          out.push_back('\n');
        }
      }
      break;
    }

    case Type::Element: {
      const XMLQualifiedNameRef tag = tagName();
      out.append(indent);
      out.push_back('<');
      AppendQualifiedName(out, tag);

      // Emit attributes.
      for (const XMLQualifiedNameRef& attrName : attributes()) {
        std::optional<RcString> attrValue = getAttribute(attrName);
        if (!attrValue.has_value()) {
          continue;
        }
        std::optional<RcString> escaped = EscapeAttributeValue(*attrValue, '"');
        if (!escaped.has_value()) {
          // Cannot represent this value in well-formed XML; skip attribute.
          continue;
        }
        out.push_back(' ');
        AppendQualifiedName(out, attrName);
        out.append("=\"");
        out.append(*escaped);
        out.push_back('"');
      }

      const std::optional<XMLNode> firstChildNode = firstChild();
      if (!firstChildNode.has_value()) {
        // Self-closing empty element.
        out.append("/>");
      } else {
        out.push_back('>');

        // Decide whether to apply block indentation.  If all children are text/cdata we keep
        // everything inline; if any child is an Element we indent.
        const bool blockIndent = prettyPrint && HasElementChildren(*this);

        for (std::optional<XMLNode> child = firstChildNode; child.has_value();
             child = child->nextSibling()) {
          if (blockIndent) {
            out.push_back('\n');
            out.append(child->serializeToString(indentLevel + 1, prettyPrint));
          } else {
            out.append(child->serializeToString(0, prettyPrint));
          }
        }

        if (blockIndent) {
          out.push_back('\n');
          out.append(indent);
        }
        out.append("</");
        AppendQualifiedName(out, tag);
        out.push_back('>');
      }
      break;
    }

    case Type::Data: {
      const std::optional<RcString> val = value();
      if (val.has_value()) {
        out.append(indent);
        if (std::optional<RcString> escaped = donner::xml::EscapeTextContent(*val);
            escaped.has_value()) {
          out.append(*escaped);
        } else {
          // Best-effort serialization for values that are not representable in
          // well-formed XML character data.
          out.append(*val);
        }
      }
      break;
    }

    case Type::CData: {
      const std::optional<RcString> val = value();
      out.append(indent);
      out.append("<![CDATA[");
      if (val.has_value()) {
        out.append(*val);
      }
      out.append("]]>");
      break;
    }

    case Type::Comment: {
      const std::optional<RcString> val = value();
      out.append(indent);
      out.append("<!--");
      if (val.has_value()) {
        out.append(*val);
      }
      out.append("-->");
      break;
    }

    case Type::DocType: {
      const std::optional<RcString> val = value();
      out.append(indent);
      out.append("<!DOCTYPE ");
      if (val.has_value()) {
        out.append(*val);
      }
      out.push_back('>');
      break;
    }

    case Type::ProcessingInstruction: {
      const std::optional<RcString> val = value();
      out.append(indent);
      out.append("<?");
      AppendQualifiedName(out, tagName());
      if (val.has_value() && !val->empty()) {
        out.push_back(' ');
        out.append(*val);
      }
      out.append("?>");
      break;
    }

    case Type::XMLDeclaration: {
      out.append(indent);
      out.append("<?xml");
      for (const XMLQualifiedNameRef& attrName : attributes()) {
        std::optional<RcString> attrValue = getAttribute(attrName);
        if (!attrValue.has_value()) {
          continue;
        }
        std::optional<RcString> escaped = EscapeAttributeValue(*attrValue, '"');
        if (!escaped.has_value()) {
          continue;
        }
        out.push_back(' ');
        AppendQualifiedName(out, attrName);
        out.append("=\"");
        out.append(*escaped);
        out.push_back('"');
      }
      out.append("?>");
      break;
    }
  }

  return RcString(out);
}

Entity XMLNode::CreateEntity(Registry& registry, Type type, const XMLQualifiedNameRef& tagName) {
  Entity entity = registry.create();
  registry.emplace<TreeComponent>(entity, tagName);
  registry.emplace<XMLNodeTypeComponent>(entity, type);
  return EntityHandle(registry, entity);
}

}  // namespace donner::xml
