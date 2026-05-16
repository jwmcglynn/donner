#include "donner/base/xml/XMLDocument.h"

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "donner/base/xml/XMLEscape.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/components/XMLDocumentContext.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"

namespace donner::xml {

using components::XMLDocumentContext;
using components::XMLNamespaceContext;

namespace {

struct SourceEditRange {
  std::size_t start = 0;
  std::size_t end = 0;
};

struct AttributeValueEdit {
  XMLNode node;
  XMLQualifiedName name;
  SourceRange attributeLocation;
  std::size_t valueStart = 0;
  std::size_t valueEnd = 0;
  char quote = '"';
};

struct OpeningTagEdit {
  XMLNode node;
  std::size_t tagStart = 0;
  std::size_t tagEnd = 0;
};

struct TextNodeEdit {
  XMLNode node;
  bool elementTextContent = false;
};

using AttributeMap = std::map<XMLQualifiedName, RcString>;

bool IsXmlWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

bool ContainsOffset(const SourceRange& range, std::size_t offset) {
  if (!range.start.offset.has_value() || !range.end.offset.has_value()) {
    return false;
  }

  return *range.start.offset <= offset && offset < *range.end.offset;
}

std::optional<std::size_t> FindOpeningTagEnd(std::string_view source, std::size_t tagStart) {
  if (tagStart >= source.size() || source[tagStart] != '<') {
    return std::nullopt;
  }

  bool inSingleQuote = false;
  bool inDoubleQuote = false;
  for (std::size_t pos = tagStart + 1; pos < source.size(); ++pos) {
    const char ch = source[pos];
    if (!inSingleQuote && !inDoubleQuote) {
      if (ch == '\'') {
        inSingleQuote = true;
      } else if (ch == '"') {
        inDoubleQuote = true;
      } else if (ch == '>') {
        return pos + 1;
      }
    } else if (inSingleQuote && ch == '\'') {
      inSingleQuote = false;
    } else if (inDoubleQuote && ch == '"') {
      inDoubleQuote = false;
    }
  }

  return std::nullopt;
}

std::optional<std::size_t> FindClosingTagStart(std::string_view source, std::size_t nodeEnd) {
  if (nodeEnd == 0 || nodeEnd > source.size()) {
    return std::nullopt;
  }

  const std::size_t closingTagStart = source.rfind("</", nodeEnd - 1);
  if (closingTagStart == std::string_view::npos || closingTagStart >= nodeEnd) {
    return std::nullopt;
  }

  return closingTagStart;
}

std::optional<SourceEditRange> ResolveEditRange(const SourceRange& range, std::string_view source) {
  if (!range.start.offset.has_value() || !range.end.offset.has_value() ||
      *range.end.offset < *range.start.offset || *range.end.offset > source.size()) {
    return std::nullopt;
  }

  return SourceEditRange{
      .start = *range.start.offset,
      .end = *range.end.offset,
  };
}

std::optional<AttributeValueEdit> GetAttributeValueSpan(const XMLNode& node,
                                                        const XMLQualifiedName& name,
                                                        const SourceRange& attributeLocation,
                                                        std::string_view source) {
  std::optional<SourceEditRange> attributeRange = ResolveEditRange(attributeLocation, source);
  if (!attributeRange.has_value() || attributeRange->end <= attributeRange->start) {
    return std::nullopt;
  }

  const std::string_view attributeSource =
      source.substr(attributeRange->start, attributeRange->end - attributeRange->start);
  const std::size_t equalsOffset = attributeSource.find('=');
  if (equalsOffset == std::string_view::npos) {
    return std::nullopt;
  }

  std::size_t quoteOffset = equalsOffset + 1;
  while (quoteOffset < attributeSource.size() && IsXmlWhitespace(attributeSource[quoteOffset])) {
    ++quoteOffset;
  }

  if (quoteOffset >= attributeSource.size()) {
    return std::nullopt;
  }

  const char quote = attributeSource[quoteOffset];
  if ((quote != '"' && quote != '\'') || attributeSource.back() != quote) {
    return std::nullopt;
  }

  return AttributeValueEdit{
      .node = node,
      .name = name,
      .attributeLocation = attributeLocation,
      .valueStart = attributeRange->start + quoteOffset + 1,
      .valueEnd = attributeRange->end - 1,
      .quote = quote,
  };
}

std::optional<XMLNode> FindDeepestNodeAtSourceOffset(const XMLNode& node, std::size_t offset) {
  for (std::optional<XMLNode> child = node.firstChild(); child.has_value();
       child = child->nextSibling()) {
    std::optional<SourceRange> childLocation = child->getNodeLocation();
    if (!childLocation.has_value() || !ContainsOffset(*childLocation, offset)) {
      continue;
    }

    return FindDeepestNodeAtSourceOffset(*child, offset).value_or(*child);
  }

  std::optional<SourceRange> nodeLocation = node.getNodeLocation();
  if (nodeLocation.has_value() && ContainsOffset(*nodeLocation, offset)) {
    return node;
  }

  return std::nullopt;
}

std::optional<AttributeValueEdit> GetAttributeValueEdit(const XMLDocument& document,
                                                        SourceEditRange range) {
  if (document.source().empty() || range.start >= document.source().size()) {
    return std::nullopt;
  }

  const std::size_t lookupOffset = range.start;
  std::optional<XMLAttributeAtSourceOffset> attribute =
      document.attributeAtSourceOffset(lookupOffset);
  if (!attribute.has_value() || !attribute->location.start.offset.has_value() ||
      !attribute->location.end.offset.has_value()) {
    return std::nullopt;
  }

  const std::size_t attributeStart = *attribute->location.start.offset;
  const std::size_t attributeEnd = *attribute->location.end.offset;
  if (attributeEnd <= attributeStart || range.end > attributeEnd) {
    return std::nullopt;
  }

  std::optional<AttributeValueEdit> edit = GetAttributeValueSpan(
      attribute->node, attribute->name, attribute->location, document.source());
  if (!edit.has_value()) {
    return std::nullopt;
  }

  if (range.start < edit->valueStart || range.end > edit->valueEnd) {
    return std::nullopt;
  }

  return edit;
}

std::optional<OpeningTagEdit> GetOpeningTagEdit(const XMLDocument& document,
                                                SourceEditRange range) {
  if (document.source().empty() || range.start >= document.source().size()) {
    return std::nullopt;
  }

  std::optional<XMLNode> node = document.nodeAtSourceOffset(range.start);
  if (!node.has_value() || node->type() != XMLNode::Type::Element) {
    return std::nullopt;
  }

  std::optional<SourceRange> nodeLocation = node->getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value()) {
    return std::nullopt;
  }

  const std::size_t tagStart = *nodeLocation->start.offset;
  std::optional<std::size_t> tagEnd = FindOpeningTagEnd(document.source(), tagStart);
  if (!tagEnd.has_value() || range.start < tagStart || range.end > *tagEnd) {
    return std::nullopt;
  }

  return OpeningTagEdit{
      .node = *node,
      .tagStart = tagStart,
      .tagEnd = *tagEnd,
  };
}

std::optional<TextNodeEdit> GetTextNodeEdit(const XMLDocument& document, SourceEditRange range) {
  if (document.source().empty() || range.start >= document.source().size()) {
    return std::nullopt;
  }

  std::optional<XMLNode> node = document.nodeAtSourceOffset(range.start);
  if (!node.has_value()) {
    return std::nullopt;
  }

  if (node->type() == XMLNode::Type::Data) {
    std::optional<SourceRange> nodeLocation = node->getNodeLocation();
    if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value() ||
        !nodeLocation->end.offset.has_value() || range.start < *nodeLocation->start.offset ||
        range.end > *nodeLocation->end.offset) {
      return std::nullopt;
    }

    return TextNodeEdit{
        .node = *node,
        .elementTextContent = false,
    };
  }

  if (node->type() != XMLNode::Type::Element || !node->value().has_value()) {
    return std::nullopt;
  }

  for (std::optional<XMLNode> child = node->firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Element) {
      return std::nullopt;
    }
  }

  std::optional<SourceRange> nodeLocation = node->getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value() ||
      !nodeLocation->end.offset.has_value()) {
    return std::nullopt;
  }

  std::optional<std::size_t> tagEnd =
      FindOpeningTagEnd(document.source(), *nodeLocation->start.offset);
  std::optional<std::size_t> closingTagStart =
      FindClosingTagStart(document.source(), *nodeLocation->end.offset);
  if (!tagEnd.has_value() || !closingTagStart.has_value() || range.start < *tagEnd ||
      range.end > *closingTagStart) {
    return std::nullopt;
  }

  return TextNodeEdit{
      .node = *node,
      .elementTextContent = true,
  };
}

std::optional<SourceEditRange> GetTextNodeSourceRange(const XMLDocument& document,
                                                      const TextNodeEdit& edit) {
  std::optional<SourceRange> nodeLocation = edit.node.getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value() ||
      !nodeLocation->end.offset.has_value()) {
    return std::nullopt;
  }

  if (!edit.elementTextContent) {
    return SourceEditRange{
        .start = *nodeLocation->start.offset,
        .end = *nodeLocation->end.offset,
    };
  }

  std::optional<std::size_t> tagEnd =
      FindOpeningTagEnd(document.source(), *nodeLocation->start.offset);
  std::optional<std::size_t> closingTagStart =
      FindClosingTagStart(document.source(), *nodeLocation->end.offset);
  if (!tagEnd.has_value() || !closingTagStart.has_value() || *closingTagStart < *tagEnd) {
    return std::nullopt;
  }

  return SourceEditRange{
      .start = *tagEnd,
      .end = *closingTagStart,
  };
}

AttributeMap BuildAttributeMap(const XMLNode& node) {
  AttributeMap result;
  for (const XMLQualifiedNameRef& name : node.attributes()) {
    std::optional<RcString> value = node.getAttribute(name);
    if (value.has_value()) {
      result.emplace(XMLQualifiedName(RcString(name.namespacePrefix), RcString(name.name)), *value);
    }
  }

  return result;
}

ParseDiagnostic MakeEditDiagnostic(RcString reason, SourceRange range) {
  return ParseDiagnostic::Error(std::move(reason), range);
}

SourceRange MakeNodeDiagnosticRange(const XMLNode& node) {
  return node.getNodeLocation().value_or(
      SourceRange{FileOffset::EndOfString(), FileOffset::EndOfString()});
}

XMLQualifiedName MakeOwnedName(const XMLQualifiedNameRef& name) {
  return XMLQualifiedName(RcString(name.namespacePrefix), RcString(name.name));
}

bool IsDocumentNode(const XMLDocument& document, const XMLNode& node) {
  return node.entityHandle().registry() == document.sharedRegistry().get();
}

std::optional<AttributeValueEdit> GetAttributeValueEditForNode(const XMLDocument& document,
                                                               const XMLNode& node,
                                                               const XMLQualifiedName& name) {
  std::optional<SourceRange> attributeLocation = node.getAttributeLocation(document.source(), name);
  if (!attributeLocation.has_value()) {
    return std::nullopt;
  }

  return GetAttributeValueSpan(node, name, *attributeLocation, document.source());
}

std::optional<SourceEditRange> GetAttributeRemovalRange(const XMLDocument& document,
                                                        const XMLNode& node,
                                                        const XMLQualifiedName& name) {
  std::optional<SourceRange> attributeLocation = node.getAttributeLocation(document.source(), name);
  std::optional<SourceEditRange> attributeRange =
      attributeLocation.has_value() ? ResolveEditRange(*attributeLocation, document.source())
                                    : std::nullopt;
  if (!attributeRange.has_value()) {
    return std::nullopt;
  }

  SourceEditRange removalRange = *attributeRange;
  while (removalRange.start > 0 && IsXmlWhitespace(document.source()[removalRange.start - 1])) {
    --removalRange.start;
  }

  return removalRange;
}

ParseResult<XMLDocument> ParseSingleAttributeElement(std::string_view attributeSource) {
  std::string fragment;
  fragment.reserve(attributeSource.size() + 9);
  fragment.append("<node ");
  fragment.append(attributeSource);
  fragment.append("/>");
  return XMLParser::Parse(fragment);
}

ParseResult<XMLDocument> ParseOpeningTagElement(std::string_view openingTagSource) {
  if (openingTagSource.empty() || openingTagSource.back() != '>') {
    return ParseDiagnostic::Error("Opening tag is missing '>'", FileOffset::Offset(0));
  }

  std::string fragment(openingTagSource);
  if (fragment.size() < 2 || fragment[fragment.size() - 2] != '/') {
    fragment.insert(fragment.end() - 1, '/');
  }

  return XMLParser::Parse(fragment);
}

ParseResult<XMLDocument> ParseTextNodeElement(std::string_view textSource) {
  std::string fragment;
  fragment.reserve(textSource.size() + 13);
  fragment.append("<node>");
  fragment.append(textSource);
  fragment.append("</node>");
  return XMLParser::Parse(fragment);
}

void AppendAttributeMutations(XMLNode& node, const AttributeMap& currentAttributes,
                              const AttributeMap& reparsedAttributes,
                              ApplySourceEditResult& result) {
  for (const auto& [name, value] : currentAttributes) {
    if (!reparsedAttributes.contains(name)) {
      node.removeAttribute(name);
      result.mutations.push_back(XMLMutation{
          .kind = XMLMutation::Kind::AttributeRemoved,
          .node = node,
          .attributeName = name,
          .value = std::nullopt,
          .scope = ReparseScope::OpeningTag,
      });
    }
  }

  for (const auto& [name, value] : reparsedAttributes) {
    const auto currentIt = currentAttributes.find(name);
    if (currentIt == currentAttributes.end() || currentIt->second != value) {
      node.setAttribute(name, value);
      result.mutations.push_back(XMLMutation{
          .kind = XMLMutation::Kind::AttributeSet,
          .node = node,
          .attributeName = name,
          .value = value,
          .scope = ReparseScope::OpeningTag,
      });
    }
  }
}

}  // namespace

std::ostream& operator<<(std::ostream& os, ReparseScope scope) {
  switch (scope) {
    case ReparseScope::AttributeValue: return os << "AttributeValue";
    case ReparseScope::OpeningTag: return os << "OpeningTag";
    case ReparseScope::TextNode: return os << "TextNode";
    case ReparseScope::ElementSubtree: return os << "ElementSubtree";
    case ReparseScope::Document: return os << "Document";
  }

  UTILS_UNREACHABLE();
}

XMLDocument::XMLDocument() : registry_(std::make_shared<Registry>()) {
  auto& ctx = registry_->ctx().emplace<XMLDocumentContext>(XMLDocumentContext::InternalCtorTag{});
  ctx.rootEntity = XMLNode::CreateDocumentNode(*this).entityHandle().entity();

  registry_->ctx().emplace<XMLNamespaceContext>(*registry_);
}

XMLDocument::XMLDocument(std::shared_ptr<Registry> registry) : registry_(std::move(registry)) {}

XMLDocument XMLDocument::CreateFromRegistry(std::shared_ptr<Registry> registry) {
  UTILS_RELEASE_ASSERT_MSG(registry != nullptr, "Cannot create XMLDocument from null registry");
  UTILS_RELEASE_ASSERT_MSG(registry->ctx().contains<XMLDocumentContext>(),
                           "Registry does not contain XMLDocumentContext");
  return XMLDocument(std::move(registry));
}

XMLNode XMLDocument::root() const {
  return XMLNode(rootEntityHandle());
}

EntityHandle XMLDocument::rootEntityHandle() const {
  return EntityHandle(*registry_, registry_->ctx().get<XMLDocumentContext>().rootEntity);
}

bool XMLDocument::hasSourceStore() const {
  return registry_->ctx().get<XMLDocumentContext>().sourceStore != nullptr;
}

std::string_view XMLDocument::source() const {
  const XMLSourceStore* store = sourceStore();
  return store != nullptr ? store->source() : std::string_view();
}

std::uint64_t XMLDocument::sourceVersion() const {
  const XMLSourceStore* store = sourceStore();
  return store != nullptr ? store->sourceVersion() : 0;
}

XMLSourceStore* XMLDocument::sourceStore() {
  return registry_->ctx().get<XMLDocumentContext>().sourceStore.get();
}

const XMLSourceStore* XMLDocument::sourceStore() const {
  return registry_->ctx().get<XMLDocumentContext>().sourceStore.get();
}

std::optional<XMLNode> XMLDocument::nodeAtSourceOffset(std::size_t offset) const {
  if (!hasSourceStore() || offset >= source().size()) {
    return std::nullopt;
  }

  return FindDeepestNodeAtSourceOffset(root(), offset);
}

std::optional<XMLAttributeAtSourceOffset> XMLDocument::attributeAtSourceOffset(
    std::size_t offset) const {
  if (!hasSourceStore() || offset >= source().size()) {
    return std::nullopt;
  }

  std::optional<XMLNode> node = nodeAtSourceOffset(offset);
  if (!node.has_value()) {
    return std::nullopt;
  }

  for (const XMLQualifiedNameRef& name : node->attributes()) {
    std::optional<SourceRange> location = node->getAttributeLocation(source(), name);
    if (location.has_value() && ContainsOffset(*location, offset)) {
      return XMLAttributeAtSourceOffset{
          .node = *node,
          .name = XMLQualifiedName(RcString(name.namespacePrefix), RcString(name.name)),
          .location = *location,
      };
    }
  }

  return std::nullopt;
}

ApplySourceEditResult XMLDocument::applySourceEdit(const XMLEditIntent& intent) {
  ApplySourceEditResult result;
  result.scope = ReparseScope::Document;

  XMLSourceStore* store = sourceStore();
  if (store == nullptr) {
    result.diagnostic = MakeEditDiagnostic(
        "Cannot apply source edit to a document without source text", intent.range);
    return result;
  }

  if (intent.sourceVersion != sourceVersion()) {
    result.diagnostic = MakeEditDiagnostic("Source version mismatch", intent.range);
    return result;
  }

  std::optional<SourceEditRange> range = ResolveEditRange(intent.range, source());
  if (!range.has_value()) {
    result.diagnostic = MakeEditDiagnostic("Invalid source edit range", intent.range);
    return result;
  }

  std::optional<AttributeValueEdit> attributeEdit = GetAttributeValueEdit(*this, *range);
  std::optional<OpeningTagEdit> openingTagEdit;
  std::optional<TextNodeEdit> textNodeEdit;
  if (attributeEdit.has_value()) {
    result.scope = ReparseScope::AttributeValue;
  } else if ((openingTagEdit = GetOpeningTagEdit(*this, *range)).has_value()) {
    result.scope = ReparseScope::OpeningTag;
  } else if ((textNodeEdit = GetTextNodeEdit(*this, *range)).has_value()) {
    result.scope = ReparseScope::TextNode;
  } else {
    result.scope = ReparseScope::Document;
  }

  std::optional<XMLSourceDelta> delta =
      store->replace(range->start, range->end - range->start, intent.replacement);
  if (!delta.has_value()) {
    result.diagnostic = MakeEditDiagnostic("Invalid source replacement", intent.range);
    return result;
  }

  result.applied = true;
  result.sourceDeltas.push_back(*delta);

  if (!attributeEdit.has_value()) {
    if (openingTagEdit.has_value()) {
      std::optional<SourceRange> updatedNodeLocation = openingTagEdit->node.getNodeLocation();
      if (!updatedNodeLocation.has_value() || !updatedNodeLocation->start.offset.has_value()) {
        result.diagnostic = MakeEditDiagnostic(
            "Opening tag edit left the node source range unavailable", intent.range);
        return result;
      }

      const std::size_t tagStart = *updatedNodeLocation->start.offset;
      std::optional<std::size_t> tagEnd = FindOpeningTagEnd(source(), tagStart);
      if (!tagEnd.has_value()) {
        result.diagnostic =
            MakeEditDiagnostic("Opening tag edit left the opening tag malformed", intent.range);
        return result;
      }

      ParseResult<XMLDocument> parsedOpeningTag =
          ParseOpeningTagElement(source().substr(tagStart, *tagEnd - tagStart));
      if (parsedOpeningTag.hasError()) {
        result.diagnostic = std::move(parsedOpeningTag).error();
        return result;
      }

      std::optional<XMLNode> parsedNode = parsedOpeningTag.result().root().firstChild();
      if (!parsedNode.has_value()) {
        result.diagnostic =
            MakeEditDiagnostic("Opening tag edit did not produce an element", intent.range);
        return result;
      }

      if (parsedNode->tagName() != openingTagEdit->node.tagName()) {
        result.diagnostic =
            MakeEditDiagnostic("Opening tag element rename is not implemented", intent.range);
        return result;
      }

      const AttributeMap currentAttributes = BuildAttributeMap(openingTagEdit->node);
      const AttributeMap reparsedAttributes = BuildAttributeMap(*parsedNode);
      AppendAttributeMutations(openingTagEdit->node, currentAttributes, reparsedAttributes, result);
      return result;
    }

    if (textNodeEdit.has_value()) {
      std::optional<SourceEditRange> updatedTextRange =
          GetTextNodeSourceRange(*this, *textNodeEdit);
      if (!updatedTextRange.has_value()) {
        result.diagnostic = MakeEditDiagnostic(
            "Text node edit left the node source range unavailable", intent.range);
        return result;
      }

      const std::size_t textStart = updatedTextRange->start;
      const std::size_t textEnd = updatedTextRange->end;
      ParseResult<XMLDocument> parsedText =
          ParseTextNodeElement(source().substr(textStart, textEnd - textStart));
      if (parsedText.hasError()) {
        result.diagnostic = std::move(parsedText).error();
        return result;
      }

      std::optional<XMLNode> parsedElement = parsedText.result().root().firstChild();
      if (!parsedElement.has_value()) {
        result.diagnostic =
            MakeEditDiagnostic("Text node edit did not produce a wrapper element", intent.range);
        return result;
      }

      std::optional<XMLNode> parsedTextNode = parsedElement->firstChild();
      if (!parsedTextNode.has_value() || parsedTextNode->type() != XMLNode::Type::Data ||
          parsedTextNode->nextSibling().has_value()) {
        result.diagnostic =
            MakeEditDiagnostic("Text node edit changed the local XML structure", intent.range);
        return result;
      }

      const RcString parsedValue = parsedTextNode->value().value_or(RcString(""));
      textNodeEdit->node.setValue(parsedValue);
      if (!textNodeEdit->elementTextContent) {
        if (std::optional<XMLNode> parent = textNodeEdit->node.parentElement()) {
          parent->setValue(parsedValue);
        }
      }
      result.mutations.push_back(XMLMutation{
          .kind = XMLMutation::Kind::NodeValueChanged,
          .node = textNodeEdit->node,
          .attributeName = XMLQualifiedName(""),
          .value = parsedValue,
          .scope = ReparseScope::TextNode,
      });
      return result;
    }

    result.diagnostic = MakeEditDiagnostic(
        "Only attribute-value, opening-tag, and text-node source edits are implemented",
        intent.range);
    return result;
  }

  std::optional<SourceRange> updatedAttributeLocation =
      attributeEdit->node.getAttributeLocation(source(), attributeEdit->name);
  if (!updatedAttributeLocation.has_value() ||
      !updatedAttributeLocation->start.offset.has_value() ||
      !updatedAttributeLocation->end.offset.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Attribute value edit left the opening tag malformed", intent.range);
    return result;
  }

  const std::size_t attributeStart = *updatedAttributeLocation->start.offset;
  const std::size_t attributeEnd = *updatedAttributeLocation->end.offset;
  ParseResult<XMLDocument> parsedAttribute =
      ParseSingleAttributeElement(source().substr(attributeStart, attributeEnd - attributeStart));
  if (parsedAttribute.hasError()) {
    result.diagnostic = std::move(parsedAttribute).error();
    return result;
  }

  std::optional<XMLNode> parsedNode = parsedAttribute.result().root().firstChild();
  if (!parsedNode.has_value()) {
    result.diagnostic = MakeEditDiagnostic("Attribute value edit did not produce an element",
                                           *updatedAttributeLocation);
    return result;
  }

  std::optional<RcString> parsedValue = parsedNode->getAttribute(attributeEdit->name);
  if (!parsedValue.has_value()) {
    result.diagnostic = MakeEditDiagnostic("Attribute value edit removed the target attribute",
                                           *updatedAttributeLocation);
    return result;
  }

  attributeEdit->node.setAttribute(attributeEdit->name, *parsedValue);
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::AttributeSet,
      .node = attributeEdit->node,
      .attributeName = attributeEdit->name,
      .value = *parsedValue,
      .scope = ReparseScope::AttributeValue,
  });
  return result;
}

ApplySourceEditResult XMLDocument::setAttribute(XMLNode node, const XMLQualifiedNameRef& name,
                                                std::string_view value) {
  ApplySourceEditResult result;
  result.scope = ReparseScope::AttributeValue;

  const XMLQualifiedName ownedName = MakeOwnedName(name);
  const SourceRange diagnosticRange = MakeNodeDiagnosticRange(node);
  if (!IsDocumentNode(*this, node)) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot set attribute on a node from another document", diagnosticRange);
    return result;
  }

  XMLSourceStore* store = sourceStore();
  if (store == nullptr) {
    result.diagnostic = MakeEditDiagnostic("Cannot set source-backed attribute without source text",
                                           diagnosticRange);
    return result;
  }

  if (node.type() != XMLNode::Type::Element && node.type() != XMLNode::Type::XMLDeclaration) {
    result.diagnostic = MakeEditDiagnostic(
        "Cannot set attribute on a node that does not support attributes", diagnosticRange);
    return result;
  }

  std::optional<AttributeValueEdit> edit = GetAttributeValueEditForNode(*this, node, ownedName);
  if (!edit.has_value()) {
    result.scope = ReparseScope::OpeningTag;
    result.diagnostic = MakeEditDiagnostic(
        "Only existing source-backed attribute writes are implemented", diagnosticRange);
    return result;
  }

  std::optional<RcString> escapedValue = EscapeAttributeValue(value, edit->quote);
  if (!escapedValue.has_value()) {
    result.diagnostic = MakeEditDiagnostic("Attribute value cannot be represented in XML source",
                                           edit->attributeLocation);
    return result;
  }

  std::optional<XMLSourceDelta> delta =
      store->replace(edit->valueStart, edit->valueEnd - edit->valueStart, *escapedValue);
  if (!delta.has_value()) {
    result.diagnostic = MakeEditDiagnostic("Invalid source replacement for attribute value",
                                           edit->attributeLocation);
    return result;
  }

  result.applied = true;
  result.sourceDeltas.push_back(*delta);

  const RcString ownedValue(value);
  node.setAttribute(ownedName, ownedValue);
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::AttributeSet,
      .node = node,
      .attributeName = ownedName,
      .value = ownedValue,
      .scope = ReparseScope::AttributeValue,
  });
  return result;
}

ApplySourceEditResult XMLDocument::removeAttribute(XMLNode node, const XMLQualifiedNameRef& name) {
  ApplySourceEditResult result;
  result.scope = ReparseScope::OpeningTag;

  const XMLQualifiedName ownedName = MakeOwnedName(name);
  const SourceRange diagnosticRange = MakeNodeDiagnosticRange(node);
  if (!IsDocumentNode(*this, node)) {
    result.diagnostic = MakeEditDiagnostic(
        "Cannot remove attribute from a node in another document", diagnosticRange);
    return result;
  }

  XMLSourceStore* store = sourceStore();
  if (store == nullptr) {
    result.diagnostic = MakeEditDiagnostic(
        "Cannot remove source-backed attribute without source text", diagnosticRange);
    return result;
  }

  if (!node.hasAttribute(ownedName)) {
    return result;
  }

  std::optional<SourceEditRange> removalRange = GetAttributeRemovalRange(*this, node, ownedName);
  if (!removalRange.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot remove attribute without a source range", diagnosticRange);
    return result;
  }

  std::optional<XMLSourceDelta> delta = store->replace(
      removalRange->start, removalRange->end - removalRange->start, std::string_view());
  if (!delta.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Invalid source replacement for attribute removal", diagnosticRange);
    return result;
  }

  result.applied = true;
  result.sourceDeltas.push_back(*delta);

  node.removeAttribute(ownedName);
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::AttributeRemoved,
      .node = node,
      .attributeName = ownedName,
      .value = std::nullopt,
      .scope = ReparseScope::OpeningTag,
  });
  return result;
}

void XMLDocument::setSource(std::string source) {
  registry_->ctx().get<XMLDocumentContext>().sourceStore =
      std::make_shared<XMLSourceStore>(std::move(source));
}

}  // namespace donner::xml
