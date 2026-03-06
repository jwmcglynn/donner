#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "tiny_skia/AlphaRuns.h"

using ::testing::ElementsAre;
using ::testing::Optional;

TEST(AlphaRunsTest, CatchOverflow) {
  const std::array overflowResults{
      tiny_skia::AlphaRuns::catchOverflow(0),   tiny_skia::AlphaRuns::catchOverflow(1),
      tiny_skia::AlphaRuns::catchOverflow(128), tiny_skia::AlphaRuns::catchOverflow(255),
      tiny_skia::AlphaRuns::catchOverflow(256),
  };
  EXPECT_THAT(overflowResults, ElementsAre(0u, 1u, 128u, 255u, 255u));
}

TEST(AlphaRunsTest, ConstructorResetAndIsEmpty) {
  tiny_skia::AlphaRuns runs(5);
  EXPECT_TRUE(runs.empty());
  EXPECT_THAT(runs.runs[0], Optional(5u));
  EXPECT_EQ(runs.alpha[0], 0u);
  EXPECT_FALSE(runs.runs[5].has_value());

  runs.runs = std::vector<tiny_skia::AlphaRun>(6);
  runs.alpha = std::vector<std::uint8_t>(6, 42);
  runs.reset(7);
  EXPECT_THAT(runs.runs[0], Optional(7u));
  EXPECT_EQ(runs.alpha[0], 0u);
}

TEST(AlphaRunsTest, AddComposesStartAndMiddle) {
  tiny_skia::AlphaRuns runs(5);
  auto offset = runs.add(1, 0, 3, 0, 70, 0);
  EXPECT_EQ(offset, 4u);
  EXPECT_FALSE(runs.empty());

  EXPECT_THAT(runs.runs[0], Optional(1u));
  EXPECT_THAT(runs.runs[1], Optional(3u));
  EXPECT_THAT(runs.runs[4], Optional(1u));
  EXPECT_EQ(runs.alpha[1], 70u);
}

TEST(AlphaRunsTest, AddUsesOffsetAndStopAlpha) {
  tiny_skia::AlphaRuns runs(6);
  const auto first = runs.add(1, 0, 3, 0, 70, 0);
  EXPECT_EQ(first, 4u);
  EXPECT_THAT(runs.runs[1], Optional(3u));
  EXPECT_EQ(runs.alpha[1], 70u);

  const auto second = runs.add(4, 0, 0, 15, 0, first);
  EXPECT_EQ(second, 4u);
  EXPECT_EQ(runs.alpha[4], 15u);
}

TEST(AlphaRunsTest, BreakRunSplitsRunsWithNonZeroBoundaries) {
  std::array<tiny_skia::AlphaRun, 8> runs{};
  std::array<std::uint8_t, 8> alpha{};
  runs[0] = tiny_skia::AlphaRun{8};
  alpha[0] = 11;

  tiny_skia::AlphaRuns::breakRun(std::span{runs}, std::span{alpha}, 0, 3);
  EXPECT_THAT(runs[0], Optional(3u));
  EXPECT_THAT(runs[3], Optional(5u));
  EXPECT_EQ(alpha[3], 11u);
  EXPECT_FALSE(runs[6].has_value());
}

TEST(AlphaRunsTest, BreakAtSplitsAtOffset) {
  std::array<tiny_skia::AlphaRun, 8> runs{};
  std::array<std::uint8_t, 8> alpha{};
  runs[0] = tiny_skia::AlphaRun{8};
  alpha[0] = 11;

  tiny_skia::AlphaRuns::breakAt(std::span{alpha}, std::span{runs}, 2);
  EXPECT_THAT(runs[0], Optional(2u));
  EXPECT_THAT(runs[2], Optional(6u));
  EXPECT_EQ(alpha[2], 11u);
}
