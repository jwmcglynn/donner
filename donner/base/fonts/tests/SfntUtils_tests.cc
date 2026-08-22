#include "donner/base/fonts/SfntUtils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include "donner/base/fonts/CffOutlineComplexity.h"

namespace donner::fonts {
namespace {

void WriteBe16(std::vector<uint8_t>* data, size_t offset, uint16_t value) {
  (*data)[offset] = static_cast<uint8_t>(value >> 8);
  (*data)[offset + 1] = static_cast<uint8_t>(value);
}

void WriteBe32(std::vector<uint8_t>* data, size_t offset, uint32_t value) {
  (*data)[offset] = static_cast<uint8_t>(value >> 24);
  (*data)[offset + 1] = static_cast<uint8_t>(value >> 16);
  (*data)[offset + 2] = static_cast<uint8_t>(value >> 8);
  (*data)[offset + 3] = static_cast<uint8_t>(value);
}

struct TableSpec {
  std::string_view tag;
  std::vector<uint8_t> bytes;
};

std::vector<uint8_t> MakeSfnt(std::vector<TableSpec> tables, uint32_t magic = 0x00010000) {
  const size_t directorySize = 12 + tables.size() * 16;
  std::vector<uint8_t> result(directorySize, 0);
  WriteBe32(&result, 0, magic);
  WriteBe16(&result, 4, static_cast<uint16_t>(tables.size()));

  size_t dataOffset = directorySize;
  for (size_t i = 0; i < tables.size(); ++i) {
    dataOffset = (dataOffset + 3) & ~size_t{3};
    result.resize(dataOffset + tables[i].bytes.size());
    const size_t recordOffset = 12 + i * 16;
    EXPECT_EQ(tables[i].tag.size(), 4u);
    std::copy(tables[i].tag.begin(), tables[i].tag.end(), result.begin() + recordOffset);
    WriteBe32(&result, recordOffset + 8, static_cast<uint32_t>(dataOffset));
    WriteBe32(&result, recordOffset + 12, static_cast<uint32_t>(tables[i].bytes.size()));
    std::copy(tables[i].bytes.begin(), tables[i].bytes.end(), result.begin() + dataOffset);
    dataOffset += tables[i].bytes.size();
  }
  return result;
}

uint8_t EncodeSmallInteger(size_t value) {
  EXPECT_LE(value, 107u);
  return static_cast<uint8_t>(value + 139);
}

std::vector<uint8_t> EncodeDictInteger(size_t value) {
  if (value <= 107) {
    return {EncodeSmallInteger(value)};
  }
  if (value <= 32767) {
    return {28, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
  }
  EXPECT_LE(value, uint32_t{0x7FFFFFFF});
  return {29, static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
          static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
}

void AppendIndexOffset(std::vector<uint8_t>* result, size_t offset, uint8_t offsetSize) {
  for (uint8_t byte = offsetSize; byte != 0; --byte) {
    result->push_back(static_cast<uint8_t>(offset >> ((byte - 1) * 8)));
  }
}

std::vector<uint8_t> MakeCffIndex(const std::vector<std::vector<uint8_t>>& objects, bool cff2) {
  std::vector<uint8_t> result(cff2 ? 4u : 2u, 0);
  if (cff2) {
    WriteBe32(&result, 0, static_cast<uint32_t>(objects.size()));
  } else {
    WriteBe16(&result, 0, static_cast<uint16_t>(objects.size()));
  }
  if (objects.empty()) {
    return result;
  }

  size_t dataSize = 0;
  for (const std::vector<uint8_t>& object : objects) {
    dataSize += object.size();
  }
  const size_t finalOffset = dataSize + 1;
  const uint8_t offsetSize =
      finalOffset <= 0xFF ? 1 : (finalOffset <= 0xFFFF ? 2 : (finalOffset <= 0xFFFFFF ? 3 : 4));
  result.push_back(offsetSize);
  size_t offset = 1;
  for (const std::vector<uint8_t>& object : objects) {
    AppendIndexOffset(&result, offset, offsetSize);
    offset += object.size();
  }
  AppendIndexOffset(&result, offset, offsetSize);
  for (const std::vector<uint8_t>& object : objects) {
    result.insert(result.end(), object.begin(), object.end());
  }
  return result;
}

std::vector<uint8_t> MakeCff1WithSubrs(std::vector<uint8_t> charString,
                                       std::vector<std::vector<uint8_t>> localSubrs,
                                       std::vector<std::vector<uint8_t>> globalSubrs = {}) {
  const std::vector<uint8_t> nameIndex = MakeCffIndex({{'A'}}, false);
  const std::vector<uint8_t> stringIndex = MakeCffIndex({}, false);
  const std::vector<uint8_t> globalIndex = MakeCffIndex(globalSubrs, false);
  const std::vector<uint8_t> localIndex = MakeCffIndex(localSubrs, false);
  const std::vector<uint8_t> charStrings = MakeCffIndex({std::move(charString)}, false);

  std::vector<uint8_t> topDict;
  std::vector<uint8_t> topIndex;
  std::vector<uint8_t> privateDict;
  size_t previousPrivateOffset = std::numeric_limits<size_t>::max();
  for (int iteration = 0; iteration < 8; ++iteration) {
    topIndex = MakeCffIndex({topDict}, false);
    const size_t privateOffset =
        4 + nameIndex.size() + topIndex.size() + stringIndex.size() + globalIndex.size();
    privateDict.clear();
    if (!localSubrs.empty()) {
      privateDict = EncodeDictInteger(2);
      privateDict.push_back(19);
    }
    const size_t charStringsOffset =
        privateOffset + privateDict.size() + (localSubrs.empty() ? 0 : localIndex.size());

    std::vector<uint8_t> nextTop = EncodeDictInteger(charStringsOffset);
    nextTop.push_back(17);
    if (!privateDict.empty()) {
      const std::vector<uint8_t> privateSize = EncodeDictInteger(privateDict.size());
      const std::vector<uint8_t> encodedPrivateOffset = EncodeDictInteger(privateOffset);
      nextTop.insert(nextTop.end(), privateSize.begin(), privateSize.end());
      nextTop.insert(nextTop.end(), encodedPrivateOffset.begin(), encodedPrivateOffset.end());
      nextTop.push_back(18);
    }
    if (nextTop == topDict && privateOffset == previousPrivateOffset) {
      break;
    }
    topDict = std::move(nextTop);
    previousPrivateOffset = privateOffset;
  }
  topIndex = MakeCffIndex({topDict}, false);

  std::vector<uint8_t> result{1, 0, 4, 4};
  result.insert(result.end(), nameIndex.begin(), nameIndex.end());
  result.insert(result.end(), topIndex.begin(), topIndex.end());
  result.insert(result.end(), stringIndex.begin(), stringIndex.end());
  result.insert(result.end(), globalIndex.begin(), globalIndex.end());
  result.insert(result.end(), privateDict.begin(), privateDict.end());
  if (!localSubrs.empty()) {
    result.insert(result.end(), localIndex.begin(), localIndex.end());
  }
  result.insert(result.end(), charStrings.begin(), charStrings.end());
  return result;
}

std::vector<uint8_t> MakeCff1WithCharset(std::vector<std::vector<uint8_t>> charStrings,
                                         std::vector<uint16_t> charsetSids,
                                         std::vector<std::vector<uint8_t>> localSubrs = {}) {
  EXPECT_EQ(charStrings.size(), charsetSids.size());
  EXPECT_FALSE(charStrings.empty());
  EXPECT_EQ(charsetSids.front(), 0u);
  const std::vector<uint8_t> nameIndex = MakeCffIndex({{'A'}}, false);
  const std::vector<uint8_t> stringIndex = MakeCffIndex({}, false);
  const std::vector<uint8_t> globalIndex = MakeCffIndex({}, false);
  const std::vector<uint8_t> localIndex = MakeCffIndex(localSubrs, false);
  const std::vector<uint8_t> encodedCharStrings = MakeCffIndex(charStrings, false);
  std::vector<uint8_t> charset{0};
  for (size_t glyph = 1; glyph < charsetSids.size(); ++glyph) {
    charset.push_back(static_cast<uint8_t>(charsetSids[glyph] >> 8));
    charset.push_back(static_cast<uint8_t>(charsetSids[glyph]));
  }

  std::vector<uint8_t> topDict;
  std::vector<uint8_t> topIndex;
  std::vector<uint8_t> privateDict;
  for (int iteration = 0; iteration < 8; ++iteration) {
    topIndex = MakeCffIndex({topDict}, false);
    const size_t charsetOffset =
        4 + nameIndex.size() + topIndex.size() + stringIndex.size() + globalIndex.size();
    privateDict.clear();
    if (!localSubrs.empty()) {
      privateDict = EncodeDictInteger(2);
      privateDict.push_back(19);
    }
    const size_t privateOffset = charsetOffset + charset.size();
    const size_t charStringsOffset =
        privateOffset + privateDict.size() + (localSubrs.empty() ? 0 : localIndex.size());

    std::vector<uint8_t> nextTop = EncodeDictInteger(charsetOffset);
    nextTop.push_back(15);
    const std::vector<uint8_t> encodedCharStringsOffset = EncodeDictInteger(charStringsOffset);
    nextTop.insert(nextTop.end(), encodedCharStringsOffset.begin(), encodedCharStringsOffset.end());
    nextTop.push_back(17);
    if (!privateDict.empty()) {
      const std::vector<uint8_t> privateSize = EncodeDictInteger(privateDict.size());
      const std::vector<uint8_t> encodedPrivateOffset = EncodeDictInteger(privateOffset);
      nextTop.insert(nextTop.end(), privateSize.begin(), privateSize.end());
      nextTop.insert(nextTop.end(), encodedPrivateOffset.begin(), encodedPrivateOffset.end());
      nextTop.push_back(18);
    }
    if (nextTop == topDict) break;
    topDict = std::move(nextTop);
  }
  topIndex = MakeCffIndex({topDict}, false);

  std::vector<uint8_t> result{1, 0, 4, 4};
  const auto append = [&](const std::vector<uint8_t>& section) {
    result.insert(result.end(), section.begin(), section.end());
  };
  append(nameIndex);
  append(topIndex);
  append(stringIndex);
  append(globalIndex);
  append(charset);
  append(privateDict);
  if (!localSubrs.empty()) result.insert(result.end(), localIndex.begin(), localIndex.end());
  result.insert(result.end(), encodedCharStrings.begin(), encodedCharStrings.end());
  return result;
}

std::vector<uint8_t> MakeCff1SeacChain(size_t depth, std::vector<uint8_t> leaf,
                                       std::vector<std::vector<uint8_t>> localSubrs = {}) {
  std::vector<std::vector<uint8_t>> charStrings{{14}, std::move(leaf)};
  std::vector<uint16_t> charsetSids{0, 34};
  for (size_t level = 1; level <= depth; ++level) {
    const uint8_t previousCode = static_cast<uint8_t>(64 + level);
    const uint8_t encodedPreviousCode = static_cast<uint8_t>(previousCode + 139);
    charStrings.push_back({139, 139, encodedPreviousCode, encodedPreviousCode, 14});
    charsetSids.push_back(static_cast<uint16_t>(34 + level));
  }
  return MakeCff1WithCharset(std::move(charStrings), std::move(charsetSids), std::move(localSubrs));
}

std::vector<uint8_t> MakeCff2WithCharString(std::vector<uint8_t> charString) {
  const std::vector<uint8_t> globalIndex = MakeCffIndex({}, true);
  const std::vector<uint8_t> charStrings = MakeCffIndex({std::move(charString)}, true);
  const std::vector<uint8_t> fdArray = MakeCffIndex({{139, 139, 18}}, true);

  std::vector<uint8_t> topDict;
  for (int iteration = 0; iteration < 8; ++iteration) {
    const size_t charStringsOffset = 5 + topDict.size() + globalIndex.size();
    const size_t fdArrayOffset = charStringsOffset + charStrings.size();
    std::vector<uint8_t> nextTop = EncodeDictInteger(charStringsOffset);
    nextTop.push_back(17);
    const std::vector<uint8_t> encodedFdArrayOffset = EncodeDictInteger(fdArrayOffset);
    nextTop.insert(nextTop.end(), encodedFdArrayOffset.begin(), encodedFdArrayOffset.end());
    nextTop.push_back(12);
    nextTop.push_back(36);
    if (nextTop == topDict) {
      break;
    }
    topDict = std::move(nextTop);
  }

  std::vector<uint8_t> result{2, 0, 5, 0, static_cast<uint8_t>(topDict.size())};
  result.insert(result.end(), topDict.begin(), topDict.end());
  result.insert(result.end(), globalIndex.begin(), globalIndex.end());
  result.insert(result.end(), charStrings.begin(), charStrings.end());
  result.insert(result.end(), fdArray.begin(), fdArray.end());
  return result;
}

std::vector<uint8_t> MakeCff1() {
  constexpr size_t kCharStringsOffset = 21;
  return {
      1,   0,   4,  4,              // Header.
      0,   1,   1,  1,   2,   'A',  // Name INDEX.
      0,   1,   1,  1,   3,   EncodeSmallInteger(kCharStringsOffset),
      17,                    // Top DICT INDEX.
      0,   0,                // String INDEX.
      0,   0,                // Global Subr INDEX.
      0,   1,   1,  1,   8,  // CharStrings INDEX.
      139, 139, 21, 149, 139, 5,
      14,  // Move, line, endchar.
  };
}

std::vector<uint8_t> MakeCff2(bool variableCharString = false) {
  constexpr size_t kCharStringsOffset = 14;
  constexpr size_t kFdArrayOffset = 27;
  std::vector<uint8_t> result = {
      2,
      0,
      5,
      0,
      5,  // Header.
      EncodeSmallInteger(kCharStringsOffset),
      17,  // CharStrings.
      EncodeSmallInteger(kFdArrayOffset),
      12,
      36,  // FDArray.
      0,
      0,
      0,
      0,  // Global Subr INDEX.
      0,
      0,
      0,
      1,
      1,
      1,
      7,  // CharStrings INDEX.
      139,
      139,
      21,
      149,
      139,
      5,  // Move and line.
      0,
      0,
      0,
      1,
      1,
      1,
      4,
      139,
      176,
      18,  // FDArray and empty Private.
  };
  if (variableCharString) {
    result[26] = 16;  // Replace rlineto with the unsupported CFF2 blend operator.
  }
  return result;
}

std::vector<uint8_t> MakeCffSfnt(std::string_view tag, std::vector<uint8_t> cff,
                                 size_t glyphCount = 1) {
  std::vector<uint8_t> maxp(6, 0);
  WriteBe16(&maxp, 4, static_cast<uint16_t>(glyphCount));
  return MakeSfnt({{tag, std::move(cff)}, {"maxp", std::move(maxp)}}, 0x4F54544F);
}

std::vector<uint8_t> CompoundGlyph(std::span<const uint16_t> dependencies) {
  std::vector<uint8_t> glyph(10, 0);
  WriteBe16(&glyph, 0, 0xFFFF);
  for (size_t i = 0; i < dependencies.size(); ++i) {
    const size_t offset = glyph.size();
    glyph.resize(offset + 6);
    const uint16_t flags =
        static_cast<uint16_t>(0x0002 | (i + 1 < dependencies.size() ? 0x0020 : 0));
    WriteBe16(&glyph, offset, flags);
    WriteBe16(&glyph, offset + 2, dependencies[i]);
  }
  return glyph;
}

std::vector<uint8_t> SimpleGlyph(size_t pointCount) {
  EXPECT_GT(pointCount, 0u);
  EXPECT_LE(pointCount, 65536u);
  std::vector<uint8_t> glyph(14, 0);
  WriteBe16(&glyph, 0, 1);
  WriteBe16(&glyph, 10, static_cast<uint16_t>(pointCount - 1));

  // All points are on-curve at (0, 0), encoded in bounded repeat runs with no coordinate bytes.
  size_t remaining = pointCount;
  while (remaining != 0) {
    const size_t runLength = std::min(remaining, size_t{256});
    glyph.push_back(runLength == 1 ? 0x31 : 0x39);
    if (runLength != 1) {
      glyph.push_back(static_cast<uint8_t>(runLength - 1));
    }
    remaining -= runLength;
  }
  return glyph;
}

std::vector<uint8_t> MakeTrueType(std::vector<std::vector<uint8_t>> glyphs) {
  std::vector<uint8_t> head(54, 0);
  WriteBe16(&head, 50, 1);
  std::vector<uint8_t> maxp(6, 0);
  WriteBe16(&maxp, 4, static_cast<uint16_t>(glyphs.size()));

  std::vector<uint8_t> loca((glyphs.size() + 1) * 4, 0);
  std::vector<uint8_t> glyf;
  for (size_t i = 0; i < glyphs.size(); ++i) {
    WriteBe32(&loca, i * 4, static_cast<uint32_t>(glyf.size()));
    glyf.insert(glyf.end(), glyphs[i].begin(), glyphs[i].end());
  }
  WriteBe32(&loca, glyphs.size() * 4, static_cast<uint32_t>(glyf.size()));
  return MakeSfnt({{"maxp", std::move(maxp)},
                   {"glyf", std::move(glyf)},
                   {"head", std::move(head)},
                   {"loca", std::move(loca)}});
}

std::vector<uint8_t> MakeDependencyChain(size_t depth) {
  std::vector<std::vector<uint8_t>> glyphs(depth, std::vector<uint8_t>(10, 0));
  for (size_t i = 0; i + 1 < depth; ++i) {
    const uint16_t child = static_cast<uint16_t>(i + 1);
    glyphs[i] = CompoundGlyph({&child, 1});
  }
  return MakeTrueType(std::move(glyphs));
}

std::vector<uint8_t> MakeSharedTail(bool appendTooDeepRoot) {
  const size_t tailDepth = kMaximumCompoundGlyphDepth - 1;
  std::vector<std::vector<uint8_t>> glyphs(tailDepth, std::vector<uint8_t>(10, 0));
  for (size_t i = 0; i + 1 < tailDepth; ++i) {
    const uint16_t child = static_cast<uint16_t>(i + 1);
    glyphs[i] = CompoundGlyph({&child, 1});
  }

  const uint16_t tail = 0;
  glyphs.push_back(CompoundGlyph({&tail, 1}));
  glyphs.push_back(CompoundGlyph({&tail, 1}));
  if (appendTooDeepRoot) {
    const uint16_t completedDepth32Root = static_cast<uint16_t>(tailDepth);
    glyphs.push_back(CompoundGlyph({&completedDepth32Root, 1}));
  }
  return MakeTrueType(std::move(glyphs));
}

uint64_t RepeatedComponentWork(size_t componentCount, size_t childPoints) {
  const uint64_t childVertices = childPoints + 2;
  const uint64_t childWork = childVertices + 4 * childPoints;
  const uint64_t components = componentCount;
  return 1 + components * (childWork + childVertices + 1) +
         childVertices * components * (components + 1) / 2;
}

TEST(SfntUtils, SortsDirectoryForBoundedLookup) {
  const std::vector<uint8_t> data = MakeSfnt({{"name", {1}}, {"head", {2, 3}}});
  auto font = SfntFont::Validate(data);
  ASSERT_TRUE(font.has_value());
  EXPECT_EQ(font->numTables(), 2u);
  EXPECT_THAT(font->findTable(data, "head"),
              testing::Optional(testing::ElementsAre(uint8_t{2}, uint8_t{3})));
  EXPECT_EQ(font->findTable(data, "cmap"), std::nullopt);
}

TEST(SfntUtils, RetainsPerGlyphComplexityIndependentOfSourceBytes) {
  std::vector<uint8_t> data = MakeTrueType({SimpleGlyph(3)});
  auto font = SfntFont::Validate(data);
  ASSERT_TRUE(font.has_value());
  data.clear();

  const auto complexity = font->glyphOutlineComplexity(0);
  ASSERT_TRUE(complexity.has_value());
  EXPECT_EQ(complexity->maximumVertices, 5u);
  EXPECT_EQ(complexity->work, 17u);
  EXPECT_FALSE(font->glyphOutlineComplexity(1).has_value());
}

TEST(SfntUtils, RejectsTableCountAboveExplicitCap) {
  std::vector<uint8_t> data(12 + (kMaximumSfntTables + 1) * 16, 0);
  WriteBe32(&data, 0, 0x00010000);
  WriteBe16(&data, 4, static_cast<uint16_t>(kMaximumSfntTables + 1));
  EXPECT_FALSE(SfntFont::Validate(data).has_value());
}

TEST(SfntUtils, RejectsDuplicateAndOutOfBoundsTables) {
  EXPECT_FALSE(SfntFont::Validate(MakeSfnt({{"name", {}}, {"name", {}}})).has_value());

  std::vector<uint8_t> outOfBounds = MakeSfnt({{"name", {1}}});
  WriteBe32(&outOfBounds, 20, 0xFFFFFFF0);
  WriteBe32(&outOfBounds, 24, 0x40);
  EXPECT_FALSE(SfntFont::Validate(outOfBounds).has_value());
}

TEST(SfntUtils, RejectsSelfAndTwoNodeCompoundCycles) {
  const uint16_t self = 0;
  EXPECT_FALSE(SfntFont::Validate(MakeTrueType({CompoundGlyph({&self, 1})})).has_value());

  const uint16_t one = 1;
  const uint16_t zero = 0;
  EXPECT_FALSE(
      SfntFont::Validate(MakeTrueType({CompoundGlyph({&one, 1}), CompoundGlyph({&zero, 1})}))
          .has_value());
}

TEST(SfntUtils, AcceptsDepth32AndRejectsDepth33) {
  EXPECT_TRUE(SfntFont::Validate(MakeDependencyChain(kMaximumCompoundGlyphDepth)).has_value());
  EXPECT_FALSE(SfntFont::Validate(MakeDependencyChain(kMaximumCompoundGlyphDepth + 1)).has_value());
}

TEST(SfntUtils, IncludesCompletedSharedTailInMemoizedDepth) {
  EXPECT_TRUE(SfntFont::Validate(MakeSharedTail(false)).has_value());
  EXPECT_FALSE(SfntFont::Validate(MakeSharedTail(true)).has_value());
}

TEST(SfntUtils, RejectsComponentRecordWorkAboveCap) {
  std::vector<uint16_t> dependencies(kMaximumCompoundComponentRecords, 1);
  EXPECT_TRUE(
      SfntFont::Validate(MakeTrueType({CompoundGlyph(dependencies), std::vector<uint8_t>(10, 0)}))
          .has_value());

  dependencies.push_back(1);
  EXPECT_FALSE(
      SfntFont::Validate(MakeTrueType({CompoundGlyph(dependencies), std::vector<uint8_t>(10, 0)}))
          .has_value());
}

TEST(SfntUtils, CapsRepeatedMultipointDecodeAndPrefixCopyWork) {
  constexpr size_t kChildPoints = 64;
  size_t acceptedComponents = 0;
  while (acceptedComponents < kMaximumCompoundComponentRecords &&
         RepeatedComponentWork(acceptedComponents + 1, kChildPoints) <= kMaximumGlyphOutlineWork) {
    ++acceptedComponents;
  }
  ASSERT_GT(acceptedComponents, 0u);
  ASSERT_LT(acceptedComponents, kMaximumCompoundComponentRecords);

  std::vector<uint16_t> dependencies(acceptedComponents, 1);
  EXPECT_TRUE(
      SfntFont::Validate(MakeTrueType({CompoundGlyph(dependencies), SimpleGlyph(kChildPoints)}))
          .has_value());

  dependencies.push_back(1);
  EXPECT_FALSE(
      SfntFont::Validate(MakeTrueType({CompoundGlyph(dependencies), SimpleGlyph(kChildPoints)}))
          .has_value());
}

TEST(SfntUtils, CapsNestedSharedOutlineExpansion) {
  constexpr size_t kLeafPoints = 64;
  std::vector<std::vector<uint8_t>> glyphs;
  glyphs.push_back(SimpleGlyph(kLeafPoints));

  uint64_t vertices = kLeafPoints + 2;
  uint64_t work = vertices + 4 * kLeafPoints;
  while (glyphs.size() < kMaximumCompoundGlyphDepth) {
    const uint64_t nextVertices = vertices * 2;
    const uint64_t nextWork = 2 * work + 5 * vertices + 3;
    if (nextVertices > kMaximumExpandedGlyphVertices || nextWork > kMaximumGlyphOutlineWork) {
      break;
    }
    const uint16_t sharedChild = static_cast<uint16_t>(glyphs.size() - 1);
    const std::array<uint16_t, 2> dependencies{sharedChild, sharedChild};
    glyphs.push_back(CompoundGlyph(dependencies));
    vertices = nextVertices;
    work = nextWork;
  }
  ASSERT_GT(glyphs.size(), 1u);
  EXPECT_TRUE(SfntFont::Validate(MakeTrueType(glyphs)).has_value());

  const uint16_t sharedChild = static_cast<uint16_t>(glyphs.size() - 1);
  const std::array<uint16_t, 2> dependencies{sharedChild, sharedChild};
  glyphs.push_back(CompoundGlyph(dependencies));
  EXPECT_FALSE(SfntFont::Validate(MakeTrueType(std::move(glyphs))).has_value());
}

TEST(SfntUtils, RejectsMalformedCompoundRecords) {
  std::vector<uint8_t> truncated(12, 0);
  WriteBe16(&truncated, 0, 0xFFFF);
  EXPECT_FALSE(SfntFont::Validate(MakeTrueType({truncated})).has_value());

  const uint16_t child = 1;
  std::vector<uint8_t> pointMatching = CompoundGlyph({&child, 1});
  WriteBe16(&pointMatching, 10, 0);
  EXPECT_FALSE(
      SfntFont::Validate(MakeTrueType({pointMatching, std::vector<uint8_t>(10, 0)})).has_value());

  std::vector<uint8_t> conflictingTransforms = CompoundGlyph({&child, 1});
  WriteBe16(&conflictingTransforms, 10, 0x0002 | 0x0008 | 0x0040);
  EXPECT_FALSE(
      SfntFont::Validate(MakeTrueType({conflictingTransforms, std::vector<uint8_t>(10, 0)}))
          .has_value());

  const uint16_t outOfRange = 2;
  EXPECT_FALSE(SfntFont::Validate(
                   MakeTrueType({CompoundGlyph({&outOfRange, 1}), std::vector<uint8_t>(10, 0)}))
                   .has_value());

  std::vector<uint8_t> reservedFlag = CompoundGlyph({&child, 1});
  WriteBe16(&reservedFlag, 10, 0x0002 | 0x0010);
  EXPECT_FALSE(
      SfntFont::Validate(MakeTrueType({reservedFlag, std::vector<uint8_t>(10, 0)})).has_value());
}

TEST(SfntUtils, RejectsNonMonotonicLoca) {
  std::vector<uint8_t> reversedLoca = MakeTrueType({std::vector<uint8_t>(10, 0)});
  auto indexed = SfntFont::Validate(reversedLoca);
  ASSERT_TRUE(indexed.has_value());
  const auto loca = indexed->findTable(reversedLoca, "loca");
  ASSERT_TRUE(loca.has_value());
  const size_t locaOffset = static_cast<size_t>(loca->data() - reversedLoca.data());
  WriteBe32(&reversedLoca, locaOffset, 12);
  WriteBe32(&reversedLoca, locaOffset + 4, 10);
  EXPECT_FALSE(SfntFont::Validate(reversedLoca).has_value());
}

TEST(SfntUtils, RejectsMalformedSimpleGlyphRecords) {
  std::vector<uint8_t> missingPointFlags(14, 0);
  WriteBe16(&missingPointFlags, 0, 1);
  WriteBe16(&missingPointFlags, 10, 0);
  WriteBe16(&missingPointFlags, 12, 0);
  EXPECT_FALSE(SfntFont::Validate(MakeTrueType({missingPointFlags})).has_value());
}

TEST(SfntUtils, CffUsesBoundedDirectoryWithoutTrueTypeDependencyIndex) {
  const std::vector<uint8_t> data = MakeSfnt({{"CFF ", {0}}});
  auto font = SfntFont::Validate(data);
  ASSERT_TRUE(font.has_value());
  EXPECT_EQ(font->numGlyphs(), 0u);
  EXPECT_EQ(font->retainedBytes(), sizeof(SfntFont::TableRecord));
}

TEST(SfntUtils, RetainsCff1PerGlyphComplexity) {
  const std::vector<uint8_t> data = MakeCffSfnt("CFF ", MakeCff1());
  const auto font = SfntFont::Validate(data);
  ASSERT_TRUE(font.has_value());

  const auto complexity = font->glyphOutlineComplexity(0);
  ASSERT_TRUE(complexity.has_value());
  EXPECT_EQ(complexity->maximumVertices, 3u);
  EXPECT_GT(complexity->work, 0u);
}

TEST(SfntUtils, RetainsNonVariableCff2ComplexityAndFailsClosedForBlend) {
  const std::vector<uint8_t> staticData = MakeCffSfnt("CFF2", MakeCff2());
  const auto staticFont = SfntFont::Validate(staticData);
  ASSERT_TRUE(staticFont.has_value());
  EXPECT_TRUE(staticFont->glyphOutlineComplexity(0).has_value());

  const std::vector<uint8_t> variableData = MakeCffSfnt("CFF2", MakeCff2(true));
  const auto variableFont = SfntFont::Validate(variableData);
  ASSERT_TRUE(variableFont.has_value());
  EXPECT_FALSE(variableFont->glyphOutlineComplexity(0).has_value());
}

TEST(SfntUtils, CffLocalSubroutineEndcharTerminatesEveryCallFrame) {
  const std::vector<uint8_t> cff = MakeCff1WithSubrs({32, 10}, {{139, 139, 21, 149, 139, 5, 14}});
  const CffOutlineValidationResult result = ValidateCffOutlineComplexities(cff, false, 1);

  ASSERT_EQ(result.status, CffOutlineValidationStatus::Complete);
  ASSERT_EQ(result.glyphs.size(), 1u);
  EXPECT_EQ(result.glyphs.front().maximumVertices, 3u);
}

TEST(SfntUtils, CffReportsActualWorkAndStopsAtCallerBudget) {
  const std::vector<uint8_t> cff = MakeCff1();
  const CffOutlineValidationResult complete = ValidateCffOutlineComplexities(cff, false, 1);
  ASSERT_EQ(complete.status, CffOutlineValidationStatus::Complete);
  EXPECT_GT(complete.work, 1u);

  const CffOutlineValidationResult limited = ValidateCffOutlineComplexities(cff, false, 1, 1);
  EXPECT_EQ(limited.status, CffOutlineValidationStatus::WorkLimitExceeded);
  EXPECT_EQ(limited.work, 1u);
  EXPECT_THAT(limited.glyphs, testing::IsEmpty());
}

TEST(SfntUtils, Cff1LegacyEndcharCompositeAggregatesRenderableComponents) {
  const std::vector<uint8_t> base{139, 139, 21, 149, 139, 5, 14};
  const std::vector<uint8_t> accent{139, 139, 21, 139, 149, 5, 14};
  const std::vector<uint8_t> cff =
      MakeCff1WithCharset({{14}, base, accent, {139, 139, 204, 247, 86, 14}}, {0, 34, 125, 150});
  const CffOutlineValidationResult result = ValidateCffOutlineComplexities(cff, false, 4);

  ASSERT_EQ(result.status, CffOutlineValidationStatus::Complete);
  ASSERT_EQ(result.glyphs.size(), 4u);
  EXPECT_EQ(result.glyphs[3].maximumVertices,
            result.glyphs[1].maximumVertices + result.glyphs[2].maximumVertices);
  EXPECT_GT(result.glyphs[3].work, result.glyphs[1].work + result.glyphs[2].work);

  const auto font = SfntFont::Validate(MakeCffSfnt("CFF ", cff, 4));
  ASSERT_TRUE(font.has_value());
  EXPECT_EQ(font->numGlyphs(), 4u);
  EXPECT_TRUE(font->glyphOutlineComplexity(3).has_value());
}

TEST(SfntUtils, Cff1SeacRejectsMissingComponentsAndCycles) {
  const std::vector<uint8_t> missing =
      MakeCff1WithCharset({{14}, {14}, {14}, {139, 139, 204, 247, 86, 14}}, {0, 34, 126, 150});
  EXPECT_EQ(ValidateCffOutlineComplexities(missing, false, 4).status,
            CffOutlineValidationStatus::Invalid);

  const std::vector<uint8_t> cycle = MakeCff1WithCharset({{14}, {139, 139, 204, 139, 14}}, {0, 34});
  EXPECT_EQ(ValidateCffOutlineComplexities(cycle, false, 2).status,
            CffOutlineValidationStatus::Invalid);
}

TEST(SfntUtils, Cff1SeacEnforcesComponentDepthPointAndWorkCaps) {
  const std::vector<uint8_t> leaf{139, 139, 21, 149, 139, 5, 14};
  const CffOutlineValidationResult maximumDepth =
      ValidateCffOutlineComplexities(MakeCff1SeacChain(10, leaf), false, 12);
  ASSERT_EQ(maximumDepth.status, CffOutlineValidationStatus::Complete);
  const CffOutlineValidationResult excessiveDepth =
      ValidateCffOutlineComplexities(MakeCff1SeacChain(11, leaf), false, 13);
  EXPECT_EQ(excessiveDepth.status, CffOutlineValidationStatus::Invalid);
  EXPECT_EQ(excessiveDepth.componentResolutionWork, maximumDepth.componentResolutionWork);

  std::vector<uint8_t> pointLeaf{139, 139, 21};
  for (size_t batch = 0; batch < 100; ++batch) {
    pointLeaf.insert(pointLeaf.end(), 48, 139);
    pointLeaf.push_back(6);
  }
  pointLeaf.push_back(14);
  EXPECT_EQ(
      ValidateCffOutlineComplexities(MakeCff1SeacChain(8, std::move(pointLeaf)), false, 10).status,
      CffOutlineValidationStatus::Invalid);

  std::vector<uint8_t> expensiveSubr;
  for (size_t operation = 0; operation < 5000; ++operation) {
    expensiveSubr.insert(expensiveSubr.end(), {139, 12, 18});
  }
  expensiveSubr.push_back(11);
  std::vector<uint8_t> workLeaf{139, 139, 21};
  for (size_t call = 0; call < 400; ++call) {
    workLeaf.insert(workLeaf.end(), {32, 10});
  }
  workLeaf.push_back(14);
  EXPECT_EQ(ValidateCffOutlineComplexities(
                MakeCff1SeacChain(2, std::move(workLeaf), {std::move(expensiveSubr)}), false, 4)
                .status,
            CffOutlineValidationStatus::Invalid);
}

TEST(SfntUtils, CffSubroutinesShareOperandsAndHintState) {
  const std::vector<uint8_t> cff =
      MakeCff1WithSubrs({32, 10, 19, 0x80, 33, 10, 21, 149, 139, 5, 14}, {
                                                                             {139, 149, 1, 11},
                                                                             {139, 139, 11},
                                                                         });
  const CffOutlineValidationResult result = ValidateCffOutlineComplexities(cff, false, 1);

  ASSERT_EQ(result.status, CffOutlineValidationStatus::Complete);
  ASSERT_EQ(result.glyphs.size(), 1u);
  EXPECT_EQ(result.glyphs.front().maximumVertices, 3u);
}

TEST(SfntUtils, CffRejectsTruncatedHintMasksAndRecursiveSubroutines) {
  const std::vector<uint8_t> truncatedMask = MakeCff1WithSubrs({32, 10, 19}, {{139, 149, 1, 11}});
  EXPECT_EQ(ValidateCffOutlineComplexities(truncatedMask, false, 1).status,
            CffOutlineValidationStatus::Invalid);

  const std::vector<uint8_t> recursive = MakeCff1WithSubrs({32, 10}, {{32, 10, 11}});
  EXPECT_EQ(ValidateCffOutlineComplexities(recursive, false, 1).status,
            CffOutlineValidationStatus::Invalid);
}

TEST(SfntUtils, CffAcceptsTenSubroutineFramesAndRejectsEleven) {
  const auto makeDepth = [](size_t depth) {
    std::vector<std::vector<uint8_t>> subrs(depth);
    for (size_t index = 0; index + 1 < depth; ++index) {
      subrs[index] = {static_cast<uint8_t>(33 + index), 10};
    }
    subrs.back() = {139, 139, 21, 149, 139, 5, 14};
    return MakeCff1WithSubrs({32, 10}, std::move(subrs));
  };

  EXPECT_EQ(ValidateCffOutlineComplexities(makeDepth(10), false, 1).status,
            CffOutlineValidationStatus::Complete);
  EXPECT_EQ(ValidateCffOutlineComplexities(makeDepth(11), false, 1).status,
            CffOutlineValidationStatus::Invalid);
}

TEST(SfntUtils, CffEnforcesVersionSpecificOperandStackCaps) {
  const auto makeLines = [](size_t operands, bool cff2) {
    std::vector<uint8_t> charString{139, 139, 21};
    charString.insert(charString.end(), operands, 139);
    charString.push_back(6);
    if (!cff2) {
      charString.push_back(14);
    }
    return charString;
  };

  EXPECT_EQ(
      ValidateCffOutlineComplexities(MakeCff1WithSubrs(makeLines(48, false), {}), false, 1).status,
      CffOutlineValidationStatus::Complete);
  EXPECT_EQ(
      ValidateCffOutlineComplexities(MakeCff1WithSubrs(makeLines(49, false), {}), false, 1).status,
      CffOutlineValidationStatus::Invalid);
  EXPECT_EQ(
      ValidateCffOutlineComplexities(MakeCff2WithCharString(makeLines(513, true)), true, 1).status,
      CffOutlineValidationStatus::Complete);
  EXPECT_EQ(
      ValidateCffOutlineComplexities(MakeCff2WithCharString(makeLines(514, true)), true, 1).status,
      CffOutlineValidationStatus::Invalid);
}

TEST(SfntUtils, CffEnforcesExpandedPointAndExecutionWorkCaps) {
  std::vector<uint8_t> lineSubr(48, 139);
  lineSubr.push_back(6);
  lineSubr.push_back(11);
  const size_t acceptedCalls = (kMaximumExpandedGlyphVertices - 2) / 48;
  const auto repeatedCalls = [](size_t count) {
    std::vector<uint8_t> charString{139, 139, 21};
    for (size_t call = 0; call < count; ++call) {
      charString.push_back(32);
      charString.push_back(10);
    }
    charString.push_back(14);
    return charString;
  };
  EXPECT_EQ(ValidateCffOutlineComplexities(
                MakeCff1WithSubrs(repeatedCalls(acceptedCalls), {lineSubr}), false, 1)
                .status,
            CffOutlineValidationStatus::Complete);
  EXPECT_EQ(ValidateCffOutlineComplexities(
                MakeCff1WithSubrs(repeatedCalls(acceptedCalls + 1), {lineSubr}), false, 1)
                .status,
            CffOutlineValidationStatus::Invalid);

  std::vector<uint8_t> expensiveSubr;
  expensiveSubr.reserve(65535);
  for (size_t operation = 0; operation < 21844; ++operation) {
    expensiveSubr.insert(expensiveSubr.end(), {139, 12, 18});
  }
  expensiveSubr.push_back(11);
  EXPECT_EQ(ValidateCffOutlineComplexities(MakeCff1WithSubrs(repeatedCalls(256), {expensiveSubr}),
                                           false, 1)
                .status,
            CffOutlineValidationStatus::Complete);
  EXPECT_EQ(ValidateCffOutlineComplexities(
                MakeCff1WithSubrs(repeatedCalls(257), {std::move(expensiveSubr)}), false, 1)
                .status,
            CffOutlineValidationStatus::Invalid);
}

TEST(SfntUtils, CffCharStringCountMustMatchMaxp) {
  std::vector<uint8_t> maxp(6, 0);
  WriteBe16(&maxp, 4, 2);
  const std::vector<uint8_t> data =
      MakeSfnt({{"CFF ", MakeCff1()}, {"maxp", std::move(maxp)}}, 0x4F54544F);
  const auto font = SfntFont::Validate(data);

  ASSERT_TRUE(font.has_value());
  EXPECT_EQ(font->numGlyphs(), 0u);
  EXPECT_FALSE(font->glyphOutlineComplexity(0).has_value());
}

TEST(SfntUtils, VariableCff2AndCompetingOutlineTablesFailClosed) {
  std::vector<uint8_t> maxp(6, 0);
  WriteBe16(&maxp, 4, 1);
  const std::vector<uint8_t> variableCff2 =
      MakeSfnt({{"CFF2", MakeCff2()}, {"fvar", {}}, {"maxp", maxp}}, 0x4F54544F);
  const auto variableFont = SfntFont::Validate(variableCff2);
  ASSERT_TRUE(variableFont.has_value());
  EXPECT_FALSE(variableFont->glyphOutlineComplexity(0).has_value());

  std::vector<uint8_t> head(54, 0);
  WriteBe16(&head, 50, 1);
  const std::vector<uint8_t> competing = MakeSfnt({{"CFF ", MakeCff1()},
                                                   {"glyf", {}},
                                                   {"head", std::move(head)},
                                                   {"loca", std::vector<uint8_t>(8, 0)},
                                                   {"maxp", std::move(maxp)}},
                                                  0x4F54544F);
  const auto competingFont = SfntFont::Validate(competing);
  ASSERT_TRUE(competingFont.has_value());
  EXPECT_FALSE(competingFont->glyphOutlineComplexity(0).has_value());
}

TEST(SfntUtils, ReportsExactRetainedIndexBytes) {
  const std::vector<uint8_t> data = MakeTrueType({std::vector<uint8_t>(10, 0), {}});
  auto font = SfntFont::Validate(data);
  ASSERT_TRUE(font.has_value());
  EXPECT_EQ(font->retainedBytes(), 4 * sizeof(SfntFont::TableRecord) + 3 * sizeof(uint32_t) +
                                       2 * sizeof(SfntFont::GlyphOutlineComplexity));
}

}  // namespace
}  // namespace donner::fonts
