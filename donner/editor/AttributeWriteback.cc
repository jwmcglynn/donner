#include "donner/editor/AttributeWriteback.h"

#include <algorithm>
#include <string>
#include <vector>

#include "donner/base/xml/XMLEscape.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/SVGDocument.h"

namespace donner::editor {

namespace {

std::optional<TextPatch> buildAttributeWritebackForNode(std::string_view source,
                                                        const xml::XMLNode& xmlNode,
                                                        std::string_view attrName,
                                                        std::string_view newValue);
std::optional<TextPatch> buildAttributeRemoveWritebackForNode(std::string_view source,
                                                              const xml::XMLNode& xmlNode,
                                                              std::string_view attrName);
std::optional<TextPatch> buildElementRemoveWritebackForNode(std::string_view source,
                                                            const xml::XMLNode& xmlNode);

std::string QualifiedNameToString(const xml::XMLQualifiedNameRef& name) {
  std::string result;
  result.reserve(name.name.size() + name.namespacePrefix.size() + 1);
  if (!name.namespacePrefix.empty()) {
    result.append(name.namespacePrefix);
    result.push_back(':');
  }
  result.append(name.name);
  return result;
}

bool HasExpectedOpeningTagAt(std::string_view source, std::size_t offset,
                             const xml::XMLQualifiedNameRef& tagName) {
  if (offset >= source.size() || source[offset] != '<') {
    return false;
  }

  const std::string qualifiedName = QualifiedNameToString(tagName);
  if (offset + 1 + qualifiedName.size() > source.size()) {
    return false;
  }

  const std::string_view candidate = source.substr(offset + 1, qualifiedName.size());
  if (candidate != qualifiedName) {
    return false;
  }

  const std::size_t afterName = offset + 1 + qualifiedName.size();
  if (afterName >= source.size()) {
    return false;
  }

  const char terminator = source[afterName];
  return terminator == '>' || terminator == '/' || terminator == ' ' || terminator == '\t' ||
         terminator == '\n' || terminator == '\r';
}

bool IsXmlWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::size_t FindStartOfLine(std::string_view source, std::size_t offset) {
  std::size_t lineStart = offset;
  while (lineStart > 0 && source[lineStart - 1] != '\n' && source[lineStart - 1] != '\r') {
    --lineStart;
  }

  return lineStart;
}

std::optional<std::vector<AttributeWritebackPathSegment>> BuildElementPath(
    const xml::XMLNode& node) {
  std::vector<AttributeWritebackPathSegment> reversedPath;

  for (std::optional<xml::XMLNode> current = node; current.has_value();) {
    const std::optional<xml::XMLNode> parent = current->parentElement();
    if (!parent.has_value()) {
      return std::nullopt;
    }

    std::size_t childIndex = 0;
    bool foundCurrent = false;
    for (auto child = parent->firstChild(); child.has_value(); child = child->nextSibling()) {
      if (child->type() != xml::XMLNode::Type::Element) {
        continue;
      }

      if (*child == *current) {
        foundCurrent = true;
        break;
      }

      ++childIndex;
    }

    if (!foundCurrent) {
      return std::nullopt;
    }

    reversedPath.push_back(AttributeWritebackPathSegment{
        childIndex,
        xml::XMLQualifiedName(RcString(current->tagName().namespacePrefix),
                              RcString(current->tagName().name)),
    });

    if (parent->type() == xml::XMLNode::Type::Document) {
      break;
    }

    current = parent;
  }

  std::reverse(reversedPath.begin(), reversedPath.end());
  return reversedPath;
}

std::optional<xml::XMLNode> ResolveNodeInParsedDocument(
    xml::XMLDocument& document, const std::vector<AttributeWritebackPathSegment>& path) {
  xml::XMLNode current = document.root();
  for (const AttributeWritebackPathSegment& segment : path) {
    std::size_t childIndex = 0;
    std::optional<xml::XMLNode> matchedChild;

    for (auto child = current.firstChild(); child.has_value(); child = child->nextSibling()) {
      if (child->type() != xml::XMLNode::Type::Element) {
        continue;
      }

      if (childIndex == segment.elementChildIndex) {
        matchedChild = child;
        break;
      }

      ++childIndex;
    }

    if (!matchedChild.has_value() || matchedChild->tagName() != segment.qualifiedName) {
      return std::nullopt;
    }

    current = *matchedChild;
  }

  return current;
}

std::optional<xml::XMLNode> FindNodeByIdInParsedTree(const xml::XMLNode& node,
                                                     const AttributeWritebackTarget& target) {
  if (target.elementPath.empty() || !target.elementId.has_value()) {
    return std::nullopt;
  }

  const auto targetTagName = target.elementPath.back().qualifiedName;
  if (node.type() == xml::XMLNode::Type::Element && node.tagName() == targetTagName) {
    if (const auto id = node.getAttribute("id"); id.has_value() && *id == *target.elementId) {
      return node;
    }
  }

  for (auto child = node.firstChild(); child.has_value(); child = child->nextSibling()) {
    if (auto match = FindNodeByIdInParsedTree(*child, target); match.has_value()) {
      return match;
    }
  }

  return std::nullopt;
}

std::optional<xml::XMLNode> ResolveNodeByTarget(xml::XMLDocument& document,
                                                const AttributeWritebackTarget& target) {
  if (auto byId = FindNodeByIdInParsedTree(document.root(), target); byId.has_value()) {
    return byId;
  }

  return ResolveNodeInParsedDocument(document, target.elementPath);
}

std::optional<TextPatch> BuildAttributeWritebackFromCurrentSource(
    std::string_view source, const AttributeWritebackTarget& target, std::string_view attrName,
    std::string_view newValue) {
  auto parsed = xml::XMLParser::Parse(source);
  if (parsed.hasError()) {
    return std::nullopt;
  }

  auto currentNode = ResolveNodeByTarget(parsed.result(), target);
  if (!currentNode.has_value()) {
    return std::nullopt;
  }

  return buildAttributeWritebackForNode(source, *currentNode, attrName, newValue);
}

std::optional<TextPatch> BuildAttributeRemoveWritebackFromCurrentSource(
    std::string_view source, const AttributeWritebackTarget& target, std::string_view attrName) {
  auto parsed = xml::XMLParser::Parse(source);
  if (parsed.hasError()) {
    return std::nullopt;
  }

  auto currentNode = ResolveNodeByTarget(parsed.result(), target);
  if (!currentNode.has_value()) {
    return std::nullopt;
  }

  return buildAttributeRemoveWritebackForNode(source, *currentNode, attrName);
}

std::optional<TextPatch> BuildElementRemoveWritebackFromCurrentSource(
    std::string_view source, const AttributeWritebackTarget& target) {
  auto parsed = xml::XMLParser::Parse(source);
  if (parsed.hasError()) {
    return std::nullopt;
  }

  auto currentNode = ResolveNodeByTarget(parsed.result(), target);
  if (!currentNode.has_value()) {
    return std::nullopt;
  }

  return buildElementRemoveWritebackForNode(source, *currentNode);
}

std::optional<TextPatch> buildAttributeWritebackForNode(std::string_view source,
                                                        const xml::XMLNode& xmlNode,
                                                        std::string_view attrName,
                                                        std::string_view newValue) {
  const auto nodeLocation = xmlNode.getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value()) {
    return std::nullopt;
  }

  // Escape the value for XML attribute context (double-quoted).
  auto escaped = xml::EscapeAttributeValue(newValue, '"');
  if (!escaped.has_value()) {
    return std::nullopt;
  }

  const xml::XMLQualifiedNameRef qualifiedName(attrName);

  // Try to find the existing attribute in the source text.
  const auto attrRange =
      xml::XMLParser::GetAttributeLocation(source, nodeLocation->start, qualifiedName);

  if (attrRange.has_value()) {
    // Attribute exists — replace its full span (name="value") with the
    // new name="escaped_value".
    if (!attrRange->start.offset.has_value() || !attrRange->end.offset.has_value()) {
      return std::nullopt;
    }

    const std::size_t start = attrRange->start.offset.value();
    const std::size_t end = attrRange->end.offset.value();
    if (start > source.size() || end > source.size() || end < start) {
      return std::nullopt;
    }

    // Build replacement: name="escaped_value"
    std::string replacement;
    replacement.reserve(attrName.size() + 3 + escaped->size());
    replacement.append(attrName);
    replacement.append("=\"");
    replacement.append(std::string_view(*escaped));
    replacement.push_back('"');

    return TextPatch{start, end - start, std::move(replacement)};
  }

  // Attribute doesn't exist — insert ` name="value"` before the element's
  // closing `>` or `/>`. Find the `>` by scanning backward from the
  // element's end offset (or forward from start if end isn't useful).
  //
  // Strategy: scan the source from the element's start offset looking for
  // the first `>` that isn't inside a quoted attribute value. This is the
  // opening tag's close, which is where we insert.
  const std::size_t elementStart = nodeLocation->start.offset.value();

  // Walk from element start looking for the closing `>` of the opening tag.
  // We need to skip over quoted regions.
  std::size_t pos = elementStart;
  if (pos < source.size() && source[pos] == '<') {
    ++pos;  // skip '<'
  }

  // Skip element name.
  while (pos < source.size() && source[pos] != '>' && source[pos] != '/' && source[pos] != ' ' &&
         source[pos] != '\t' && source[pos] != '\n' && source[pos] != '\r') {
    ++pos;
  }

  // Walk through attributes, skipping quoted values.
  while (pos < source.size()) {
    const char c = source[pos];
    if (c == '"' || c == '\'') {
      ++pos;
      while (pos < source.size() && source[pos] != c) {
        ++pos;
      }
      if (pos < source.size()) ++pos;
    } else if (c == '>' || (c == '/' && pos + 1 < source.size() && source[pos + 1] == '>')) {
      // Found the tag close. Insert ` name="value"` right before it.
      std::string insertion;
      insertion.push_back(' ');
      insertion.append(attrName);
      insertion.append("=\"");
      insertion.append(std::string_view(*escaped));
      insertion.push_back('"');

      return TextPatch{pos, 0, std::move(insertion)};
    } else {
      ++pos;
    }
  }

  // Couldn't find the tag close — source is malformed or stale.
  return std::nullopt;
}

std::optional<TextPatch> buildAttributeRemoveWritebackForNode(std::string_view source,
                                                              const xml::XMLNode& xmlNode,
                                                              std::string_view attrName) {
  const auto nodeLocation = xmlNode.getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value()) {
    return std::nullopt;
  }

  const auto attrRange =
      xml::XMLParser::GetAttributeLocation(source, nodeLocation->start, attrName);
  if (!attrRange.has_value()) {
    return std::nullopt;
  }

  if (!attrRange->start.offset.has_value() || !attrRange->end.offset.has_value()) {
    return std::nullopt;
  }

  std::size_t start = attrRange->start.offset.value();
  std::size_t end = attrRange->end.offset.value();
  if (start > source.size() || end > source.size() || end < start) {
    return std::nullopt;
  }

  if (start > 0 && IsXmlWhitespace(source[start - 1])) {
    --start;
  } else if (end < source.size() && IsXmlWhitespace(source[end])) {
    ++end;
  }

  return TextPatch{start, end - start, ""};
}

std::optional<TextPatch> buildElementRemoveWritebackForNode(std::string_view source,
                                                            const xml::XMLNode& xmlNode) {
  const auto nodeLocation = xmlNode.getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value() ||
      !nodeLocation->end.offset.has_value()) {
    return std::nullopt;
  }

  std::size_t start = nodeLocation->start.offset.value();
  std::size_t end = nodeLocation->end.offset.value();
  if (start > source.size() || end > source.size() || end < start) {
    return std::nullopt;
  }

  const std::size_t lineStart = FindStartOfLine(source, start);
  bool onlyIndentBeforeNode = true;
  for (std::size_t i = lineStart; i < start; ++i) {
    if (source[i] != ' ' && source[i] != '\t') {
      onlyIndentBeforeNode = false;
      break;
    }
  }

  if (onlyIndentBeforeNode) {
    start = lineStart;
  }

  if (end < source.size()) {
    if (source[end] == '\r') {
      ++end;
      if (end < source.size() && source[end] == '\n') {
        ++end;
      }
    } else if (source[end] == '\n') {
      ++end;
    } else if (IsXmlWhitespace(source[end])) {
      ++end;
    }
  }

  return TextPatch{start, end - start, ""};
}

}  // namespace

std::optional<AttributeWritebackTarget> captureAttributeWritebackTarget(
    const svg::SVGElement& element) {
  const auto xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return std::nullopt;
  }

  auto path = BuildElementPath(*xmlNode);
  if (!path.has_value()) {
    return std::nullopt;
  }

  AttributeWritebackTarget target{.elementPath = std::move(*path)};
  const RcString elementId = element.id();
  if (!elementId.empty()) {
    target.elementId = elementId;
  }
  return target;
}

std::optional<svg::SVGElement> resolveAttributeWritebackTarget(
    svg::SVGDocument& document, const AttributeWritebackTarget& target) {
  if (target.elementPath.empty()) {
    return std::nullopt;
  }

  if (target.elementId.has_value()) {
    const auto targetTagName = target.elementPath.back().qualifiedName;
    std::vector<svg::SVGElement> stack;
    stack.push_back(document.svgElement());
    while (!stack.empty()) {
      svg::SVGElement current = stack.back();
      stack.pop_back();
      if (current.tagName() == targetTagName && current.id() == *target.elementId) {
        return current;
      }

      std::vector<svg::SVGElement> children;
      for (auto child = current.firstChild(); child.has_value(); child = child->nextSibling()) {
        children.push_back(*child);
      }

      for (auto it = children.rbegin(); it != children.rend(); ++it) {
        stack.push_back(*it);
      }
    }
  }

  svg::SVGElement current = document.svgElement();
  const AttributeWritebackPathSegment& rootSegment = target.elementPath.front();
  if (rootSegment.elementChildIndex != 0 || current.tagName() != rootSegment.qualifiedName) {
    return std::nullopt;
  }

  for (std::size_t i = 1; i < target.elementPath.size(); ++i) {
    const AttributeWritebackPathSegment& segment = target.elementPath[i];
    std::size_t childIndex = 0;
    std::optional<svg::SVGElement> matchedChild;
    for (auto child = current.firstChild(); child.has_value(); child = child->nextSibling()) {
      if (childIndex == segment.elementChildIndex) {
        matchedChild = child;
        break;
      }
      ++childIndex;
    }

    if (!matchedChild.has_value() || matchedChild->tagName() != segment.qualifiedName) {
      return std::nullopt;
    }

    current = *matchedChild;
  }

  return current;
}

std::optional<TextPatch> buildAttributeWriteback(std::string_view source,
                                                 const svg::SVGElement& element,
                                                 std::string_view attrName,
                                                 std::string_view newValue) {
  const auto xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return std::nullopt;
  }

  if (const auto location = xmlNode->getNodeLocation();
      location.has_value() && location->start.offset.has_value() &&
      HasExpectedOpeningTagAt(source, location->start.offset.value(), xmlNode->tagName())) {
    return buildAttributeWritebackForNode(source, *xmlNode, attrName, newValue);
  }

  const auto target = captureAttributeWritebackTarget(element);
  if (!target.has_value()) {
    return std::nullopt;
  }

  return BuildAttributeWritebackFromCurrentSource(source, *target, attrName, newValue);
}

std::optional<TextPatch> buildAttributeWriteback(std::string_view source,
                                                 const AttributeWritebackTarget& target,
                                                 std::string_view attrName,
                                                 std::string_view newValue) {
  return BuildAttributeWritebackFromCurrentSource(source, target, attrName, newValue);
}

std::optional<TextPatch> buildAttributeRemoveWriteback(std::string_view source,
                                                       const AttributeWritebackTarget& target,
                                                       std::string_view attrName) {
  return BuildAttributeRemoveWritebackFromCurrentSource(source, target, attrName);
}

std::optional<TextPatch> buildElementRemoveWriteback(std::string_view source,
                                                     const AttributeWritebackTarget& target) {
  return BuildElementRemoveWritebackFromCurrentSource(source, target);
}

}  // namespace donner::editor
