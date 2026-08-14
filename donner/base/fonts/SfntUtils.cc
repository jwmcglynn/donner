#include "donner/base/fonts/SfntUtils.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace donner::fonts {

namespace {

constexpr uint32_t kSfntTrueType = 0x00010000;
constexpr uint32_t kSfntCff = 0x4F54544F;    // "OTTO"
constexpr uint32_t kSfntApple = 0x74727565;  // "true"
constexpr uint32_t kSfntType1 = 0x74797031;  // "typ1"

constexpr uint16_t kArg1And2AreWords = 0x0001;
constexpr uint16_t kArgsAreXyValues = 0x0002;
constexpr uint16_t kWeHaveAScale = 0x0008;
constexpr uint16_t kMoreComponents = 0x0020;
constexpr uint16_t kWeHaveAnXAndYScale = 0x0040;
constexpr uint16_t kWeHaveATwoByTwo = 0x0080;
constexpr uint16_t kWeHaveInstructions = 0x0100;
constexpr uint16_t kScaledComponentOffset = 0x0800;
constexpr uint16_t kUnscaledComponentOffset = 0x1000;
constexpr uint16_t kKnownCompoundFlags = 0x1FEF;

struct DependencyRange {
  uint32_t begin = 0;
  uint32_t count = 0;
};

struct DfsFrame {
  uint16_t glyph = 0;
  uint32_t nextDependency = 0;
};

bool IsSfntMagic(uint32_t magic) {
  return magic == kSfntTrueType || magic == kSfntCff || magic == kSfntApple || magic == kSfntType1;
}

bool HasBytes(std::span<const uint8_t> data, size_t offset, size_t length) {
  return offset <= data.size() && length <= data.size() - offset;
}

bool AddWithin(size_t value, size_t increment, size_t limit, size_t* result) {
  if (value > limit || increment > limit - value) {
    return false;
  }
  *result = value + increment;
  return true;
}

bool ValidateSimpleGlyph(std::span<const uint8_t> glyph, uint16_t contourCount,
                         size_t* totalPoints) {
  const size_t endpointBytes = static_cast<size_t>(contourCount) * 2;
  if (!HasBytes(glyph, 10, endpointBytes + 2)) {
    return false;
  }

  uint16_t previousEndpoint = 0;
  for (size_t i = 0; i < contourCount; ++i) {
    const uint16_t endpoint = ReadBe16(glyph.data() + 10 + i * 2);
    if (i != 0 && endpoint <= previousEndpoint) {
      return false;
    }
    previousEndpoint = endpoint;
  }

  const size_t pointCount = static_cast<size_t>(previousEndpoint) + 1;
  if (!AddWithin(*totalPoints, pointCount, kMaximumSimpleGlyphPoints, totalPoints)) {
    return false;
  }

  size_t cursor = 10 + endpointBytes;
  const size_t instructionLength = ReadBe16(glyph.data() + cursor);
  cursor += 2;
  if (!HasBytes(glyph, cursor, instructionLength)) {
    return false;
  }
  cursor += instructionLength;

  size_t decodedPoints = 0;
  size_t coordinateBytes = 0;
  while (decodedPoints < pointCount) {
    if (!HasBytes(glyph, cursor, 1)) {
      return false;
    }
    const uint8_t flags = glyph[cursor++];
    size_t runLength = 1;
    if ((flags & 0x08) != 0) {
      if (!HasBytes(glyph, cursor, 1)) {
        return false;
      }
      runLength += glyph[cursor++];
    }
    if (runLength > pointCount - decodedPoints) {
      return false;
    }

    const size_t xBytesPerPoint = (flags & 0x02) != 0 ? 1 : ((flags & 0x10) != 0 ? 0 : 2);
    const size_t yBytesPerPoint = (flags & 0x04) != 0 ? 1 : ((flags & 0x20) != 0 ? 0 : 2);
    const size_t bytesPerPoint = xBytesPerPoint + yBytesPerPoint;
    if (bytesPerPoint != 0 &&
        runLength > (std::numeric_limits<size_t>::max() - coordinateBytes) / bytesPerPoint) {
      return false;
    }
    coordinateBytes += runLength * bytesPerPoint;
    decodedPoints += runLength;
  }

  return HasBytes(glyph, cursor, coordinateBytes);
}

bool ParseCompoundGlyph(std::span<const uint8_t> glyph, size_t numGlyphs,
                        std::vector<uint16_t>* dependencies, size_t* totalComponents) {
  size_t cursor = 10;
  uint16_t flags = 0;
  bool hasInstructions = false;
  do {
    if (!HasBytes(glyph, cursor, 4) || *totalComponents >= kMaximumCompoundComponentRecords) {
      return false;
    }

    flags = ReadBe16(glyph.data() + cursor);
    const uint16_t dependency = ReadBe16(glyph.data() + cursor + 2);
    cursor += 4;
    ++*totalComponents;

    if ((flags & ~kKnownCompoundFlags) != 0 || (flags & kArgsAreXyValues) == 0 ||
        dependency >= numGlyphs ||
        ((flags & kScaledComponentOffset) != 0 && (flags & kUnscaledComponentOffset) != 0)) {
      return false;
    }

    const unsigned int transformKinds = ((flags & kWeHaveAScale) != 0 ? 1u : 0u) +
                                        ((flags & kWeHaveAnXAndYScale) != 0 ? 1u : 0u) +
                                        ((flags & kWeHaveATwoByTwo) != 0 ? 1u : 0u);
    if (transformKinds > 1) {
      return false;
    }

    size_t payloadBytes = (flags & kArg1And2AreWords) != 0 ? 4 : 2;
    if ((flags & kWeHaveAScale) != 0) {
      payloadBytes += 2;
    } else if ((flags & kWeHaveAnXAndYScale) != 0) {
      payloadBytes += 4;
    } else if ((flags & kWeHaveATwoByTwo) != 0) {
      payloadBytes += 8;
    }
    if (!HasBytes(glyph, cursor, payloadBytes)) {
      return false;
    }
    cursor += payloadBytes;
    dependencies->push_back(dependency);
    hasInstructions = hasInstructions || (flags & kWeHaveInstructions) != 0;
  } while ((flags & kMoreComponents) != 0);

  if (hasInstructions) {
    if (!HasBytes(glyph, cursor, 2)) {
      return false;
    }
    const size_t instructionLength = ReadBe16(glyph.data() + cursor);
    cursor += 2;
    if (!HasBytes(glyph, cursor, instructionLength)) {
      return false;
    }
  }
  return true;
}

bool ValidateDependencyGraph(std::span<const DependencyRange> ranges,
                             std::span<const uint16_t> dependencies) {
  std::vector<uint8_t> colors(ranges.size(), 0);
  std::vector<uint32_t> expandedComponents(ranges.size(), 0);
  std::array<DfsFrame, kMaximumCompoundGlyphDepth> stack{};

  for (size_t root = 0; root < ranges.size(); ++root) {
    if (colors[root] != 0) {
      continue;
    }

    size_t depth = 1;
    stack[0] = DfsFrame{static_cast<uint16_t>(root), 0};
    colors[root] = 1;
    while (depth != 0) {
      DfsFrame& frame = stack[depth - 1];
      const DependencyRange range = ranges[frame.glyph];
      if (frame.nextDependency < range.count) {
        const uint16_t child = dependencies[range.begin + frame.nextDependency++];
        if (colors[child] == 1) {
          return false;
        }
        if (colors[child] == 0) {
          if (depth >= kMaximumCompoundGlyphDepth) {
            return false;
          }
          colors[child] = 1;
          stack[depth++] = DfsFrame{child, 0};
        }
        continue;
      }

      size_t expanded = 0;
      for (size_t i = 0; i < range.count; ++i) {
        const uint16_t child = dependencies[range.begin + i];
        const size_t childWork = static_cast<size_t>(expandedComponents[child]) + 1;
        if (!AddWithin(expanded, childWork, kMaximumCompoundComponentRecords, &expanded)) {
          return false;
        }
      }
      expandedComponents[frame.glyph] = static_cast<uint32_t>(expanded);
      colors[frame.glyph] = 2;
      --depth;
    }
  }
  return true;
}

}  // namespace

uint32_t SfntTag(std::string_view tag) {
  if (tag.size() != 4) {
    return 0;
  }
  return (static_cast<uint32_t>(static_cast<uint8_t>(tag[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(tag[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(tag[2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(tag[3]));
}

SfntFont::SfntFont() = default;
SfntFont::~SfntFont() = default;
SfntFont::SfntFont(SfntFont&&) noexcept = default;
SfntFont& SfntFont::operator=(SfntFont&&) noexcept = default;

std::optional<SfntFont> SfntFont::Validate(std::span<const uint8_t> data) {
  if (data.size() < 12 || !IsSfntMagic(ReadBe32(data.data()))) {
    return std::nullopt;
  }

  const size_t numTables = ReadBe16(data.data() + 4);
  if (numTables > kMaximumSfntTables || numTables > (data.size() - 12) / 16) {
    return std::nullopt;
  }

  SfntFont font;
  font.numTables_ = numTables;
  if (numTables != 0) {
    font.tables_ = std::make_unique<TableRecord[]>(numTables);
  }

  for (size_t i = 0; i < numTables; ++i) {
    const size_t recordOffset = 12 + i * 16;
    const uint32_t tableOffset = ReadBe32(data.data() + recordOffset + 8);
    const uint32_t tableLength = ReadBe32(data.data() + recordOffset + 12);
    if (!HasBytes(data, tableOffset, tableLength)) {
      return std::nullopt;
    }
    font.tables_[i] = TableRecord{ReadBe32(data.data() + recordOffset), tableOffset, tableLength};
  }

  if (numTables > 1) {
    std::sort(font.tables_.get(), font.tables_.get() + numTables,
              [](const TableRecord& lhs, const TableRecord& rhs) { return lhs.tag < rhs.tag; });
  }
  for (size_t i = 1; i < numTables; ++i) {
    if (font.tables_[i - 1].tag == font.tables_[i].tag) {
      return std::nullopt;
    }
  }

  const auto glyf = font.findTable(data, "glyf");
  if (!glyf) {
    return font;
  }

  const auto head = font.findTable(data, "head");
  const auto maxp = font.findTable(data, "maxp");
  const auto loca = font.findTable(data, "loca");
  if (!head || head->size() < 54 || !maxp || maxp->size() < 6 || !loca) {
    return std::nullopt;
  }

  const int16_t locaFormat = static_cast<int16_t>(ReadBe16(head->data() + 50));
  if (locaFormat != 0 && locaFormat != 1) {
    return std::nullopt;
  }
  font.numGlyphs_ = ReadBe16(maxp->data() + 4);
  const size_t locaCount = font.numGlyphs_ + 1;
  const size_t locaEntrySize = locaFormat == 0 ? 2 : 4;
  if (locaCount > loca->size() / locaEntrySize) {
    return std::nullopt;
  }
  font.glyphOffsets_ = std::make_unique<uint32_t[]>(locaCount);

  uint32_t previousOffset = 0;
  for (size_t i = 0; i < locaCount; ++i) {
    const uint32_t offset = locaFormat == 0
                                ? static_cast<uint32_t>(ReadBe16(loca->data() + i * 2)) * 2
                                : ReadBe32(loca->data() + i * 4);
    if (offset < previousOffset || offset > glyf->size()) {
      return std::nullopt;
    }
    font.glyphOffsets_[i] = offset;
    previousOffset = offset;
  }

  std::vector<DependencyRange> ranges(font.numGlyphs_);
  std::vector<uint16_t> dependencies;
  dependencies.reserve(std::min(glyf->size() / 4, kMaximumCompoundComponentRecords));
  size_t totalComponents = 0;
  size_t totalPoints = 0;
  for (size_t glyphIndex = 0; glyphIndex < font.numGlyphs_; ++glyphIndex) {
    const size_t start = font.glyphOffsets_[glyphIndex];
    const size_t end = font.glyphOffsets_[glyphIndex + 1];
    if (start == end) {
      continue;
    }

    const std::span<const uint8_t> glyph = glyf->subspan(start, end - start);
    if (glyph.size() < 10) {
      return std::nullopt;
    }
    const int16_t contourCount = static_cast<int16_t>(ReadBe16(glyph.data()));
    if (contourCount >= 0) {
      if (contourCount != 0 &&
          !ValidateSimpleGlyph(glyph, static_cast<uint16_t>(contourCount), &totalPoints)) {
        return std::nullopt;
      }
      continue;
    }
    if (contourCount != -1) {
      return std::nullopt;
    }

    const size_t dependencyStart = dependencies.size();
    if (!ParseCompoundGlyph(glyph, font.numGlyphs_, &dependencies, &totalComponents)) {
      return std::nullopt;
    }
    ranges[glyphIndex] =
        DependencyRange{static_cast<uint32_t>(dependencyStart),
                        static_cast<uint32_t>(dependencies.size() - dependencyStart)};
  }

  if (!ValidateDependencyGraph(ranges, dependencies)) {
    return std::nullopt;
  }
  return font;
}

const SfntFont::TableRecord* SfntFont::findRecord(uint32_t tag) const {
  if (numTables_ == 0) {
    return nullptr;
  }
  const TableRecord* const begin = tables_.get();
  const TableRecord* const end = begin + numTables_;
  const TableRecord* const found = std::lower_bound(
      begin, end, tag,
      [](const TableRecord& record, uint32_t value) { return record.tag < value; });
  return found != end && found->tag == tag ? found : nullptr;
}

std::optional<std::span<const uint8_t>> SfntFont::findTable(std::span<const uint8_t> data,
                                                            std::string_view tag) const {
  if (tag.size() != 4) {
    return std::nullopt;
  }
  const TableRecord* const record = findRecord(SfntTag(tag));
  if (!record || !HasBytes(data, record->offset, record->length)) {
    return std::nullopt;
  }
  return data.subspan(record->offset, record->length);
}

bool SfntFont::hasTable(std::string_view tag) const {
  return tag.size() == 4 && findRecord(SfntTag(tag)) != nullptr;
}

size_t SfntFont::retainedBytes() const {
  return numTables_ * sizeof(TableRecord) +
         (glyphOffsets_ ? (numGlyphs_ + 1) * sizeof(uint32_t) : 0);
}

bool ValidateSfnt(std::span<const uint8_t> data) {
  return SfntFont::Validate(data).has_value();
}

std::optional<std::span<const uint8_t>> FindSfntTable(std::span<const uint8_t> data,
                                                      std::string_view tag) {
  if (tag.size() != 4) {
    return std::nullopt;
  }
  auto font = SfntFont::Validate(data);
  return font ? font->findTable(data, tag) : std::nullopt;
}

}  // namespace donner::fonts
