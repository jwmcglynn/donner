#include "donner/base/xml/XMLDocument.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/xml/XMLEscape.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/components/XMLDocumentContext.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"
#include "donner/base/xml/components/XMLValueComponent.h"

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

struct ElementSubtreeEdit {
  XMLNode node;
};

using AttributeMap = std::map<XMLQualifiedName, RcString>;

bool IsXmlWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

void AppendQualifiedName(std::string& out, const XMLQualifiedNameRef& name) {
  if (!name.namespacePrefix.empty()) {
    out.append(name.namespacePrefix);
    out.push_back(':');
  }
  out.append(name.name);
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

std::optional<std::size_t> FindDirtyOpeningTagEnd(std::string_view source, std::size_t tagStart,
                                                  std::size_t nodeEnd) {
  if (tagStart >= source.size() || source[tagStart] != '<' || nodeEnd <= tagStart) {
    return std::nullopt;
  }

  const std::size_t scanEnd = nodeEnd < source.size() ? nodeEnd : source.size();
  for (std::size_t pos = tagStart + 1; pos < scanEnd; ++pos) {
    if (source[pos] == '>') {
      return pos + 1;
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
  if (!tagEnd.has_value() && nodeLocation->end.offset.has_value()) {
    tagEnd = FindDirtyOpeningTagEnd(document.source(), tagStart, *nodeLocation->end.offset);
  }
  if (range.start == tagStart && range.end == tagStart) {
    return std::nullopt;
  }
  if (tagEnd.has_value() && nodeLocation->end.offset.has_value() && range.start == tagStart &&
      range.end == *nodeLocation->end.offset && *tagEnd == *nodeLocation->end.offset) {
    return std::nullopt;
  }
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

std::optional<ElementSubtreeEdit> GetElementSubtreeEdit(const XMLDocument& document,
                                                        SourceEditRange range) {
  if (document.source().empty() || range.start > document.source().size()) {
    return std::nullopt;
  }

  std::size_t lookupOffset = range.start;
  if (lookupOffset == document.source().size()) {
    if (lookupOffset == 0) {
      return std::nullopt;
    }
    --lookupOffset;
  }

  std::optional<XMLNode> node = document.nodeAtSourceOffset(lookupOffset);
  while (node.has_value()) {
    if (node->type() == XMLNode::Type::Element) {
      std::optional<SourceRange> nodeLocation = node->getNodeLocation();
      if (nodeLocation.has_value() && nodeLocation->start.offset.has_value() &&
          nodeLocation->end.offset.has_value()) {
        std::optional<std::size_t> tagEnd =
            FindOpeningTagEnd(document.source(), *nodeLocation->start.offset);
        std::optional<std::size_t> closingTagStart =
            FindClosingTagStart(document.source(), *nodeLocation->end.offset);
        if (tagEnd.has_value() && closingTagStart.has_value() && range.start >= *tagEnd &&
            range.end <= *closingTagStart) {
          return ElementSubtreeEdit{
              .node = *node,
          };
        }
      }
    }

    node = node->parentElement();
  }

  return std::nullopt;
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

void ClearSourceLocationsRecursive(XMLNode node) {
  for (std::optional<XMLNode> child = node.firstChild(); child.has_value();) {
    XMLNode currentChild = *child;
    child = currentChild.nextSibling();
    ClearSourceLocationsRecursive(currentChild);
  }

  node.clearSourceLocation();
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

std::optional<std::size_t> GetAttributeInsertionOffset(const XMLDocument& document,
                                                       const XMLNode& node) {
  std::optional<SourceRange> nodeLocation = node.getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value()) {
    return std::nullopt;
  }

  std::optional<std::size_t> tagEnd =
      FindOpeningTagEnd(document.source(), *nodeLocation->start.offset);
  if (!tagEnd.has_value() || *tagEnd == 0) {
    return std::nullopt;
  }

  if (*tagEnd >= 2 && document.source()[*tagEnd - 2] == '/') {
    return *tagEnd - 2;
  }

  return *tagEnd - 1;
}

std::string SerializeAttributeInsertion(std::string_view source, std::size_t insertionOffset,
                                        const XMLQualifiedName& name,
                                        std::string_view escapedValue) {
  std::string result;
  if (insertionOffset == 0 || !IsXmlWhitespace(source[insertionOffset - 1])) {
    result.push_back(' ');
  }
  AppendQualifiedName(result, name);
  result.append("=\"");
  result.append(escapedValue);
  result.push_back('"');
  return result;
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

void SetSourceOffsetsFromParsed(XMLNode& node, const XMLNode& parsedNode,
                                std::size_t sourceOffsetBase) {
  std::optional<SourceRange> location = parsedNode.getNodeLocation();
  if (!location.has_value() || !location->start.offset.has_value() ||
      !location->end.offset.has_value()) {
    return;
  }

  node.setSourceStartOffset(FileOffset::Offset(sourceOffsetBase + *location->start.offset));
  node.setSourceEndOffset(FileOffset::Offset(sourceOffsetBase + *location->end.offset));
}

void SyncAttributesFromParsed(XMLNode& target, const XMLNode& parsedNode) {
  const AttributeMap currentAttributes = BuildAttributeMap(target);
  const AttributeMap parsedAttributes = BuildAttributeMap(parsedNode);

  for (const auto& [name, value] : currentAttributes) {
    if (!parsedAttributes.contains(name)) {
      target.removeAttribute(name);
    }
  }

  for (const auto& [name, value] : parsedAttributes) {
    const auto currentIt = currentAttributes.find(name);
    if (currentIt == currentAttributes.end() || currentIt->second != value) {
      target.setAttribute(name, value);
    }
  }
}

void SyncValueFromParsed(XMLNode& target, const XMLNode& parsedNode) {
  std::optional<RcString> parsedValue = parsedNode.value();
  if (parsedValue.has_value()) {
    target.setValue(*parsedValue);
  } else if (target.entityHandle().all_of<components::XMLValueComponent>()) {
    target.entityHandle().remove<components::XMLValueComponent>();
  }
}

std::optional<RcString> ElementId(const XMLNode& node) {
  if (node.type() != XMLNode::Type::Element) {
    return std::nullopt;
  }

  return node.getAttribute("id");
}

std::optional<std::size_t> FindReusableChild(const XMLNode& parsedChild,
                                             const std::vector<XMLNode>& oldChildren,
                                             const std::vector<bool>& usedChildren) {
  std::optional<RcString> parsedId = ElementId(parsedChild);
  if (!parsedId.has_value() || parsedId->empty()) {
    return std::nullopt;
  }

  for (std::size_t index = 0; index < oldChildren.size(); ++index) {
    if (usedChildren[index]) {
      continue;
    }

    const XMLNode& oldChild = oldChildren[index];
    std::optional<RcString> oldId = ElementId(oldChild);
    if (oldChild.type() == parsedChild.type() && oldChild.tagName() == parsedChild.tagName() &&
        oldId.has_value() && *oldId == *parsedId) {
      return index;
    }
  }

  return std::nullopt;
}

XMLNode CloneParsedNodeInto(XMLDocument& document, const XMLNode& parsedNode,
                            std::size_t sourceOffsetBase);

void ReplaceChildrenFromParsedNode(XMLDocument& document, XMLNode& target,
                                   const XMLNode& parsedTarget, std::size_t sourceOffsetBase);

void SyncNodeFromParsed(XMLDocument& document, XMLNode& target, const XMLNode& parsedNode,
                        std::size_t sourceOffsetBase) {
  if (target.type() == XMLNode::Type::Element || target.type() == XMLNode::Type::XMLDeclaration) {
    SyncAttributesFromParsed(target, parsedNode);
    ReplaceChildrenFromParsedNode(document, target, parsedNode, sourceOffsetBase);
  }

  SyncValueFromParsed(target, parsedNode);
  SetSourceOffsetsFromParsed(target, parsedNode, sourceOffsetBase);
}

XMLNode CloneParsedNodeInto(XMLDocument& document, const XMLNode& parsedNode,
                            std::size_t sourceOffsetBase) {
  XMLNode clone = [&]() {
    switch (parsedNode.type()) {
      case XMLNode::Type::Element:
        return XMLNode::CreateElementNode(document, parsedNode.tagName());
      case XMLNode::Type::Data:
        return XMLNode::CreateDataNode(document, parsedNode.value().value_or(RcString("")));
      case XMLNode::Type::CData:
        return XMLNode::CreateCDataNode(document, parsedNode.value().value_or(RcString("")));
      case XMLNode::Type::Comment:
        return XMLNode::CreateCommentNode(document, parsedNode.value().value_or(RcString("")));
      case XMLNode::Type::DocType:
        return XMLNode::CreateDocTypeNode(document, parsedNode.value().value_or(RcString("")));
      case XMLNode::Type::ProcessingInstruction:
        return XMLNode::CreateProcessingInstructionNode(document, parsedNode.tagName().name,
                                                        parsedNode.value().value_or(RcString("")));
      case XMLNode::Type::XMLDeclaration: return XMLNode::CreateXMLDeclarationNode(document);
      case XMLNode::Type::Document: break;
    }

    UTILS_UNREACHABLE();
  }();

  SyncNodeFromParsed(document, clone, parsedNode, sourceOffsetBase);
  return clone;
}

void ReplaceChildrenFromParsedNode(XMLDocument& document, XMLNode& target,
                                   const XMLNode& parsedTarget, std::size_t sourceOffsetBase) {
  std::vector<XMLNode> oldChildren;
  for (std::optional<XMLNode> child = target.firstChild(); child.has_value();
       child = child->nextSibling()) {
    oldChildren.push_back(*child);
  }

  for (const XMLNode& child : oldChildren) {
    target.removeChild(child);
  }

  std::vector<bool> usedChildren(oldChildren.size(), false);
  for (std::optional<XMLNode> child = parsedTarget.firstChild(); child.has_value();
       child = child->nextSibling()) {
    std::optional<XMLNode> nodeToAppend;
    if (std::optional<std::size_t> oldIndex =
            FindReusableChild(*child, oldChildren, usedChildren)) {
      usedChildren[*oldIndex] = true;
      nodeToAppend = oldChildren[*oldIndex];
      SyncNodeFromParsed(document, *nodeToAppend, *child, sourceOffsetBase);
    } else {
      nodeToAppend = CloneParsedNodeInto(document, *child, sourceOffsetBase);
    }

    target.appendChild(*nodeToAppend);
  }

  for (std::size_t index = 0; index < oldChildren.size(); ++index) {
    if (!usedChildren[index]) {
      ClearSourceLocationsRecursive(oldChildren[index]);
    }
  }

  SyncValueFromParsed(target, parsedTarget);
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
  std::optional<ElementSubtreeEdit> elementSubtreeEdit;
  if (attributeEdit.has_value()) {
    result.scope = ReparseScope::AttributeValue;
  } else if ((openingTagEdit = GetOpeningTagEdit(*this, *range)).has_value()) {
    result.scope = ReparseScope::OpeningTag;
  } else if ((textNodeEdit = GetTextNodeEdit(*this, *range)).has_value()) {
    result.scope = ReparseScope::TextNode;
  } else if ((elementSubtreeEdit = GetElementSubtreeEdit(*this, *range)).has_value()) {
    result.scope = ReparseScope::ElementSubtree;
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

    if (elementSubtreeEdit.has_value()) {
      std::optional<SourceRange> updatedNodeLocation = elementSubtreeEdit->node.getNodeLocation();
      if (!updatedNodeLocation.has_value() || !updatedNodeLocation->start.offset.has_value() ||
          !updatedNodeLocation->end.offset.has_value()) {
        result.diagnostic = MakeEditDiagnostic(
            "Element subtree edit left the node source range unavailable", intent.range);
        return result;
      }

      const std::size_t nodeStart = *updatedNodeLocation->start.offset;
      const std::size_t nodeEnd = *updatedNodeLocation->end.offset;
      ParseResult<XMLDocument> parsedSubtree =
          XMLParser::Parse(source().substr(nodeStart, nodeEnd - nodeStart));
      if (parsedSubtree.hasError()) {
        result.diagnostic = std::move(parsedSubtree).error();
        return result;
      }

      std::optional<XMLNode> parsedNode = parsedSubtree.result().root().firstChild();
      if (!parsedNode.has_value() || parsedNode->type() != XMLNode::Type::Element ||
          parsedNode->nextSibling().has_value()) {
        result.diagnostic =
            MakeEditDiagnostic("Element subtree edit did not produce one element", intent.range);
        return result;
      }

      if (parsedNode->tagName() != elementSubtreeEdit->node.tagName()) {
        result.diagnostic =
            MakeEditDiagnostic("Element subtree edit renamed the target element", intent.range);
        return result;
      }

      ReplaceChildrenFromParsedNode(*this, elementSubtreeEdit->node, *parsedNode, nodeStart);
      result.mutations.push_back(XMLMutation{
          .kind = XMLMutation::Kind::SubtreeReplaced,
          .node = elementSubtreeEdit->node,
          .attributeName = XMLQualifiedName(""),
          .value = std::nullopt,
          .scope = ReparseScope::ElementSubtree,
      });
      return result;
    }

    result.diagnostic = MakeEditDiagnostic(
        "Only attribute-value, opening-tag, text-node, and element-subtree source edits are "
        "implemented",
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

    if (node.hasAttribute(ownedName)) {
      result.diagnostic =
          MakeEditDiagnostic("Cannot update attribute without a source range", diagnosticRange);
      return result;
    }

    std::optional<std::size_t> insertionOffset = GetAttributeInsertionOffset(*this, node);
    if (!insertionOffset.has_value()) {
      result.diagnostic = MakeEditDiagnostic(
          "Cannot insert attribute without an opening tag source range", diagnosticRange);
      return result;
    }

    std::optional<RcString> escapedValue = EscapeAttributeValue(value, '"');
    if (!escapedValue.has_value()) {
      result.diagnostic = MakeEditDiagnostic("Attribute value cannot be represented in XML source",
                                             diagnosticRange);
      return result;
    }

    const std::string insertion =
        SerializeAttributeInsertion(source(), *insertionOffset, ownedName, *escapedValue);
    std::optional<XMLSourceDelta> delta = store->replace(*insertionOffset, 0, insertion);
    if (!delta.has_value()) {
      result.diagnostic =
          MakeEditDiagnostic("Invalid source replacement for attribute insertion", diagnosticRange);
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
        .scope = ReparseScope::OpeningTag,
    });
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

ApplySourceEditResult XMLDocument::removeNode(XMLNode node) {
  ApplySourceEditResult result;
  result.scope = ReparseScope::ElementSubtree;

  const SourceRange diagnosticRange = MakeNodeDiagnosticRange(node);
  if (!IsDocumentNode(*this, node)) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot remove a node from another document", diagnosticRange);
    return result;
  }

  XMLSourceStore* store = sourceStore();
  if (store == nullptr) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot remove source-backed node without source text", diagnosticRange);
    return result;
  }

  if (node.type() == XMLNode::Type::Document) {
    result.diagnostic = MakeEditDiagnostic("Cannot remove XML document root", diagnosticRange);
    return result;
  }

  std::optional<SourceRange> nodeLocation = node.getNodeLocation();
  std::optional<SourceEditRange> removalRange =
      nodeLocation.has_value() ? ResolveEditRange(*nodeLocation, source()) : std::nullopt;
  if (!removalRange.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot remove node without a source range", diagnosticRange);
    return result;
  }

  std::optional<XMLSourceDelta> delta = store->replace(
      removalRange->start, removalRange->end - removalRange->start, std::string_view());
  if (!delta.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Invalid source replacement for node removal", diagnosticRange);
    return result;
  }

  result.applied = true;
  result.sourceDeltas.push_back(*delta);

  ClearSourceLocationsRecursive(node);
  node.remove();
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::NodeRemoved,
      .node = node,
      .attributeName = XMLQualifiedName(""),
      .value = std::nullopt,
      .scope = ReparseScope::ElementSubtree,
  });
  return result;
}

void XMLDocument::setSource(std::string source) {
  registry_->ctx().get<XMLDocumentContext>().sourceStore =
      std::make_shared<XMLSourceStore>(std::move(source));
}

}  // namespace donner::xml
