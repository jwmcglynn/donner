#include "donner/editor/SourceSelection.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>

#include "donner/base/FileOffset.h"
#include "donner/base/xml/XMLNode.h"

namespace donner::editor {

namespace {

Coordinates FileOffsetToEditorCoordinates(const TextEditor& textEditor, const FileOffset& offset) {
  if (offset.offset.has_value()) {
    return textEditor.getCoordinatesAtByteOffset(*offset.offset);
  }

  if (offset.lineInfo.has_value()) {
    return Coordinates(static_cast<int>(offset.lineInfo->line) - 1,
                       static_cast<int>(offset.lineInfo->offsetOnLine));
  }

  return Coordinates(0, 0);
}

std::optional<std::size_t> ResolveFileOffset(std::string_view source, const FileOffset& offset) {
  const FileOffset resolved = offset.resolveOffset(source);
  if (!resolved.offset.has_value()) {
    return std::nullopt;
  }

  return std::min(*resolved.offset, source.size());
}

bool RangeContainsOffset(std::string_view source, const SourceRange& range, std::size_t offset) {
  const std::optional<std::size_t> start = ResolveFileOffset(source, range.start);
  const std::optional<std::size_t> end = ResolveFileOffset(source, range.end);
  return start.has_value() && end.has_value() && *start <= offset && offset < *end;
}

std::optional<svg::SVGElement> FindElementAtSourceOffsetImpl(const svg::SVGElement& element,
                                                             std::string_view source,
                                                             std::size_t offset) {
  const std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return std::nullopt;
  }

  const std::optional<SourceRange> range = xmlNode->getNodeLocation();
  if (!range.has_value() || !RangeContainsOffset(source, *range, offset)) {
    return std::nullopt;
  }

  for (std::optional<svg::SVGElement> child = element.firstChild(); child.has_value();
       child = child->nextSibling()) {
    std::optional<svg::SVGElement> childMatch =
        FindElementAtSourceOffsetImpl(*child, source, offset);
    if (childMatch.has_value()) {
      return childMatch;
    }
  }

  return element;
}

bool IsAncestorOrSelf(const svg::SVGElement& maybeAncestor, const svg::SVGElement& element) {
  for (std::optional<svg::SVGElement> current = element; current.has_value();
       current = current->parentElement()) {
    if (*current == maybeAncestor) {
      return true;
    }
  }

  return false;
}

bool RangeEndsAt(std::string_view source, const std::optional<SourceRange>& range,
                 std::size_t offset) {
  if (!range.has_value()) {
    return false;
  }

  const std::optional<std::size_t> end = ResolveFileOffset(source, range->end);
  return end.has_value() && *end == offset;
}

bool ElementTagEndsAt(const svg::SVGElement& element, std::string_view source, std::size_t offset) {
  const std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  return xmlNode.has_value() && (RangeEndsAt(source, xmlNode->getOpeningTagLocation(), offset) ||
                                 RangeEndsAt(source, xmlNode->getClosingTagLocation(), offset));
}

}  // namespace

bool HighlightElementSource(TextEditor& textEditor, const svg::SVGElement& element) {
  auto xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return false;
  }

  auto range = xmlNode->getNodeLocation();
  if (!range.has_value()) {
    return false;
  }

  textEditor.selectAndFocus(FileOffsetToEditorCoordinates(textEditor, range->start),
                            FileOffsetToEditorCoordinates(textEditor, range->end));
  return true;
}

std::optional<svg::SVGElement> FindElementAtSourceOffset(const svg::SVGDocument& document,
                                                         std::string_view source,
                                                         std::size_t offset) {
  if (offset >= source.size()) {
    return std::nullopt;
  }

  return FindElementAtSourceOffsetImpl(document.svgElement(), source, offset);
}

std::optional<svg::SVGElement> FindElementAtSourceCursor(const svg::SVGDocument& document,
                                                         const TextEditor& textEditor) {
  const std::size_t offset = textEditor.getByteOffsetAtCoordinates(textEditor.getCursorPosition());
  const std::string source = textEditor.getText();
  std::optional<svg::SVGElement> current = FindElementAtSourceOffset(document, source, offset);
  if (offset == 0) {
    return current;
  }

  std::optional<svg::SVGElement> previous = FindElementAtSourceOffset(document, source, offset - 1);
  if (previous.has_value() && ElementTagEndsAt(*previous, source, offset)) {
    return previous;
  }

  if (previous.has_value() && (!current.has_value() || IsAncestorOrSelf(*current, *previous))) {
    return previous;
  }

  return current;
}

}  // namespace donner::editor
