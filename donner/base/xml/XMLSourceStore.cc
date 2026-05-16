#include "donner/base/xml/XMLSourceStore.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace donner::xml {

namespace {

constexpr std::uint32_t kFirstAnchorId = 1;

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

}  // namespace

XMLSourceStore::XMLSourceStore(std::string source) : source_(std::move(source)) {}

std::optional<SourceAnchorId> XMLSourceStore::createAnchor(std::size_t offset,
                                                           SourceAnchorBias bias) {
  if (!isBoundary(offset) ||
      anchors_.size() >=
          static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - kFirstAnchorId)) {
    return std::nullopt;
  }

  anchors_.push_back(Anchor{
      .offset = offset,
      .bias = bias,
      .valid = true,
  });
  return SourceAnchorId{static_cast<std::uint32_t>(anchors_.size() - 1 + kFirstAnchorId)};
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
  if (anchor == nullptr || !anchor->valid) {
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
  Anchor* anchor = findAnchor(id);
  if (anchor != nullptr) {
    anchor->valid = false;
  }
}

std::optional<XMLSourceDelta> XMLSourceStore::replace(std::size_t offset, std::size_t length,
                                                      std::string_view replacement) {
  if (offset > source_.size() || length > source_.size() - offset) {
    return std::nullopt;
  }

  const std::size_t end = offset + length;
  if (!isBoundary(offset) || !isBoundary(end) || !IsValidUtf8(replacement)) {
    return std::nullopt;
  }

  source_.replace(offset, length, replacement);
  ++sourceVersion_;

  const std::size_t insertedLength = replacement.size();
  for (Anchor& anchor : anchors_) {
    if (!anchor.valid) {
      continue;
    }

    if (length == 0) {
      if (anchor.offset > offset ||
          (anchor.offset == offset && anchor.bias == SourceAnchorBias::After)) {
        anchor.offset += insertedLength;
      }
      continue;
    }

    if (anchor.offset < offset) {
      continue;
    }

    if (anchor.offset > end) {
      anchor.offset = anchor.offset - length + insertedLength;
      continue;
    }

    if (anchor.offset == offset || anchor.offset == end) {
      anchor.offset = (anchor.bias == SourceAnchorBias::After) ? offset + insertedLength : offset;
      continue;
    }

    anchor.valid = false;
  }

  return XMLSourceDelta{
      .offset = offset,
      .removedLength = length,
      .insertedLength = insertedLength,
      .sourceVersion = sourceVersion_,
  };
}

bool XMLSourceStore::isBoundary(std::size_t offset) const {
  if (offset > source_.size()) {
    return false;
  }
  return offset == source_.size() ||
         !IsContinuationByte(static_cast<unsigned char>(source_[offset]));
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

  const std::size_t index = static_cast<std::size_t>(id.value - kFirstAnchorId);
  if (index >= anchors_.size()) {
    return nullptr;
  }
  return &anchors_[index];
}

const XMLSourceStore::Anchor* XMLSourceStore::findAnchor(SourceAnchorId id) const {
  if (!id.isValid()) {
    return nullptr;
  }

  const std::size_t index = static_cast<std::size_t>(id.value - kFirstAnchorId);
  if (index >= anchors_.size()) {
    return nullptr;
  }
  return &anchors_[index];
}

}  // namespace donner::xml
