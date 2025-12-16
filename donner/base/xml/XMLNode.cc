#include "donner/base/xml/XMLNode.h"

#include "donner/base/FileOffset.h"
#include "donner/base/SmallVector.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"
#include "donner/base/xml/components/XMLValueComponent.h"

namespace donner::xml {

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
struct AttributeSourceOffset {
  FileOffsetRange fullRange;
  FileOffsetRange valueRange;
};

struct SourceOffsetComponent {
  std::optional<FileOffset> startOffset;
  std::optional<FileOffset> endOffset;
  std::optional<FileOffsetRange> valueRange;
  SmallVector<std::pair<XMLQualifiedName, AttributeSourceOffset>, 8> attributeOffsets;
};

}  // namespace

XMLNode::XMLNode(EntityHandle handle) : handle_(handle) {}

XMLNode XMLNode::CreateDocumentNode(XMLDocument& document) {
  const Entity entity = CreateEntity(document.registry(), Type::Document);
  return XMLNode(EntityHandle(document.registry(), entity));
}

XMLNode XMLNode::CreateElementNode(XMLDocument& document, const XMLQualifiedNameRef& tagName) {
  const Entity entity = CreateEntity(document.registry(), Type::Element, tagName);
  return XMLNode(EntityHandle(document.registry(), entity));
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

XMLQualifiedNameRef XMLNode::tagName() const {
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
  handle_.get_or_emplace<components::XMLValueComponent>().value = RcString(value);
}

std::optional<EditOperation> XMLNode::setValuePreserveSource(const RcStringOrRef& value) {
  setValue(value);

  if (const auto* offsets = handle_.try_get<SourceOffsetComponent>()) {
    if (offsets->valueRange.has_value()) {
      return EditOperation::ReplaceValue(*offsets->valueRange, RcString(value));
    }
  }

  return std::nullopt;
}

bool XMLNode::hasAttribute(const XMLQualifiedNameRef& name) const {
  return handle_.get_or_emplace<AttributesComponent>().hasAttribute(name);
}

std::optional<RcString> XMLNode::getAttribute(const XMLQualifiedNameRef& name) const {
  return handle_.get_or_emplace<AttributesComponent>().getAttribute(name);
}

std::optional<FileOffsetRange> XMLNode::getNodeLocation() const {
  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    if (offset->startOffset && offset->endOffset) {
      return FileOffsetRange{*offset->startOffset, *offset->endOffset};
    }
  }

  return std::nullopt;
}

std::optional<FileOffsetRange> XMLNode::getAttributeLocation(
    std::string_view xmlInput, const XMLQualifiedNameRef& name) const {
  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    for (const auto& [storedName, location] : offset->attributeOffsets) {
      if (storedName == name) {
        return location.fullRange;
      }
    }

    if (offset->startOffset) {
      if (auto maybeLocation =
              XMLParser::GetAttributeLocation(xmlInput, offset->startOffset.value(), name)) {
        return maybeLocation;
      }
    }
  }

  return std::nullopt;
}

std::optional<FileOffsetRange> XMLNode::getValueLocation() const {
  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    return offset->valueRange;
  }

  return std::nullopt;
}

std::optional<FileOffsetRange> XMLNode::getAttributeValueLocation(
    const XMLQualifiedNameRef& name) const {
  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    for (const auto& [storedName, storedLocation] : offset->attributeOffsets) {
      if (storedName == name) {
        return storedLocation.valueRange;
      }
    }
  }

  return std::nullopt;
}

void XMLNode::addAttributeLocation(const XMLQualifiedNameRef& name, FileOffsetRange location,
                                   FileOffsetRange valueRange) {
  auto& offsets = handle_.get_or_emplace<SourceOffsetComponent>().attributeOffsets;
  for (auto& [storedName, storedLocation] : offsets) {
    if (storedName == name) {
      storedLocation = AttributeSourceOffset{location, valueRange};
      return;
    }
  }

  offsets.emplace_back(XMLQualifiedName(name.namespacePrefix, name.name),
                       AttributeSourceOffset{location, valueRange});
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

std::optional<EditOperation> XMLNode::setAttributePreserveSource(const XMLQualifiedNameRef& name,
                                                                 std::string_view value) {
  setAttribute(name, value);

  if (auto valueLocation = getAttributeValueLocation(name)) {
    return EditOperation::ReplaceValue(*valueLocation, RcString(value));
  }

  return std::nullopt;
}

void XMLNode::removeAttribute(const XMLQualifiedNameRef& name) {
  handle_.get_or_emplace<AttributesComponent>().removeAttribute(*handle_.registry(), name);
}

std::optional<XMLNode> XMLNode::parentElement() const {
  const auto& tree = handle_.get<TreeComponent>();
  return tree.parent() != entt::null ? std::make_optional(XMLNode(toHandle(tree.parent())))
                                     : std::nullopt;
}

std::optional<XMLNode> XMLNode::firstChild() const {
  const auto& tree = handle_.get<TreeComponent>();
  return tree.firstChild() != entt::null ? std::make_optional(XMLNode(toHandle(tree.firstChild())))
                                         : std::nullopt;
}

std::optional<XMLNode> XMLNode::lastChild() const {
  const auto& tree = handle_.get<TreeComponent>();
  return tree.lastChild() != entt::null ? std::make_optional(XMLNode(toHandle(tree.lastChild())))
                                        : std::nullopt;
}

std::optional<XMLNode> XMLNode::previousSibling() const {
  const auto& tree = handle_.get<TreeComponent>();
  return tree.previousSibling() != entt::null
             ? std::make_optional(XMLNode(toHandle(tree.previousSibling())))
             : std::nullopt;
}

std::optional<XMLNode> XMLNode::nextSibling() const {
  const auto& tree = handle_.get<TreeComponent>();
  return tree.nextSibling() != entt::null
             ? std::make_optional(XMLNode(toHandle(tree.nextSibling())))
             : std::nullopt;
}

void XMLNode::insertBefore(const XMLNode& newNode, std::optional<XMLNode> referenceNode) {
  handle_.get<TreeComponent>().insertBefore(
      registry(), newNode.handle_.entity(),
      referenceNode ? referenceNode->handle_.entity() : entt::null);
}

void XMLNode::appendChild(const XMLNode& child) {
  handle_.get<TreeComponent>().appendChild(registry(), child.handle_.entity());
}

void XMLNode::replaceChild(const XMLNode& newChild, const XMLNode& oldChild) {
  handle_.get<TreeComponent>().replaceChild(registry(), newChild.handle_.entity(),
                                            oldChild.handle_.entity());
}

void XMLNode::removeChild(const XMLNode& child) {
  handle_.get<TreeComponent>().removeChild(registry(), child.entityHandle().entity());
}

void XMLNode::remove() {
  handle_.get<TreeComponent>().remove(registry());
}

std::optional<FileOffset> XMLNode::sourceStartOffset() const {
  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    return offset->startOffset;
  } else {
    return std::nullopt;
  }
}

void XMLNode::setSourceStartOffset(FileOffset offset) {
  handle_.get_or_emplace<SourceOffsetComponent>().startOffset = offset;
}

std::optional<FileOffset> XMLNode::sourceEndOffset() const {
  if (const auto* offset = handle_.try_get<SourceOffsetComponent>()) {
    return offset->endOffset;
  } else {
    return std::nullopt;
  }
}

void XMLNode::setSourceEndOffset(FileOffset offset) {
  handle_.get_or_emplace<SourceOffsetComponent>().endOffset = offset;
}

void XMLNode::setValueSourceRange(FileOffsetRange range) {
  handle_.get_or_emplace<SourceOffsetComponent>().valueRange = range;
}

Entity XMLNode::CreateEntity(Registry& registry, Type type, const XMLQualifiedNameRef& tagName) {
  Entity entity = registry.create();
  registry.emplace<TreeComponent>(entity, tagName);
  registry.emplace<XMLNodeTypeComponent>(entity, type);
  return EntityHandle(registry, entity);
}

}  // namespace donner::xml
