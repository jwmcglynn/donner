#include "donner/svg/components/StylesheetComponent.h"  // IWYU pragma: keep

#include <atomic>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/css/Stylesheet.h"
#include "donner/css/parser/StylesheetParser.h"

namespace donner::svg::components {
namespace {

std::uint64_t NextStylesheetContentRevision() {
  static std::atomic<std::uint64_t> nextRevision{1};
  const std::uint64_t revision = nextRevision.fetch_add(1, std::memory_order_relaxed);
  UTILS_RELEASE_ASSERT(revision != 0);
  return revision;
}

std::size_t LayerSyntaxBytes(const css::Stylesheet& sheet) {
  if (const auto* syntax = sheet.layers()) {
    std::size_t bytes = sizeof(*syntax) + 2 * sizeof(void*) +
                        syntax->nodes.capacity() * sizeof(css::LayerNode) +
                        syntax->events.capacity() * sizeof(css::LayerEvent) +
                        syntax->fontFaceLayers.capacity() * sizeof(css::LayerId);
    for (const auto& node : syntax->nodes) {
      bytes += node.name.size();
    }
    return bytes;
  }
  return 0;
}

}  // namespace

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
  installParsedStylesheet(str, std::move(newSourceMap),
                          css::parser::StylesheetParser::Parse(str, warnings));
}

bool StylesheetComponent::applyTextEdit(const StylesheetComponent& previous,
                                        std::string_view updatedText,
                                        const StylesheetSourceMap& updatedSourceMap,
                                        std::size_t offset, std::size_t removedLength,
                                        std::size_t insertedLength,
                                        css::EditedRuleWindow* changed) {
  const css::parser::StylesheetParser::TextEdit edit{offset, removedLength, insertedLength};
  auto updated = css::parser::StylesheetParser::TryReparseTextEdit(
      previous.stylesheet, previous.text, updatedText, edit, changed);
  if (!updated) {
    return false;
  }
  installParsedStylesheet(RcStringOrRef(updatedText), updatedSourceMap, std::move(*updated));
  return true;
}

void StylesheetComponent::installParsedStylesheet(const RcStringOrRef& str,
                                                  StylesheetSourceMap newSourceMap,
                                                  css::Stylesheet parsed) {
  text = RcString(str);
  sourceMap = std::move(newSourceMap);
  stylesheet = std::move(parsed);
  importedStylesheets.clear();
  importOrigins.clear();
  combinedFontFaces.clear();
  importGeneration.reset();
  ++fontFaceRevision;

  // Every path that loads or mutates a stylesheet ends here, so recomputing the cache alongside
  // the parse keeps it from going stale.
  rebuildSelectorCaches();
}

void StylesheetComponent::setImportedStylesheets(std::vector<css::Stylesheet>&& imported,
                                                 std::uint64_t generation,
                                                 std::vector<StylesheetImportOrigin> origins) {
  importedStylesheets = std::move(imported);
  importOrigins = std::move(origins);
  importGeneration = generation;
  ++fontFaceRevision;

  combinedFontFaces.clear();
  for (const css::Stylesheet& sheet : importedStylesheets) {
    combinedFontFaces.insert(combinedFontFaces.end(), sheet.fontFaces().begin(),
                             sheet.fontFaces().end());
  }
  if (!combinedFontFaces.empty()) {
    combinedFontFaces.insert(combinedFontFaces.end(), stylesheet.fontFaces().begin(),
                             stylesheet.fontFaces().end());
  }
  rebuildSelectorCaches();
}

void StylesheetComponent::rebuildSelectorCaches() {
  // Both parsing and import installation change the rules named by the cache key.
  contentRevision_ = NextStylesheetContentRevision();
  std::vector<const css::Stylesheet*> sheets;
  sheets.reserve(importedStylesheets.size() + 1);
  importedRuleIndexes.clear();
  for (const css::Stylesheet& sheet : importedStylesheets) {
    sheets.push_back(&sheet);
    importedRuleIndexes.emplace_back(sheet.rules());
  }
  sheets.push_back(&stylesheet);
  dependencySummary = SelectorDependencySummary(sheets);
  ruleIndex = RuleIndex(stylesheet.rules());
}

std::size_t StylesheetComponent::selectorCacheBytes() const {
  std::size_t bytes = ruleIndex.retainedBytes() + dependencySummary.retainedBytes() +
                      importedRuleIndexes.capacity() * sizeof(RuleIndex) +
                      importOrigins.capacity() * sizeof(StylesheetImportOrigin) +
                      LayerSyntaxBytes(stylesheet);
  for (std::size_t i = 0; i < importedRuleIndexes.size(); ++i) {
    bytes += importedRuleIndexes[i].retainedBytes() + LayerSyntaxBytes(importedStylesheets[i]);
  }
  return bytes;
}

}  // namespace donner::svg::components
