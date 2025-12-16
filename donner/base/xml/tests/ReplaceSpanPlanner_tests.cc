#include "donner/base/xml/ReplaceSpanPlanner.h"

#include "donner/base/FileOffset.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::xml {
namespace {

TEST(ReplaceSpanPlannerTests, SortsNonOverlappingReplacements) {
  ReplaceSpanPlanner planner;
  ReplaceSpanPlanner::ReplaceSpan first{SourceDocument::Replacement{
      FileOffsetRange{FileOffset::Offset(8), FileOffset::Offset(12)}, RcString("b")},
                                         std::nullopt};
  ReplaceSpanPlanner::ReplaceSpan second{SourceDocument::Replacement{
      FileOffsetRange{FileOffset::Offset(0), FileOffset::Offset(2)}, RcString("a")},
                                         std::nullopt};

  auto result = planner.plan({first, second});
  ASSERT_TRUE(result.hasResult());
  ASSERT_EQ(result.result().ordered.size(), 2u);
  EXPECT_EQ(result.result().ordered[0].range.start.offset.value(), 0u);
  EXPECT_EQ(result.result().ordered[1].range.start.offset.value(), 8u);
  EXPECT_FALSE(result.result().usedFallback);
}

TEST(ReplaceSpanPlannerTests, UsesFallbackWhenOffsetsMissing) {
  ReplaceSpanPlanner planner;
  ReplaceSpanPlanner::ReplaceSpan entry{
      SourceDocument::Replacement{
          FileOffsetRange{FileOffset::EndOfString(), FileOffset::EndOfString()}, RcString("b")},
      SourceDocument::Replacement{
          FileOffsetRange{FileOffset::Offset(4), FileOffset::Offset(6)}, RcString("fallback")}};

  auto result = planner.plan({entry});
  ASSERT_TRUE(result.hasResult());
  ASSERT_EQ(result.result().ordered.size(), 1u);
  EXPECT_EQ(result.result().ordered[0].replacement, RcString("fallback"));
  EXPECT_TRUE(result.result().usedFallback);
}

TEST(ReplaceSpanPlannerTests, RejectsOverlapWithoutFallback) {
  ReplaceSpanPlanner planner;
  ReplaceSpanPlanner::ReplaceSpan first{SourceDocument::Replacement{
      FileOffsetRange{FileOffset::Offset(0), FileOffset::Offset(5)}, RcString("a")},
                                         std::nullopt};
  ReplaceSpanPlanner::ReplaceSpan second{SourceDocument::Replacement{
      FileOffsetRange{FileOffset::Offset(4), FileOffset::Offset(8)}, RcString("b")},
                                         std::nullopt};

  auto result = planner.plan({first, second});
  ASSERT_FALSE(result.hasResult());
  EXPECT_EQ(result.error().reason,
            RcString("Overlapping replacements with no compatible fallback"));
}

TEST(ReplaceSpanPlannerTests, FallbackExpandsToCoverOverlap) {
  ReplaceSpanPlanner planner;
  ReplaceSpanPlanner::ReplaceSpan first{SourceDocument::Replacement{
      FileOffsetRange{FileOffset::Offset(10), FileOffset::Offset(12)}, RcString("a")},
                                         std::nullopt};
  ReplaceSpanPlanner::ReplaceSpan second{
      SourceDocument::Replacement{
          FileOffsetRange{FileOffset::Offset(11), FileOffset::Offset(14)}, RcString("b")},
      SourceDocument::Replacement{
          FileOffsetRange{FileOffset::Offset(10), FileOffset::Offset(15)}, RcString("merged")}};

  auto result = planner.plan({first, second});
  ASSERT_TRUE(result.hasResult());
  ASSERT_EQ(result.result().ordered.size(), 1u);
  EXPECT_EQ(result.result().ordered[0].range.start.offset.value(), 10u);
  EXPECT_EQ(result.result().ordered[0].range.end.offset.value(), 15u);
  EXPECT_EQ(result.result().ordered[0].replacement, RcString("merged"));
  EXPECT_TRUE(result.result().usedFallback);
}

}  // namespace
}  // namespace donner::xml

