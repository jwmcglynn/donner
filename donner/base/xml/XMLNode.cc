#include "donner/base/xml/XMLNode.h"

#include <string>

#include "donner/base/FileOffset.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLEscape.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
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
  std::uint64_t anchorSourceVersion = 0;
};

/// Escape text content (Data nodes): escape `<`, `>`, and `&`, but not quotes since
/// we are not in an attribute context.
std::string EscapeTextContent(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char ch : text) {
    switch (ch) {
      case '<': out.append("&lt;"); break;
      case '>': out.append("&gt;"); break;
      case '&': out.append("&amp;"); break;
      default: out.push_back(ch); break;
    }
  }
  return out;
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
  return handle.registry()->ctx().get<XMLDocumentContext>().sourceStore.get();
}

void InvalidateAnchors(XMLSourceStore& sourceStore, SourceAnchorSpan span) {
  sourceStore.invalidateAnchor(span.start);
  sourceStore.invalidateAnchor(span.end);
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

  return offset.startOffset;
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

  return offset.endOffset;
}

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

std::optional<SourceRange> XMLNode::getAttributeLocation(std::string_view xmlInput,
                                                         const XMLQualifiedNameRef& name) const {
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
    return ResolveStartOffset(*offset, GetSourceStore(handle_));
  } else {
    return std::nullopt;
  }
}

void XMLNode::setSourceStartOffset(FileOffset offset) {
  auto& sourceOffset = handle_.get_or_emplace<SourceOffsetComponent>();
  sourceOffset.startOffset = offset;
  UpdateSourceAnchorSpan(handle_, sourceOffset);
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
}

RcString XMLNode::serializeToString(int indentLevel) const {
  std::string indent(static_cast<size_t>(indentLevel) * 2, ' ');
  std::string out;

  switch (type()) {
    case Type::Document: {
      // Serialize all children of the document node directly.
      for (std::optional<XMLNode> child = firstChild(); child.has_value();
           child = child->nextSibling()) {
        out.append(child->serializeToString(indentLevel));
        out.push_back('\n');
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
        const bool blockIndent = HasElementChildren(*this);

        for (std::optional<XMLNode> child = firstChildNode; child.has_value();
             child = child->nextSibling()) {
          if (blockIndent) {
            out.push_back('\n');
            out.append(child->serializeToString(indentLevel + 1));
          } else {
            out.append(child->serializeToString(0));
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
        out.append(EscapeTextContent(*val));
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
