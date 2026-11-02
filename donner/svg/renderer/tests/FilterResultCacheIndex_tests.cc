#include "donner/svg/renderer/FilterResultCacheIndex.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace donner::svg {
namespace {

using Index = FilterResultCacheIndex<int>;

constexpr std::array<double, 4> kIdentity = {1.0, 0.0, 0.0, 1.0};

FilterResultContentKey Content(const std::shared_ptr<int>& document, std::uint32_t element) {
  FilterResultContentKey key;
  key.document = document;
  key.filteredElement = element;
  return key;
}

FilterResultPlacement Linear(std::array<double, 4> linear) {
  FilterResultPlacement placement;
  placement.linear = linear;
  return placement;
}

std::array<double, 4> Scale(double scale) {
  return {scale, 0.0, 0.0, scale};
}

TEST(FilterResultCacheIndexTest, FindsOnlyMatchingContentAndPlacement) {
  auto document = std::make_shared<int>(0);
  Index index;
  (void)index.insert(Content(document, 1), Linear(kIdentity), 7, 16, 1);

  ASSERT_NE(index.find(Content(document, 1), Linear(kIdentity), 1), nullptr);
  EXPECT_EQ(index.find(Content(document, 1), Linear(kIdentity), 1)->payload, 7);
  EXPECT_EQ(index.find(Content(document, 2), Linear(kIdentity), 1), nullptr);
  EXPECT_EQ(index.find(Content(document, 1), Linear(Scale(2.0)), 1), nullptr);

  FilterResultPlacement bucketed = Linear(kIdentity);
  bucketed.fractionBucketX = 1;
  EXPECT_EQ(index.find(Content(document, 1), bucketed, 1), nullptr);

  FilterResultContentKey revised = Content(document, 1);
  revised.documentRevision = 1;
  EXPECT_EQ(index.find(revised, Linear(kIdentity), 1), nullptr);
}

TEST(FilterResultCacheIndexTest, DestroyedDocumentsMatchNothingAndAreDropped) {
  auto document = std::make_shared<int>(0);
  Index index;
  const FilterResultContentKey content = Content(document, 1);
  (void)index.insert(content, Linear(kIdentity), 7, 16, 1);
  document.reset();
  EXPECT_EQ(index.find(content, Linear(kIdentity), 1), nullptr);

  const std::vector<int> dropped = index.dropExpired();
  EXPECT_EQ(dropped, std::vector<int>{7});
  EXPECT_EQ(index.size(), 0u);
  EXPECT_EQ(index.bytes(), 0u);
}

TEST(FilterResultCacheIndexTest, PendingResultsAreFoundUntilDroppedAndKeptOnceCommitted) {
  auto document = std::make_shared<int>(0);
  Index index;
  (void)index.insert(Content(document, 1), Linear(kIdentity), 1, 10, 1);
  EXPECT_NE(index.find(Content(document, 1), Linear(kIdentity), 1), nullptr);
  EXPECT_EQ(index.dropPending(), std::vector<int>{1});
  EXPECT_EQ(index.size(), 0u);

  (void)index.insert(Content(document, 2), Linear(kIdentity), 2, 10, 2);
  index.commitPending();
  EXPECT_TRUE(index.dropPending().empty());
  EXPECT_NE(index.find(Content(document, 2), Linear(kIdentity), 3), nullptr);
}

TEST(FilterResultCacheIndexTest, EvictsLeastRecentlyUsedResultsNotUsedRecently) {
  auto document = std::make_shared<int>(0);
  Index index;
  for (std::uint32_t i = 0; i < Index::kMaximumEntries; ++i) {
    ASSERT_TRUE(index.canInsert(1, 1));
    EXPECT_TRUE(
        index.insert(Content(document, i), Linear(kIdentity), static_cast<int>(i), 1, 1).empty());
  }
  // Every entry was used in frame 1, so frames 1 and 2 refuse to evict any of them.
  EXPECT_FALSE(index.canInsert(1, 1));
  EXPECT_FALSE(index.canInsert(1, 2));
  // In frame 3, touch the oldest so the second oldest becomes the victim.
  ASSERT_NE(index.find(Content(document, 0), Linear(kIdentity), 3), nullptr);
  ASSERT_TRUE(index.canInsert(1, 3));
  const std::vector<int> evicted =
      index.insert(Content(document, 1000), Linear(kIdentity), 1000, 1, 3);
  EXPECT_EQ(evicted, std::vector<int>{1});
  EXPECT_EQ(index.size(), Index::kMaximumEntries);
  EXPECT_NE(index.find(Content(document, 0), Linear(kIdentity), 3), nullptr);
}

TEST(FilterResultCacheIndexTest, LoweringTheByteLimitEvictsUntilItFits) {
  auto document = std::make_shared<int>(0);
  Index index;
  for (std::uint32_t i = 0; i < 4; ++i) {
    (void)index.insert(Content(document, i), Linear(kIdentity), static_cast<int>(i), 100, 1);
  }
  EXPECT_EQ(index.bytes(), 400u);
  const std::vector<int> evicted = index.setByteLimit(250);
  EXPECT_EQ(evicted, (std::vector<int>{0, 1}));
  EXPECT_EQ(index.bytes(), 200u);
  EXPECT_FALSE(index.canInsert(251, 5));
  EXPECT_TRUE(index.canInsert(150, 5));

  // Inserting under the lowered limit evicts what the new entry needs.
  EXPECT_EQ(index.insert(Content(document, 9), Linear(kIdentity), 9, 150, 5), std::vector<int>{2});
  EXPECT_EQ(index.bytes(), 250u);
}

TEST(FilterResultCacheIndexTest, AdmitsOnTheSecondMissAtATransform) {
  auto document = std::make_shared<int>(0);
  Index index;
  const FilterResultContentKey content = Content(document, 1);
  EXPECT_FALSE(index.admitMiss(content, kIdentity, 10));
  EXPECT_TRUE(index.admitMiss(content, kIdentity, 10));
  EXPECT_TRUE(index.admitMiss(content, kIdentity, 11));
  // A miss long after the last one starts over.
  EXPECT_FALSE(index.admitMiss(content, kIdentity, 11 + Index::kAdmissionWindowFrames + 1));
}

TEST(FilterResultCacheIndexTest, ContinuouslyChangingScaleIsNotAdmittedUntilItHolds) {
  auto document = std::make_shared<int>(0);
  Index index;
  const FilterResultContentKey content = Content(document, 1);
  for (std::uint64_t frame = 1; frame < 12; ++frame) {
    EXPECT_FALSE(index.admitMiss(content, Scale(1.0 + 0.025 * static_cast<double>(frame)), frame))
        << frame;
  }
  // The final scale repeats on the next frame and is admitted.
  EXPECT_TRUE(index.admitMiss(content, Scale(1.0 + 0.025 * 11.0), 12));
}

TEST(FilterResultCacheIndexTest, SeveralStaticScalesOfOneContentAreAdmitted) {
  auto document = std::make_shared<int>(0);
  Index index;
  const FilterResultContentKey content = Content(document, 1);
  for (std::uint64_t frame = 1; frame <= 3; ++frame) {
    const bool admitted = frame > 1;
    EXPECT_EQ(index.admitMiss(content, Scale(1.0), frame), admitted);
    EXPECT_EQ(index.admitMiss(content, Scale(1.5), frame), admitted);
    EXPECT_EQ(index.admitMiss(content, Scale(2.0), frame), admitted);
  }
}

TEST(FilterResultCacheIndexTest, ClearReturnsEveryPayload) {
  auto document = std::make_shared<int>(0);
  Index index;
  (void)index.insert(Content(document, 1), Linear(kIdentity), 1, 10, 1);
  (void)index.insert(Content(document, 2), Linear(kIdentity), 2, 10, 1);
  EXPECT_EQ(index.clear(), (std::vector<int>{1, 2}));
  EXPECT_EQ(index.size(), 0u);
  EXPECT_EQ(index.bytes(), 0u);
  EXPECT_FALSE(index.admitMiss(Content(document, 1), Scale(3.0), 2));
}

}  // namespace
}  // namespace donner::svg
