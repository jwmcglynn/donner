/// @file
/// Unit tests for the glyph-identity cache's key semantics and eviction
/// policy. No GPU device is created: every entry here carries an empty
/// resident slot, which is exactly the part of the cache that is pure
/// bookkeeping.

#include "donner/svg/renderer/geode/GeodeGlyphResidency.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/Path.h"

namespace donner::geode {

namespace {

GlyphGeometryKey MakeKey(uint32_t glyphIndex, float outlineScale = 0.25f) {
  GlyphGeometryKey key;
  key.fontId = 7u;
  key.glyphIndex = glyphIndex;
  key.outlineScale = outlineScale;
  return key;
}

/// An encode whose accounted size is `curveCount * sizeof(Curve)`, so a test
/// can hand the budget a known number of bytes without going through the
/// encoder.
EncodedPath MakeEncodeWithCurves(size_t curveCount) {
  EncodedPath encoded;
  encoded.curves.resize(curveCount);
  return encoded;
}

/// Insert `key` and immediately mark it used in `frame`, which is what the
/// renderer does for every occurrence it draws.
GeodeGlyphResidentEntry* InsertUsed(GeodeGlyphCache& cache, const GlyphGeometryKey& key,
                                    size_t curveCount, uint64_t frame) {
  GeodeGlyphResidentEntry* entry = cache.insert(key, Path(), MakeEncodeWithCurves(curveCount));
  entry->lastUsedFrame = frame;
  return entry;
}

}  // namespace

TEST(GeodeGlyphCacheTest, DistinctGlyphIdentitiesTakeDistinctEntries) {
  GeodeGlyphCache cache(/*deviceId=*/1u);

  GlyphGeometryKey a = MakeKey(/*glyphIndex=*/10);
  GlyphGeometryKey b = MakeKey(/*glyphIndex=*/11);
  GlyphGeometryKey scaled = MakeKey(/*glyphIndex=*/10, /*outlineScale=*/0.5f);
  GlyphGeometryKey stretched = MakeKey(/*glyphIndex=*/10);
  stretched.stretchScaleX = 2.0f;
  GlyphGeometryKey otherFont = MakeKey(/*glyphIndex=*/10);
  otherFont.fontId = 8u;

  InsertUsed(cache, a, /*curveCount=*/1, /*frame=*/1);
  InsertUsed(cache, b, /*curveCount=*/1, /*frame=*/1);
  InsertUsed(cache, scaled, /*curveCount=*/1, /*frame=*/1);
  InsertUsed(cache, stretched, /*curveCount=*/1, /*frame=*/1);
  InsertUsed(cache, otherFont, /*curveCount=*/1, /*frame=*/1);

  EXPECT_EQ(cache.size(), 5u);
  EXPECT_NE(cache.find(a), nullptr);
  EXPECT_NE(cache.find(scaled), nullptr);
  EXPECT_NE(cache.find(a), cache.find(scaled));
  EXPECT_NE(cache.find(a), cache.find(stretched));
  EXPECT_NE(cache.find(a), cache.find(otherFont));
}

TEST(GeodeGlyphCacheTest, RepeatedLookupOfOneIdentityReusesTheSameEntry) {
  GeodeGlyphCache cache(/*deviceId=*/1u);
  const GlyphGeometryKey key = MakeKey(/*glyphIndex=*/42);

  GeodeGlyphResidentEntry* first = InsertUsed(cache, key, /*curveCount=*/3, /*frame=*/1);
  ASSERT_NE(first, nullptr);

  EXPECT_EQ(cache.find(key), first);
  EXPECT_EQ(cache.find(MakeKey(/*glyphIndex=*/42)), first);
  EXPECT_EQ(cache.size(), 1u);
}

TEST(GeodeGlyphCacheTest, ScalesThatDifferInTheLastBitDoNotShareAnEntry) {
  GeodeGlyphCache cache(/*deviceId=*/1u);
  const float scale = 0.25f;
  const float nudged = std::nextafter(scale, 1.0f);

  InsertUsed(cache, MakeKey(/*glyphIndex=*/5, scale), /*curveCount=*/1, /*frame=*/1);
  EXPECT_EQ(cache.find(MakeKey(/*glyphIndex=*/5, nudged)), nullptr);
}

TEST(GeodeGlyphCacheTest, MissingEntryReportsAMiss) {
  GeodeGlyphCache cache(/*deviceId=*/1u);
  EXPECT_EQ(cache.find(MakeKey(/*glyphIndex=*/1)), nullptr);
  EXPECT_EQ(cache.size(), 0u);
}

TEST(GeodeGlyphCacheTest, EntryCountBudgetDropsTheLeastRecentlyUsedFirst) {
  GeodeGlyphCache cache(/*deviceId=*/1u);
  const GlyphGeometryKey oldest = MakeKey(/*glyphIndex=*/1);
  const GlyphGeometryKey middle = MakeKey(/*glyphIndex=*/2);
  const GlyphGeometryKey newest = MakeKey(/*glyphIndex=*/3);
  InsertUsed(cache, oldest, /*curveCount=*/1, /*frame=*/1);
  InsertUsed(cache, middle, /*curveCount=*/1, /*frame=*/2);
  InsertUsed(cache, newest, /*curveCount=*/1, /*frame=*/3);

  EXPECT_EQ(cache.evictToBudget(/*currentFrame=*/4, /*maxEntries=*/2,
                                /*maxEncodedBytes=*/1u << 20),
            1u);
  EXPECT_EQ(cache.size(), 2u);
  EXPECT_EQ(cache.find(oldest), nullptr);
  EXPECT_NE(cache.find(middle), nullptr);
  EXPECT_NE(cache.find(newest), nullptr);
}

TEST(GeodeGlyphCacheTest, ByteBudgetDropsEntriesUntilItFits) {
  GeodeGlyphCache cache(/*deviceId=*/1u);
  const uint64_t curveBytes = 6u * sizeof(float);
  InsertUsed(cache, MakeKey(/*glyphIndex=*/1), /*curveCount=*/4, /*frame=*/1);
  InsertUsed(cache, MakeKey(/*glyphIndex=*/2), /*curveCount=*/4, /*frame=*/2);
  InsertUsed(cache, MakeKey(/*glyphIndex=*/3), /*curveCount=*/4, /*frame=*/3);
  ASSERT_EQ(cache.encodedBytes(), 12u * curveBytes);

  EXPECT_EQ(cache.evictToBudget(/*currentFrame=*/4, /*maxEntries=*/100,
                                /*maxEncodedBytes=*/5u * curveBytes),
            2u);
  EXPECT_EQ(cache.size(), 1u);
  EXPECT_EQ(cache.encodedBytes(), 4u * curveBytes);
  EXPECT_NE(cache.find(MakeKey(/*glyphIndex=*/3)), nullptr);
}

TEST(GeodeGlyphCacheTest, EntriesUsedThisFrameSurviveAnOverBudgetTrim) {
  // The frame that is drawing right now has recorded draws reading these
  // entries' geometry, and freeing a slab range mid-frame would hand it to a
  // later allocation in the same frame.
  GeodeGlyphCache cache(/*deviceId=*/1u);
  InsertUsed(cache, MakeKey(/*glyphIndex=*/1), /*curveCount=*/1, /*frame=*/9);
  InsertUsed(cache, MakeKey(/*glyphIndex=*/2), /*curveCount=*/1, /*frame=*/9);
  InsertUsed(cache, MakeKey(/*glyphIndex=*/3), /*curveCount=*/1, /*frame=*/9);

  EXPECT_EQ(cache.evictToBudget(/*currentFrame=*/9, /*maxEntries=*/1,
                                /*maxEncodedBytes=*/1u << 20),
            0u);
  EXPECT_EQ(cache.size(), 3u);
}

TEST(GeodeGlyphCacheTest, TrimIsSkippedWhenTheCacheAlreadyFits) {
  GeodeGlyphCache cache(/*deviceId=*/1u);
  InsertUsed(cache, MakeKey(/*glyphIndex=*/1), /*curveCount=*/1, /*frame=*/1);

  EXPECT_EQ(cache.evictToBudget(/*currentFrame=*/2, /*maxEntries=*/4,
                                /*maxEncodedBytes=*/1u << 20),
            0u);
  EXPECT_EQ(cache.size(), 1u);
}

TEST(GeodeGlyphCacheTest, BeginFrameTrimsAtMostOncePerFrame) {
  GeodeGlyphCache cache(/*deviceId=*/1u);
  InsertUsed(cache, MakeKey(/*glyphIndex=*/1), /*curveCount=*/1, /*frame=*/1);
  InsertUsed(cache, MakeKey(/*glyphIndex=*/2), /*curveCount=*/1, /*frame=*/1);
  InsertUsed(cache, MakeKey(/*glyphIndex=*/3), /*curveCount=*/1, /*frame=*/1);

  EXPECT_EQ(cache.beginFrame(/*frameIndex=*/2, /*maxEntries=*/2, /*maxEncodedBytes=*/1u << 20), 1u);
  // A second touch of the same frame must not trim again, even though the
  // renderer calls the accessor once per glyph occurrence.
  EXPECT_EQ(cache.beginFrame(/*frameIndex=*/2, /*maxEntries=*/1, /*maxEncodedBytes=*/1u << 20), 0u);
  EXPECT_EQ(cache.size(), 2u);

  EXPECT_EQ(cache.beginFrame(/*frameIndex=*/3, /*maxEntries=*/1, /*maxEncodedBytes=*/1u << 20), 1u);
  EXPECT_EQ(cache.size(), 1u);
}

TEST(GeodeTextInstanceRecordComponentTest, OccurrenceAddressesSurviveGrowth) {
  // A pending batch holds pointers into these entries while later occurrences
  // are still appending; a reallocating container would leave the batch
  // writing through dangling pointers at flush.
  GeodeTextInstanceRecordComponent component;
  component.occurrences.push_back(GeodeTextInstanceRecordComponent::Occurrence{});
  const GeodeTextInstanceRecordComponent::Occurrence* first = &component.occurrences.front();

  for (int i = 0; i < 512; ++i) {
    component.occurrences.push_back(GeodeTextInstanceRecordComponent::Occurrence{});
  }

  EXPECT_EQ(&component.occurrences.front(), first);
}

TEST(GeodeTextInstanceRecordComponentTest, MoveLeavesTheSourceWithNoSlotsToRelease) {
  // entt's swap-and-pop removal move-assigns the surviving component over the
  // removed one; a source that kept its slots would free the survivor's
  // records from its own destructor.
  GeodeTextInstanceRecordComponent source;
  source.occurrences.push_back(GeodeTextInstanceRecordComponent::Occurrence{});
  source.lastFrame = 5u;

  GeodeTextInstanceRecordComponent moved = std::move(source);
  EXPECT_EQ(moved.occurrences.size(), 1u);
  EXPECT_EQ(moved.lastFrame, 5u);
  EXPECT_TRUE(source.occurrences.empty());

  GeodeTextInstanceRecordComponent assigned;
  assigned = std::move(moved);
  EXPECT_EQ(assigned.occurrences.size(), 1u);
  EXPECT_TRUE(moved.occurrences.empty());
}

}  // namespace donner::geode
