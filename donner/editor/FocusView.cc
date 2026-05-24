#include "donner/editor/FocusView.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/xml/XMLNode.h"

namespace donner::editor {
namespace {

struct ByteRange {
  std::size_t start = 0;
  std::size_t end = 0;
};

struct FragmentReference {
  std::string fragmentId;
  std::optional<std::size_t> sourceOffset;
};

struct FocusElementCollection {
  std::vector<svg::SVGElement> elements;
  std::vector<std::pair<std::size_t, svg::SVGElement>> links;
};

std::optional<ByteRange> ResolveSourceRange(std::string_view source, const SourceRange& range) {
  const FileOffset start = range.start.resolveOffset(source);
  const FileOffset end = range.end.resolveOffset(source);
  if (!start.offset.has_value() || !end.offset.has_value()) {
    return std::nullopt;
  }

  const std::size_t clampedStart = std::min(*start.offset, source.size());
  const std::size_t clampedEnd = std::min(*end.offset, source.size());
  if (clampedEnd < clampedStart) {
    return std::nullopt;
  }

  return ByteRange{.start = clampedStart, .end = clampedEnd};
}

std::vector<std::size_t> BuildLineStarts(std::string_view source) {
  std::vector<std::size_t> result;
  result.push_back(0);
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '\n') {
      result.push_back(i + 1);
    }
  }
  return result;
}

int LineForOffset(const std::vector<std::size_t>& lineStarts, std::size_t offset) {
  const auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
  return static_cast<int>(std::distance(lineStarts.begin(), it) - 1);
}

LineRange RangeToLines(const std::vector<std::size_t>& lineStarts, ByteRange range) {
  const int startLine = LineForOffset(lineStarts, range.start);
  const std::size_t inclusiveEnd = range.end > range.start ? range.end - 1 : range.start;
  const int endLine = LineForOffset(lineStarts, inclusiveEnd) + 1;
  return LineRange{.startLine = startLine, .endLine = endLine};
}

SourcePoint PointForOffset(const std::vector<std::size_t>& lineStarts, std::size_t offset) {
  const int line = LineForOffset(lineStarts, offset);
  return SourcePoint{.line = line, .column = static_cast<int>(offset - lineStarts[line])};
}

std::optional<ByteRange> NodeRange(std::string_view source, const svg::SVGElement& element) {
  std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return std::nullopt;
  }

  std::optional<SourceRange> sourceRange = xmlNode->getNodeLocation();
  if (!sourceRange.has_value()) {
    return std::nullopt;
  }

  return ResolveSourceRange(source, *sourceRange);
}

bool IsAsciiSpace(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

void AppendFragmentReference(std::vector<FragmentReference>* references,
                             std::string_view fragmentId, std::optional<std::size_t> sourceOffset) {
  if (fragmentId.empty()) {
    return;
  }

  references->push_back(FragmentReference{
      .fragmentId = std::string(fragmentId),
      .sourceOffset = sourceOffset,
  });
}

void AppendUrlFragmentReferences(std::string_view value, std::optional<std::size_t> valueOffset,
                                 std::vector<FragmentReference>* references) {
  std::size_t searchPos = 0;
  while (searchPos < value.size()) {
    const std::size_t urlPos = value.find("url(", searchPos);
    if (urlPos == std::string_view::npos) {
      return;
    }

    std::size_t index = urlPos + 4;
    while (index < value.size() && IsAsciiSpace(value[index])) {
      ++index;
    }

    char quote = '\0';
    if (index < value.size() && (value[index] == '\'' || value[index] == '"')) {
      quote = value[index];
      ++index;
      while (index < value.size() && IsAsciiSpace(value[index])) {
        ++index;
      }
    }

    if (index >= value.size() || value[index] != '#') {
      searchPos = index;
      continue;
    }

    const std::size_t fragmentStart = index + 1;
    std::size_t fragmentEnd = fragmentStart;
    while (fragmentEnd < value.size()) {
      const char ch = value[fragmentEnd];
      if ((quote != '\0' && ch == quote) || (quote == '\0' && (ch == ')' || IsAsciiSpace(ch)))) {
        break;
      }
      ++fragmentEnd;
    }

    std::optional<std::size_t> sourceOffset;
    if (valueOffset.has_value()) {
      sourceOffset = *valueOffset + index;
    }

    AppendFragmentReference(references, value.substr(fragmentStart, fragmentEnd - fragmentStart),
                            sourceOffset);
    searchPos = fragmentEnd;
  }
}

void AppendHrefFragmentReference(std::string_view value, std::optional<std::size_t> valueOffset,
                                 std::vector<FragmentReference>* references) {
  std::size_t start = 0;
  while (start < value.size() && IsAsciiSpace(value[start])) {
    ++start;
  }

  if (start >= value.size() || value[start] != '#') {
    return;
  }

  std::size_t end = value.size();
  while (end > start + 1 && IsAsciiSpace(value[end - 1])) {
    --end;
  }

  std::optional<std::size_t> sourceOffset;
  if (valueOffset.has_value()) {
    sourceOffset = *valueOffset + start;
  }

  AppendFragmentReference(references, value.substr(start + 1, end - start - 1), sourceOffset);
}

std::vector<FragmentReference> ReferencedFragments(std::string_view source,
                                                   const svg::SVGElement& element) {
  std::vector<FragmentReference> result;
  std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return result;
  }

  for (const xml::XMLQualifiedNameRef& attrName : xmlNode->attributes()) {
    std::optional<RcString> attrValue = xmlNode->getAttribute(attrName);
    if (!attrValue.has_value()) {
      continue;
    }

    std::optional<std::size_t> valueOffset;
    if (std::optional<xml::XMLAttributeSourceLocation> location =
            xmlNode->getAttributeSourceLocation(attrName)) {
      if (std::optional<ByteRange> valueRange = ResolveSourceRange(source, location->valueRange)) {
        valueOffset = valueRange->start;
      }
    }

    const std::string_view value = *attrValue;
    AppendUrlFragmentReferences(value, valueOffset, &result);
    if (attrName.name == "href") {
      AppendHrefFragmentReference(value, valueOffset, &result);
    }
  }

  return result;
}

void AppendReferencedFragmentsInSubtree(std::string_view source, const svg::SVGElement& root,
                                        std::vector<FragmentReference>* references) {
  for (const FragmentReference& reference : ReferencedFragments(source, root)) {
    references->push_back(reference);
  }

  for (std::optional<svg::SVGElement> child = root.firstChild(); child.has_value();
       child = child->nextSibling()) {
    AppendReferencedFragmentsInSubtree(source, *child, references);
  }
}

std::optional<svg::SVGElement> FindElementById(const svg::SVGElement& root, std::string_view id) {
  if (root.id() == id) {
    return root;
  }

  for (std::optional<svg::SVGElement> child = root.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (std::optional<svg::SVGElement> result = FindElementById(*child, id)) {
      return result;
    }
  }

  return std::nullopt;
}

FocusElementCollection CollectFocusElements(const svg::SVGDocument& document,
                                            const svg::SVGElement& selected) {
  FocusElementCollection result;
  std::vector<Entity> visited;
  result.elements.push_back(selected);
  visited.push_back(selected.entityHandle().entity());

  const svg::SVGElement root = document.svgElement();
  for (std::size_t i = 0; i < result.elements.size(); ++i) {
    std::vector<FragmentReference> references;
    AppendReferencedFragmentsInSubtree(document.source(), result.elements[i], &references);

    for (const FragmentReference& reference : references) {
      std::optional<svg::SVGElement> referenced = FindElementById(root, reference.fragmentId);
      if (!referenced.has_value()) {
        continue;
      }

      if (reference.sourceOffset.has_value()) {
        result.links.emplace_back(*reference.sourceOffset, *referenced);
      }

      const Entity entity = referenced->entityHandle().entity();
      if (std::ranges::find(visited, entity) != visited.end()) {
        continue;
      }

      visited.push_back(entity);
      result.elements.push_back(*referenced);
    }
  }

  return result;
}

std::vector<ByteRange> AncestorTagRanges(std::string_view source, ByteRange nodeRange) {
  std::vector<ByteRange> result;
  if (nodeRange.start >= nodeRange.end || nodeRange.start >= source.size()) {
    return result;
  }

  const std::size_t openEnd = source.find('>', nodeRange.start);
  if (openEnd == std::string_view::npos || openEnd >= nodeRange.end) {
    result.push_back(nodeRange);
    return result;
  }

  const ByteRange openTag{.start = nodeRange.start, .end = openEnd + 1};
  result.push_back(openTag);

  const std::size_t closeStart = source.rfind("</", nodeRange.end - 1);
  if (closeStart != std::string_view::npos && closeStart >= openTag.end &&
      closeStart < nodeRange.end) {
    result.push_back(ByteRange{.start = closeStart, .end = nodeRange.end});
  }

  return result;
}

bool AddAncestorTagLineRanges(std::string_view source, const std::vector<std::size_t>& lineStarts,
                              const svg::SVGElement& element, FocusPartition* partition,
                              bool required) {
  for (std::optional<svg::SVGElement> ancestor = element.parentElement(); ancestor.has_value();
       ancestor = ancestor->parentElement()) {
    std::optional<ByteRange> ancestorRange = NodeRange(source, *ancestor);
    if (!ancestorRange.has_value()) {
      return !required;
    }

    for (ByteRange tagRange : AncestorTagRanges(source, *ancestorRange)) {
      partition->dimmed.push_back(RangeToLines(lineStarts, tagRange));
    }
  }

  return true;
}

void Normalize(std::vector<LineRange>* ranges) {
  std::erase_if(*ranges, [](const LineRange& range) { return range.endLine <= range.startLine; });
  std::sort(ranges->begin(), ranges->end(), [](const LineRange& a, const LineRange& b) {
    return a.startLine != b.startLine ? a.startLine < b.startLine : a.endLine < b.endLine;
  });

  std::vector<LineRange> merged;
  for (const LineRange& range : *ranges) {
    if (merged.empty() || range.startLine > merged.back().endLine) {
      merged.push_back(range);
    } else {
      merged.back().endLine = std::max(merged.back().endLine, range.endLine);
    }
  }
  *ranges = std::move(merged);
}

bool ContainsLine(const std::vector<LineRange>& ranges, int line) {
  return std::ranges::any_of(ranges, [line](const LineRange& range) {
    return line >= range.startLine && line < range.endLine;
  });
}

std::vector<LineRange> HiddenLineRanges(int lineCount, const std::vector<LineRange>& visible) {
  std::vector<LineRange> result;
  int hiddenStart = -1;
  for (int line = 0; line < lineCount; ++line) {
    if (ContainsLine(visible, line)) {
      if (hiddenStart != -1) {
        result.push_back(LineRange{.startLine = hiddenStart, .endLine = line});
        hiddenStart = -1;
      }
    } else if (hiddenStart == -1) {
      hiddenStart = line;
    }
  }

  if (hiddenStart != -1) {
    result.push_back(LineRange{.startLine = hiddenStart, .endLine = lineCount});
  }
  return result;
}

}  // namespace

FocusPartition ComputeFocusPartition(const svg::SVGDocument& document,
                                     const svg::SVGElement& selected) {
  if (!document.hasSourceStore()) {
    return {};
  }

  const std::string_view source = document.source();
  const std::vector<std::size_t> lineStarts = BuildLineStarts(source);
  std::optional<ByteRange> selectedRange = NodeRange(source, selected);
  if (!selectedRange.has_value()) {
    return {};
  }

  FocusPartition partition;
  const FocusElementCollection focusElements = CollectFocusElements(document, selected);

  for (std::size_t i = 0; i < focusElements.elements.size(); ++i) {
    std::optional<ByteRange> elementRange =
        i == 0 ? selectedRange : NodeRange(source, focusElements.elements[i]);
    if (elementRange.has_value()) {
      partition.fullColor.push_back(RangeToLines(lineStarts, *elementRange));
    }

    if (!AddAncestorTagLineRanges(source, lineStarts, focusElements.elements[i], &partition,
                                  /*required=*/i == 0)) {
      return {};
    }
  }

  for (const auto& [fromOffset, referenced] : focusElements.links) {
    std::optional<ByteRange> targetRange = NodeRange(source, referenced);
    if (!targetRange.has_value() || fromOffset > source.size()) {
      continue;
    }

    partition.referenceLinks.push_back(FocusReferenceLink{
        .from = PointForOffset(lineStarts, fromOffset),
        .to = PointForOffset(lineStarts, targetRange->start),
    });
  }

  Normalize(&partition.fullColor);
  Normalize(&partition.dimmed);

  std::vector<LineRange> visible = partition.fullColor;
  visible.insert(visible.end(), partition.dimmed.begin(), partition.dimmed.end());
  Normalize(&visible);
  partition.hidden = HiddenLineRanges(static_cast<int>(lineStarts.size()), visible);
  return partition;
}

}  // namespace donner::editor
