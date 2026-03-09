#include "donner/base/xml/SourceDocument.h"

#include <algorithm>
#include <string>

namespace donner::xml {

SourceDocument::SourceDocument(const RcStringOrRef& text) : source_(text) {}

SourceDocument::ApplyResult::ApplyResult(RcString text, OffsetMap offsetMap)
    : text(std::move(text)), offsetMap(std::move(offsetMap)) {}

SourceDocument::OffsetMap::OffsetMap(size_t originalSize,
                                     std::vector<ReplacementInfo>&& replacements,
                                     parser::LineOffsets&& lineOffsets)
    : originalSize_(originalSize),
      replacements_(std::move(replacements)),
      lineOffsets_(std::move(lineOffsets)) {}

size_t SourceDocument::OffsetMap::mapOffset(size_t offset) const {
  int64_t delta = 0;
  for (const auto& replacement : replacements_) {
    if (offset < replacement.start) {
      break;
    }

    if (offset < replacement.end) {
      const size_t relative = offset - replacement.start;
      const size_t clamped = std::min(relative, replacement.replacementSize);
      const int64_t translated = static_cast<int64_t>(replacement.start) + replacement.deltaBefore +
                                 static_cast<int64_t>(clamped);
      return static_cast<size_t>(std::max<int64_t>(0, translated));
    }

    delta = replacement.deltaAfter;
  }

  const int64_t translated = static_cast<int64_t>(offset) + delta;
  return static_cast<size_t>(std::max<int64_t>(0, translated));
}

FileOffset SourceDocument::OffsetMap::translateOffset(const FileOffset& offset) const {
  const size_t resolvedOriginalOffset = offset.offset.value_or(originalSize_);
  const size_t mappedOffset = mapOffset(std::min(resolvedOriginalOffset, originalSize_));

  FileOffset translated = FileOffset::Offset(mappedOffset);
  translated.lineInfo = lineOffsets_.fileOffset(mappedOffset).lineInfo;
  return translated;
}

FileOffsetRange SourceDocument::OffsetMap::translateRange(const FileOffsetRange& range) const {
  return FileOffsetRange{translateOffset(range.start), translateOffset(range.end)};
}

namespace {

// Lightweight rope that gathers spans and replacement strings before materializing the final
// buffer. This avoids repeated reallocations when many edits occur in a single document.
class ReplacementRope {
public:
  explicit ReplacementRope(std::string_view source) : source_(source) {}

  void appendUnchanged(size_t start, size_t end) {
    if (end <= start) {
      return;
    }

    segments_.push_back(source_.substr(start, end - start));
    totalSize_ += end - start;
  }

  void appendReplacement(std::string_view replacement) {
    segments_.push_back(replacement);
    totalSize_ += replacement.size();
  }

  RcString build() const {
    std::string result;
    result.reserve(totalSize_);
    for (const std::string_view segment : segments_) {
      result.append(segment);
    }

    return RcString(std::move(result));
  }

private:
  std::string_view source_;
  std::vector<std::string_view> segments_;
  size_t totalSize_ = 0;
};

}  // namespace

ParseResult<SourceDocument::ApplyResult> SourceDocument::applyReplacements(
    const std::vector<Replacement>& replacements) const {
  const std::string_view source = source_;

  std::vector<OffsetMap::ReplacementInfo> resolved;
  resolved.reserve(replacements.size());

  size_t previousEnd = 0;
  int64_t cumulativeDelta = 0;

  for (const auto& replacement : replacements) {
    const FileOffset resolvedStart = replacement.range.start.resolveOffset(source);
    const FileOffset resolvedEnd = replacement.range.end.resolveOffset(source);

    if (!resolvedStart.offset.has_value() || !resolvedEnd.offset.has_value()) {
      return ParseError{RcString("Replacement is missing offset information"),
                        FileOffset::Offset(0)};
    }

    const size_t start = resolvedStart.offset.value();
    const size_t end = resolvedEnd.offset.value();

    if (start > end || end > source.size()) {
      return ParseError{RcString("Replacement range is out of bounds"), FileOffset::Offset(start)};
    }

    if (start < previousEnd) {
      return ParseError{RcString("Replacements must be non-overlapping and ordered"),
                        FileOffset::Offset(start)};
    }

    const int64_t delta =
        static_cast<int64_t>(replacement.replacement.size()) - static_cast<int64_t>(end - start);

    resolved.push_back(OffsetMap::ReplacementInfo{start, end, replacement.replacement.size(),
                                                  cumulativeDelta, cumulativeDelta + delta});

    cumulativeDelta += delta;
    previousEnd = end;
  }

  const int64_t targetSize = static_cast<int64_t>(source.size()) + cumulativeDelta;
  ReplacementRope rope(source);

  size_t cursor = 0;
  for (size_t i = 0; i < resolved.size(); ++i) {
    const auto& replacementInfo = resolved[i];
    const std::string_view replacementText = replacements[i].replacement;

    rope.appendUnchanged(cursor, replacementInfo.start);
    rope.appendReplacement(replacementText);
    cursor = replacementInfo.end;
  }

  rope.appendUnchanged(cursor, source.size());

  RcString updatedText = rope.build();
  if (static_cast<int64_t>(updatedText.size()) != std::max<int64_t>(0, targetSize)) {
    return ParseError{RcString("Unexpected rope size while applying replacements"),
                      FileOffset::Offset(0)};
  }

  SourceDocument::OffsetMap offsetMap(source.size(), std::move(resolved),
                                      parser::LineOffsets(updatedText));

  return ApplyResult(std::move(updatedText), std::move(offsetMap));
}

}  // namespace donner::xml
