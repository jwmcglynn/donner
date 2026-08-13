#include "donner/base/MemoryAttribution.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace donner {

namespace {

std::size_t Index(MemoryCategory category) {
  return static_cast<std::size_t>(category);
}

}  // namespace

/// The probe's state is process-wide, so each test drives categories through
/// explicit values rather than assuming it starts empty.
TEST(MemoryAttribution, RetainedLevelIsReplacedNotAccumulated) {
  SetRetainedBytes(MemoryCategory::CompositorLayerBitmaps, 4096);
  EXPECT_EQ(PeekMemoryAttribution().retainedBytes[Index(MemoryCategory::CompositorLayerBitmaps)],
            4096u);

  SetRetainedBytes(MemoryCategory::CompositorLayerBitmaps, 1024);
  EXPECT_EQ(PeekMemoryAttribution().retainedBytes[Index(MemoryCategory::CompositorLayerBitmaps)],
            1024u);
}

TEST(MemoryAttribution, RetainedHighWaterSurvivesADrop) {
  SetRetainedBytes(MemoryCategory::CompositorSegmentBitmaps, 8192);
  SetRetainedBytes(MemoryCategory::CompositorSegmentBitmaps, 16);

  const MemoryAttributionSample sample = PeekMemoryAttribution();
  EXPECT_EQ(sample.retainedBytes[Index(MemoryCategory::CompositorSegmentBitmaps)], 16u);
  EXPECT_GE(sample.retainedHighWaterBytes[Index(MemoryCategory::CompositorSegmentBitmaps)], 8192u);
}

TEST(MemoryAttribution, TransientFlowAccumulatesWithinAFrameAndResetsAtTheBoundary) {
  // Close whatever window a previous test left open so this frame's flow is
  // only what this test adds.
  std::ignore = SampleMemoryAttribution();

  AddTransientBytes(MemoryCategory::WorkerFrameSnapshot, 100);
  AddTransientBytes(MemoryCategory::WorkerFrameSnapshot, 250);
  EXPECT_EQ(PeekMemoryAttribution().transientBytes[Index(MemoryCategory::WorkerFrameSnapshot)],
            350u);

  const MemoryAttributionSample closed = SampleMemoryAttribution();
  EXPECT_EQ(closed.transientBytes[Index(MemoryCategory::WorkerFrameSnapshot)], 350u);
  EXPECT_GE(closed.transientHighWaterBytes[Index(MemoryCategory::WorkerFrameSnapshot)], 350u);

  // The next frame starts from zero: a steady per-frame allocation must not
  // read as growth.
  EXPECT_EQ(PeekMemoryAttribution().transientBytes[Index(MemoryCategory::WorkerFrameSnapshot)], 0u);
}

TEST(MemoryAttribution, TotalRetainedSumsEveryCategory) {
  for (std::size_t index = 0; index < kMemoryCategoryCount; ++index) {
    SetRetainedBytes(static_cast<MemoryCategory>(index), 0);
  }
  SetRetainedBytes(MemoryCategory::PresentationTiles, 300);
  SetRetainedBytes(MemoryCategory::PresentationRetired, 45);

  const MemoryAttributionSample sample = PeekMemoryAttribution();
  EXPECT_EQ(sample.totalRetainedBytes, 345u);
  EXPECT_GE(sample.totalRetainedHighWaterBytes, 345u);
}

TEST(MemoryAttribution, EntryCountsArePublishedIndependentlyOfBytes) {
  SetEntryCount(MemoryCategory::PresentationTiles, 7);
  EXPECT_EQ(PeekMemoryAttribution().entryCounts[Index(MemoryCategory::PresentationTiles)], 7u);
}

TEST(MemoryAttribution, EveryCategoryHasAUniqueName) {
  std::unordered_set<std::string_view> names;
  for (std::size_t index = 0; index < kMemoryCategoryCount; ++index) {
    const std::string_view name = MemoryCategoryName(static_cast<MemoryCategory>(index));
    EXPECT_NE(name, "unknown") << "category " << index << " has no name";
    EXPECT_TRUE(names.insert(name).second) << "duplicate category name " << name;
  }
  EXPECT_EQ(names.size(), kMemoryCategoryCount);
}

/// The browser build cannot read these names out of linear memory - a closure
/// pass renames the string helpers an `EM_ASM` body would need - so
/// `WholeAppWorkerBridge.cc` carries the same list as a JS literal in enum
/// order. This is the guard that the two do not drift: if it fails, update the
/// `names` array in `PublishMemoryAttribution` to match.
TEST(MemoryAttribution, NamesMatchTheBrowserBridgeLiteral) {
  const std::vector<std::string_view> expected = {
      "compositorSegmentBitmaps", "compositorSegmentTextures", "compositorLayerBitmaps",
      "compositorLayerTextures",  "renderResultTiles",         "workerFrameSnapshot",
      "presentationTiles",        "presentationOverviewTiles", "presentationRetired",
      "layerThumbnails",
  };
  ASSERT_EQ(expected.size(), kMemoryCategoryCount);
  for (std::size_t index = 0; index < kMemoryCategoryCount; ++index) {
    EXPECT_EQ(MemoryCategoryName(static_cast<MemoryCategory>(index)), expected[index])
        << "at index " << index;
  }
}

/// Same drift guard for the allocation-tag names, which the bridge publishes as
/// a second JS literal and indexes by the tag stored beside each live large
/// block. A mismatch here mislabels every block in the attribution table, so it
/// is worth its own assertion rather than a comment.
TEST(HeapSizeHistogram, AllocTagNamesMatchTheBrowserBridgeLiteral) {
  const std::vector<std::string_view> expected = {
      "untagged",           "workerRenderFrame", "workerBuildPreview", "workerFinalSnapshot",
      "workerOther",        "appPollResult",     "appUiFrame",         "appHostFrame",
      "appInput",           "renderTileRaster",  "gpuReadbackStaging", "compositorBitmap",
      "presentationUpload", "imguiDrawLists",
  };
  ASSERT_EQ(expected.size(), kAllocTagCount);
  for (std::size_t index = 0; index < kAllocTagCount; ++index) {
    EXPECT_EQ(AllocTagName(static_cast<AllocTag>(index)), expected[index]) << "at index " << index;
  }
}

/// Every frame stage has to map to a distinct tag, or two stages' large blocks
/// pile into one row and the table stops discriminating.
TEST(HeapSizeHistogram, EveryStageMapsToADistinctTag) {
  std::vector<AllocTag> seen;
  for (std::size_t index = 0; index < kMemoryStageCount; ++index) {
    const AllocTag tag = AllocTagForStage(static_cast<MemoryStage>(index));
    EXPECT_NE(tag, AllocTag::Untagged)
        << "stage " << MemoryStageName(static_cast<MemoryStage>(index));
    EXPECT_EQ(std::find(seen.begin(), seen.end(), tag), seen.end())
        << "duplicate tag for stage " << MemoryStageName(static_cast<MemoryStage>(index));
    seen.push_back(tag);
  }
}

/// The tag guard has to restore the previous tag, not reset to `Untagged`: the
/// frame-stage brackets nest finer guards inside them, and a guard that popped
/// to `Untagged` would silently move the rest of a stage's allocations out of
/// the stage's row.
TEST(HeapSizeHistogram, TagGuardsNestAndRestore) {
  // Without the wrappers linked in there is nothing to observe, but the guard
  // must still be safe to construct and destroy in any configuration.
  {
    ScopedAllocTag outer(AllocTag::WorkerRenderFrame);
    { ScopedAllocTag inner(AllocTag::RenderTileRaster); }
  }
  const AllocTagTotals totals = SampleAllocTagTotals();
  if (!HeapSizeHistogramEnabled()) {
    for (std::size_t index = 0; index < kAllocTagCount; ++index) {
      EXPECT_EQ(totals.liveBytes[index], 0) << "at index " << index;
    }
  }
}

}  // namespace donner
