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
  sourceMap = StylesheetSourceMap();
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  stylesheet = donner::css::parser::StylesheetParser::Parse(str, disabled);
}

void StylesheetComponent::parseStylesheet(const RcStringOrRef& str,
                                          StylesheetSourceMap newSourceMap) {
  sourceMap = std::move(newSourceMap);
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  stylesheet = donner::css::parser::StylesheetParser::Parse(str, disabled);
}

}  // namespace donner::svg::components
