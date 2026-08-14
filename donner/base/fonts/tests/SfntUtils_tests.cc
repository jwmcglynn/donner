#include "donner/base/fonts/SfntUtils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

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

std::vector<uint8_t> MakeSfnt(std::vector<TableSpec> tables) {
  const size_t directorySize = 12 + tables.size() * 16;
  std::vector<uint8_t> result(directorySize, 0);
  WriteBe32(&result, 0, 0x00010000);
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

TEST(SfntUtils, ReportsExactRetainedIndexBytes) {
  const std::vector<uint8_t> data = MakeTrueType({std::vector<uint8_t>(10, 0), {}});
  auto font = SfntFont::Validate(data);
  ASSERT_TRUE(font.has_value());
  EXPECT_EQ(font->retainedBytes(), 4 * sizeof(SfntFont::TableRecord) + 3 * sizeof(uint32_t));
}

}  // namespace
}  // namespace donner::fonts
