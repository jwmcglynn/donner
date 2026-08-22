#include "donner/base/xml/XMLSourceStore.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace donner::xml {

namespace {

void SaturatingIncrement(std::uint64_t& value) {
  if (value != std::numeric_limits<std::uint64_t>::max()) {
    ++value;
  }
}

void SaturatingAdd(std::uint64_t& value, std::size_t amount) {
  const std::uint64_t unsignedAmount = static_cast<std::uint64_t>(amount);
  if (unsignedAmount > std::numeric_limits<std::uint64_t>::max() - value) {
    value = std::numeric_limits<std::uint64_t>::max();
  } else {
    value += unsignedAmount;
  }
}

bool IsContinuationByte(unsigned char byte) {
  return (byte & 0xC0U) == 0x80U;
}

int Utf8SequenceLength(unsigned char lead) {
  if ((lead & 0x80U) == 0x00U) {
    return 1;
  }
  if ((lead & 0xE0U) == 0xC0U) {
    return 2;
  }
  if ((lead & 0xF0U) == 0xE0U) {
    return 3;
  }
  if ((lead & 0xF8U) == 0xF0U) {
    return 4;
  }
  return 0;
}

std::int32_t DecodeUtf8(const unsigned char* bytes, int length) {
  for (int i = 1; i < length; ++i) {
    if (!IsContinuationByte(bytes[i])) {
      return -1;
    }
  }

  std::int32_t codepoint = 0;
  switch (length) {
    case 1: codepoint = static_cast<std::int32_t>(bytes[0] & 0x7FU); break;
    case 2:
      codepoint = ((static_cast<std::int32_t>(bytes[0]) & 0x1F) << 6) |
                  (static_cast<std::int32_t>(bytes[1]) & 0x3F);
      break;
    case 3:
      codepoint = ((static_cast<std::int32_t>(bytes[0]) & 0x0F) << 12) |
                  ((static_cast<std::int32_t>(bytes[1]) & 0x3F) << 6) |
                  (static_cast<std::int32_t>(bytes[2]) & 0x3F);
      break;
    case 4:
      codepoint = ((static_cast<std::int32_t>(bytes[0]) & 0x07) << 18) |
                  ((static_cast<std::int32_t>(bytes[1]) & 0x3F) << 12) |
                  ((static_cast<std::int32_t>(bytes[2]) & 0x3F) << 6) |
                  (static_cast<std::int32_t>(bytes[3]) & 0x3F);
      break;
    default: return -1;
  }

  if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
      (length == 4 && codepoint < 0x10000)) {
    return -1;
  }
  if (codepoint > 0x10FFFF) {
    return -1;
  }
  if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
    return -1;
  }
  return codepoint;
}

bool IsValidXmlSourceCodepoint(std::int32_t codepoint) {
  if (codepoint <= 0) {
    return false;
  }
  if (codepoint < 0x20 && codepoint != 0x09 && codepoint != 0x0A && codepoint != 0x0D) {
    return false;
  }
  if (codepoint == 0xFFFE || codepoint == 0xFFFF) {
    return false;
  }
  return true;
}

bool ReplacementRangeIsValid(std::size_t sourceSize, std::size_t offset, std::size_t length) {
  return offset <= sourceSize && length <= sourceSize - offset;
}

bool ReplacementFitsLimit(std::size_t sourceSize, std::size_t removedLength,
                          std::size_t insertedLength, std::size_t maximumSourceSize) {
  const std::size_t retainedSourceSize = sourceSize - removedLength;
  return retainedSourceSize <= maximumSourceSize &&
         insertedLength <= maximumSourceSize - retainedSourceSize;
}

template <typename AnchorT>
bool UpdateAnchorForReplacement(AnchorT& anchor, std::size_t offset, std::size_t removedLength,
                                std::size_t end, std::size_t insertedLength) {
  if (removedLength == 0) {
    if (anchor.offset > offset ||
        (anchor.offset == offset && anchor.bias == SourceAnchorBias::After)) {
      anchor.offset += insertedLength;
    }
    return false;
  }
  if (anchor.offset < offset) {
    return false;
  }
  if (anchor.offset > end) {
    anchor.offset = anchor.offset - removedLength + insertedLength;
    return false;
  }
  if (anchor.offset == offset || anchor.offset == end) {
    anchor.offset = anchor.bias == SourceAnchorBias::After ? offset + insertedLength : offset;
    return false;
  }
  return true;
}

template <typename AnchorMapT, typename RetireCallbackT>
void UpdateAnchorsForReplacement(AnchorMapT& anchors, std::size_t offset, std::size_t removedLength,
                                 std::size_t insertedLength, RetireCallbackT&& retireCallback) {
  const std::size_t end = offset + removedLength;
  for (auto it = anchors.begin(); it != anchors.end();) {
    if (UpdateAnchorForReplacement(it->second, offset, removedLength, end, insertedLength)) {
      it = anchors.erase(it);
      retireCallback();
    } else {
      ++it;
    }
  }
}

}  // namespace

XMLSourceStore::XMLSourceStore(std::string source, std::size_t maximumSourceSize)
    : XMLSourceStore(std::move(source), ResourceLimits{.maximumSourceSize = maximumSourceSize}) {}

XMLSourceStore::XMLSourceStore(std::string source, ResourceLimits limits)
    : source_(std::make_shared<std::string>(std::move(source))), resourceLimits_(limits) {
  resourceLimits_.maximumSourceSize = std::max(resourceLimits_.maximumSourceSize, source_->size());
}

RcString XMLSourceStore::sourceReference() const {
  return RcString::FromSharedStorage(source_, std::string_view(*source_));
}

XMLSourceStore::ResourceStats XMLSourceStore::resourceStats() const {
  return ResourceStats{
      .liveAnchorCount = anchors_.size(),
      .peakLiveAnchorCount = peakLiveAnchorCount_,
      .totalCreatedAnchors = totalCreatedAnchors_,
      .totalRetiredAnchors = totalRetiredAnchors_,
      .totalAnchorUpdateWork = totalAnchorUpdateWork_,
      .lastAnchorUpdateWork = lastAnchorUpdateWork_,
      .anchorUpdateWorkRejections = anchorUpdateWorkRejections_,
  };
}

bool XMLSourceStore::setResourceLimits(ResourceLimits limits) {
  if (limits.maximumSourceSize < source_->size() ||
      limits.maximumLiveAnchorCount < anchors_.size()) {
    return false;
  }

  resourceLimits_ = limits;
  return true;
}

std::optional<SourceAnchorId> XMLSourceStore::createAnchor(std::size_t offset,
                                                           SourceAnchorBias bias) {
  if (!isBoundary(offset) || anchors_.size() >= resourceLimits_.maximumLiveAnchorCount ||
      nextAnchorId_ == 0) {
    return std::nullopt;
  }

  const SourceAnchorId id{nextAnchorId_++};
  anchors_.emplace(id.value, Anchor{
                                 .offset = offset,
                                 .bias = bias,
                             });
  recordCreatedAnchor();
  return id;
}

std::optional<SourceAnchorSpan> XMLSourceStore::createSpan(std::size_t start, std::size_t end,
                                                           SourceAnchorBias startBias,
                                                           SourceAnchorBias endBias) {
  if (end < start) {
    return std::nullopt;
  }

  std::optional<SourceAnchorId> startAnchor = createAnchor(start, startBias);
  if (!startAnchor.has_value()) {
    return std::nullopt;
  }

  std::optional<SourceAnchorId> endAnchor = createAnchor(end, endBias);
  if (!endAnchor.has_value()) {
    invalidateAnchor(*startAnchor);
    return std::nullopt;
  }

  return SourceAnchorSpan{
      .start = *startAnchor,
      .end = *endAnchor,
  };
}

std::optional<std::size_t> XMLSourceStore::resolveAnchor(SourceAnchorId id) const {
  const Anchor* anchor = findAnchor(id);
  if (anchor == nullptr) {
    return std::nullopt;
  }

  return anchor->offset;
}

std::optional<ResolvedSourceSpan> XMLSourceStore::resolveSpan(SourceAnchorSpan span) const {
  std::optional<std::size_t> start = resolveAnchor(span.start);
  std::optional<std::size_t> end = resolveAnchor(span.end);
  if (!start.has_value() || !end.has_value() || *end < *start) {
    return std::nullopt;
  }

  return ResolvedSourceSpan{
      .start = *start,
      .end = *end,
  };
}

void XMLSourceStore::invalidateAnchor(SourceAnchorId id) {
  if (id.isValid() && anchors_.erase(id.value) != 0) {
    recordRetiredAnchor();
  }
}

std::optional<XMLSourceDelta> XMLSourceStore::replace(std::size_t offset, std::size_t length,
                                                      std::string_view replacement) {
  if (!ReplacementRangeIsValid(source_->size(), offset, length)) {
    return std::nullopt;
  }
  if (!ReplacementFitsLimit(source_->size(), length, replacement.size(),
                            resourceLimits_.maximumSourceSize)) {
    return std::nullopt;
  }

  const std::size_t end = offset + length;
  if (!isBoundary(offset) || !isBoundary(end) || !IsValidUtf8(replacement)) {
    return std::nullopt;
  }

  lastAnchorUpdateWork_ = 0;
  if (anchors_.size() > resourceLimits_.maximumAnchorUpdateWorkPerEdit) {
    SaturatingIncrement(anchorUpdateWorkRejections_);
    return std::nullopt;
  }

  ensureUniqueSource();
  source_->replace(offset, length, replacement);
  ++sourceVersion_;

  const std::size_t insertedLength = replacement.size();
  const std::size_t anchorUpdateWork = anchors_.size();
  UpdateAnchorsForReplacement(anchors_, offset, length, insertedLength,
                              [this]() { recordRetiredAnchor(); });
  lastAnchorUpdateWork_ = anchorUpdateWork;
  SaturatingAdd(totalAnchorUpdateWork_, anchorUpdateWork);

  return XMLSourceDelta{
      .offset = offset,
      .removedLength = length,
      .insertedLength = insertedLength,
      .sourceVersion = sourceVersion_,
  };
}

FileOffset XMLSourceStore::resolveLineInfo(FileOffset offset) const {
  if (!offset.offset.has_value()) {
    return offset;
  }

  if (!lineOffsets_.has_value() || lineOffsetsVersion_ != sourceVersion_) {
    lineOffsets_.emplace(std::string_view(*source_));
    lineOffsetsVersion_ = sourceVersion_;
  }

  return lineOffsets_->fileOffset(offset.offset.value());
}

bool XMLSourceStore::isBoundary(std::size_t offset) const {
  if (offset > source_->size()) {
    return false;
  }
  return offset == source_->size() ||
         !IsContinuationByte(static_cast<unsigned char>((*source_)[offset]));
}

void XMLSourceStore::ensureUniqueSource() {
  if (source_.use_count() != 1) {
    source_ = std::make_shared<std::string>(*source_);
  }
}

bool XMLSourceStore::IsValidUtf8(std::string_view value) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
  std::size_t i = 0;
  while (i < value.size()) {
    const int length = Utf8SequenceLength(bytes[i]);
    if (length == 0 || i + static_cast<std::size_t>(length) > value.size()) {
      return false;
    }
    const std::int32_t codepoint = DecodeUtf8(bytes + i, length);
    if (!IsValidXmlSourceCodepoint(codepoint)) {
      return false;
    }
    i += static_cast<std::size_t>(length);
  }
  return true;
}

XMLSourceStore::Anchor* XMLSourceStore::findAnchor(SourceAnchorId id) {
  if (!id.isValid()) {
    return nullptr;
  }

  auto it = anchors_.find(id.value);
  return it != anchors_.end() ? &it->second : nullptr;
}

const XMLSourceStore::Anchor* XMLSourceStore::findAnchor(SourceAnchorId id) const {
  if (!id.isValid()) {
    return nullptr;
  }

  auto it = anchors_.find(id.value);
  return it != anchors_.end() ? &it->second : nullptr;
}

void XMLSourceStore::recordCreatedAnchor() {
  SaturatingIncrement(totalCreatedAnchors_);
  peakLiveAnchorCount_ = std::max(peakLiveAnchorCount_, anchors_.size());
}

void XMLSourceStore::recordRetiredAnchor() {
  SaturatingIncrement(totalRetiredAnchors_);
}

}  // namespace donner::xml
