#include "donner/svg/components/StylesheetComponent.h"  // IWYU pragma: keep

#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/Stylesheet.h"
#include "donner/css/parser/StylesheetParser.h"

namespace donner::svg::components {

void StylesheetSourceMap::addSegment(std::size_t cssStartOffset, std::size_t cssEndOffset,
                                     FileOffset documentStartOffset) {
  if (cssEndOffset <= cssStartOffset || !documentStartOffset.offset.has_value()) {
    return;
  }

  segments_.push_back(StylesheetSourceMapSegment{
      .cssStartOffset = cssStartOffset,
      .cssEndOffset = cssEndOffset,
      .documentStartOffset = documentStartOffset,
  });
}

std::optional<FileOffset> StylesheetSourceMap::mapOffset(const FileOffset& localOffset) const {
  if (!localOffset.offset.has_value()) {
    return std::nullopt;
  }

  const std::size_t offset = *localOffset.offset;
  for (const StylesheetSourceMapSegment& segment : segments_) {
    if (!segment.documentStartOffset.offset.has_value()) {
      continue;
    }

    if (offset >= segment.cssStartOffset && offset <= segment.cssEndOffset) {
      const std::size_t segmentOffset = offset - segment.cssStartOffset;
      return FileOffset::Offset(*segment.documentStartOffset.offset + segmentOffset);
    }
  }

  return std::nullopt;
}

std::optional<SourceRange> StylesheetSourceMap::mapToDocumentSource(
    const SourceRange& localRange) const {
  std::optional<FileOffset> start = mapOffset(localRange.start);
  std::optional<FileOffset> end = mapOffset(localRange.end);
  if (!start.has_value() || !end.has_value() || !start->offset.has_value() ||
      !end->offset.has_value() || *end->offset < *start->offset) {
    return std::nullopt;
  }

  return SourceRange{.start = *start, .end = *end};
}

std::optional<FileOffset> StylesheetSourceMap::mapPointToDocumentSource(
    FileOffset localOffset) const {
  if (!localOffset.offset) {
    return std::nullopt;
  }
  for (const StylesheetSourceMapSegment& segment : segments_) {
    if (*localOffset.offset >= segment.cssStartOffset &&
        *localOffset.offset < segment.cssEndOffset) {
      return FileOffset::Offset(*segment.documentStartOffset.offset + *localOffset.offset -
                                segment.cssStartOffset);
    }
  }
  if (!segments_.empty() && cssLength_ == *localOffset.offset &&
      segments_.back().cssEndOffset == *cssLength_) {
    const auto& last = segments_.back();
    return FileOffset::Offset(*last.documentStartOffset.offset + last.cssEndOffset -
                              last.cssStartOffset);
  }
  return std::nullopt;
}

std::optional<std::size_t> StylesheetSourceMap::mapToLocalCssOffset(
    std::size_t documentOffset) const {
  for (const StylesheetSourceMapSegment& segment : segments_) {
    if (!segment.documentStartOffset.offset.has_value()) {
      continue;
    }

    const std::size_t documentStart = *segment.documentStartOffset.offset;
    const std::size_t documentEnd = documentStart + segment.cssEndOffset - segment.cssStartOffset;
    if (documentOffset >= documentStart && documentOffset < documentEnd) {
      return segment.cssStartOffset + documentOffset - documentStart;
    }
  }

  return std::nullopt;
}

void StylesheetComponent::parseStylesheet(const RcStringOrRef& str) {
  parseStylesheet(str, StylesheetSourceMap());
}

void StylesheetComponent::parseStylesheet(const RcStringOrRef& str,
                                          StylesheetSourceMap newSourceMap) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  parseStylesheet(str, std::move(newSourceMap), disabled);
}

void StylesheetComponent::parseStylesheet(const RcStringOrRef& str,
                                          StylesheetSourceMap newSourceMap,
                                          ParseWarningSink& warnings) {
  text = RcString(str);
  sourceMap = std::move(newSourceMap);
  stylesheet = donner::css::parser::StylesheetParser::Parse(str, warnings);

  // Every path that loads or mutates a stylesheet ends here, so recomputing the cache alongside
  // the parse keeps it from going stale.
  attributeSelectorNames.clear();
  attributeSelectorMatchesAnyName = false;
  selectorsDependOnTextContent = false;
  for (const css::SelectorRule& rule : stylesheet.rules()) {
    rule.selector.collectAttributeSelectorNames(attributeSelectorNames,
                                                attributeSelectorMatchesAnyName);
    selectorsDependOnTextContent =
        selectorsDependOnTextContent || rule.selector.dependsOnTextContent();
  }
}

}  // namespace donner::svg::components
