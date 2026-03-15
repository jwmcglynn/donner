#include "donner/base/xml/XMLNode.h"

#include "donner/base/FileOffset.h"
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
struct SourceOffsetComponent {
  std::optional<FileOffset> startOffset;
  std::optional<FileOffset> endOffset;
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
    if (offset->startOffset) {
      if (auto maybeLocation =
              XMLParser::GetAttributeLocation(xmlInput, offset->startOffset.value(), name)) {
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

Entity XMLNode::CreateEntity(Registry& registry, Type type, const XMLQualifiedNameRef& tagName) {
  Entity entity = registry.create();
  registry.emplace<TreeComponent>(entity, tagName);
  registry.emplace<XMLNodeTypeComponent>(entity, type);
  return EntityHandle(registry, entity);
}

namespace {

/// Escape XML special characters in text content.
void appendEscaped(std::vector<char>& buf, std::string_view text) {
  for (const char c : text) {
    switch (c) {
      case '&': buf.insert(buf.end(), {'&', 'a', 'm', 'p', ';'}); break;
      case '<': buf.insert(buf.end(), {'&', 'l', 't', ';'}); break;
      case '>': buf.insert(buf.end(), {'&', 'g', 't', ';'}); break;
      case '"': buf.insert(buf.end(), {'&', 'q', 'u', 'o', 't', ';'}); break;
      default: buf.push_back(c); break;
    }
  }
}

void appendIndent(std::vector<char>& buf, int level) {
  for (int i = 0; i < level * 2; ++i) {
    buf.push_back(' ');
  }
}

void appendString(std::vector<char>& buf, std::string_view str) {
  buf.insert(buf.end(), str.begin(), str.end());
}

void serializeNodeImpl(const XMLNode& node, std::vector<char>& buf, int indent) {
  switch (node.type()) {
    case XMLNode::Type::Element: {
      appendIndent(buf, indent);
      buf.push_back('<');
      const auto tag = node.tagName();
      appendString(buf, tag.toString());

      // Attributes.
      for (const auto& attrName : node.attributes()) {
        buf.push_back(' ');
        appendString(buf, attrName.toString());
        buf.push_back('=');
        buf.push_back('"');
        if (auto val = node.getAttribute(attrName)) {
          appendEscaped(buf, *val);
        }
        buf.push_back('"');
      }

      // Children.
      auto child = node.firstChild();
      if (!child.has_value()) {
        // Self-closing.
        appendString(buf, "/>");
      } else {
        buf.push_back('>');

        // Check if only text children (inline content).
        bool hasElementChildren = false;
        for (auto c = child; c.has_value(); c = c->nextSibling()) {
          if (c->type() == XMLNode::Type::Element) {
            hasElementChildren = true;
            break;
          }
        }

        if (hasElementChildren) {
          buf.push_back('\n');
          for (auto c = child; c.has_value(); c = c->nextSibling()) {
            serializeNodeImpl(*c, buf, indent + 1);
            buf.push_back('\n');
          }
          appendIndent(buf, indent);
        } else {
          // Inline text children.
          for (auto c = child; c.has_value(); c = c->nextSibling()) {
            serializeNodeImpl(*c, buf, 0);
          }
        }

        appendString(buf, "</");
        appendString(buf, tag.toString());
        buf.push_back('>');
      }
      break;
    }
    case XMLNode::Type::Data: {
      if (auto val = node.value()) {
        appendEscaped(buf, *val);
      }
      break;
    }
    case XMLNode::Type::CData: {
      appendString(buf, "<![CDATA[");
      if (auto val = node.value()) {
        appendString(buf, *val);
      }
      appendString(buf, "]]>");
      break;
    }
    case XMLNode::Type::Comment: {
      appendIndent(buf, indent);
      appendString(buf, "<!--");
      if (auto val = node.value()) {
        appendString(buf, *val);
      }
      appendString(buf, "-->");
      break;
    }
    default:
      break;
  }
}

}  // namespace

RcString XMLNode::serializeToString(int indentLevel) const {
  std::vector<char> buf;
  buf.reserve(256);
  serializeNodeImpl(*this, buf, indentLevel);
  return RcString::fromVector(std::move(buf));
}

}  // namespace donner::xml
