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

TEST(FilterResultCacheIndexTest, FindsOnlyMatchingContentAndPlacement) {
  auto document = std::make_shared<int>(0);
  Index index;
  (void)index.insert(Content(document, 1), Linear(kIdentity), 7, 16);

  ASSERT_NE(index.find(Content(document, 1), Linear(kIdentity)), nullptr);
  EXPECT_EQ(index.find(Content(document, 1), Linear(kIdentity))->payload, 7);
  EXPECT_EQ(index.find(Content(document, 2), Linear(kIdentity)), nullptr);
  EXPECT_EQ(index.find(Content(document, 1), Linear({2.0, 0.0, 0.0, 2.0})), nullptr);

  FilterResultPlacement bucketed = Linear(kIdentity);
  bucketed.fractionBucketX = 1;
  EXPECT_EQ(index.find(Content(document, 1), bucketed), nullptr);

  FilterResultContentKey revised = Content(document, 1);
  revised.documentRevision = 1;
  EXPECT_EQ(index.find(revised, Linear(kIdentity)), nullptr);
}

TEST(FilterResultCacheIndexTest, DestroyedDocumentsMatchNothingAndAreDropped) {
  auto document = std::make_shared<int>(0);
  Index index;
  const FilterResultContentKey content = Content(document, 1);
  (void)index.insert(content, Linear(kIdentity), 7, 16);
  document.reset();
  EXPECT_EQ(index.find(content, Linear(kIdentity)), nullptr);

  const std::vector<int> dropped = index.dropExpired();
  EXPECT_EQ(dropped, std::vector<int>{7});
  EXPECT_EQ(index.size(), 0u);
  EXPECT_EQ(index.bytes(), 0u);
}

TEST(FilterResultCacheIndexTest, EvictsLeastRecentlyUsedPastTheEntryBound) {
  auto document = std::make_shared<int>(0);
  Index index;
  for (std::uint32_t i = 0; i < Index::kMaximumEntries; ++i) {
    EXPECT_TRUE(
        index.insert(Content(document, i), Linear(kIdentity), static_cast<int>(i), 1).empty());
  }
  // Touch the oldest so the second oldest becomes the victim.
  ASSERT_NE(index.find(Content(document, 0), Linear(kIdentity)), nullptr);
  const std::vector<int> evicted =
      index.insert(Content(document, 1000), Linear(kIdentity), 1000, 1);
  EXPECT_EQ(evicted, std::vector<int>{1});
  EXPECT_EQ(index.size(), Index::kMaximumEntries);
  EXPECT_NE(index.find(Content(document, 0), Linear(kIdentity)), nullptr);
}

TEST(FilterResultCacheIndexTest, LoweringTheByteLimitEvictsUntilItFits) {
  auto document = std::make_shared<int>(0);
  Index index;
  for (std::uint32_t i = 0; i < 4; ++i) {
    (void)index.insert(Content(document, i), Linear(kIdentity), static_cast<int>(i), 100);
  }
  EXPECT_EQ(index.bytes(), 400u);
  const std::vector<int> evicted = index.setByteLimit(250);
  EXPECT_EQ(evicted, (std::vector<int>{0, 1}));
  EXPECT_EQ(index.bytes(), 200u);
  EXPECT_FALSE(index.canHold(251));
  EXPECT_TRUE(index.canHold(250));

  // Inserting under the lowered limit evicts what the new entry needs.
  EXPECT_EQ(index.insert(Content(document, 9), Linear(kIdentity), 9, 150), std::vector<int>{2});
  EXPECT_EQ(index.bytes(), 250u);
}

TEST(FilterResultCacheIndexTest, AdmitsFirstMissesAndRepeatedTransforms) {
  auto document = std::make_shared<int>(0);
  Index index;
  const FilterResultContentKey content = Content(document, 1);
  EXPECT_TRUE(index.admitMiss(content, kIdentity, 10));
  // Another fractional bucket at the same linear transform is not animation.
  EXPECT_TRUE(index.admitMiss(content, kIdentity, 10));
  EXPECT_TRUE(index.admitMiss(content, kIdentity, 11));
}

TEST(FilterResultCacheIndexTest, ContinuouslyChangingScaleIsNotAdmittedUntilItHolds) {
  auto document = std::make_shared<int>(0);
  Index index;
  const FilterResultContentKey content = Content(document, 1);
  EXPECT_TRUE(index.admitMiss(content, kIdentity, 1));
  for (std::uint64_t frame = 2; frame < 12; ++frame) {
    const double scale = 1.0 + 0.025 * static_cast<double>(frame);
    EXPECT_FALSE(index.admitMiss(content, {scale, 0.0, 0.0, scale}, frame)) << frame;
  }
  // The final scale repeats on the next frame and is admitted.
  const double held = 1.0 + 0.025 * 11.0;
  EXPECT_TRUE(index.admitMiss(content, {held, 0.0, 0.0, held}, 12));
}

TEST(FilterResultCacheIndexTest, ScaleChangeAfterAQuietPeriodIsAdmitted) {
  auto document = std::make_shared<int>(0);
  Index index;
  const FilterResultContentKey content = Content(document, 1);
  EXPECT_TRUE(index.admitMiss(content, kIdentity, 1));
  EXPECT_TRUE(
      index.admitMiss(content, {2.0, 0.0, 0.0, 2.0}, 1 + Index::kAnimationWindowFrames + 1));
}

TEST(FilterResultCacheIndexTest, ClearReturnsEveryPayload) {
  auto document = std::make_shared<int>(0);
  Index index;
  (void)index.insert(Content(document, 1), Linear(kIdentity), 1, 10);
  (void)index.insert(Content(document, 2), Linear(kIdentity), 2, 10);
  EXPECT_EQ(index.clear(), (std::vector<int>{1, 2}));
  EXPECT_EQ(index.size(), 0u);
  EXPECT_EQ(index.bytes(), 0u);
  EXPECT_TRUE(index.admitMiss(Content(document, 1), {3.0, 0.0, 0.0, 3.0}, 2));
}

}  // namespace
}  // namespace donner::svg
