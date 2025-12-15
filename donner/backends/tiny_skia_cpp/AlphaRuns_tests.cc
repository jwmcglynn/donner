#include "donner/backends/tiny_skia_cpp/AlphaRuns.h"

#include <vector>

#include "gtest/gtest.h"

namespace donner::backends::tiny_skia_cpp {
namespace {

std::vector<uint8_t> Expand(const AlphaRuns& runs, uint32_t width) {
  std::vector<uint8_t> out;
  out.reserve(width);

  size_t runIndex = 0;
  size_t alphaIndex = 0;
  while (true) {
    const size_t n = runs.runs()[runIndex];
    if (n == 0) {
      break;
    }
    const uint8_t coverage = runs.alpha()[alphaIndex];
    for (size_t i = 0; i < n; ++i) {
      out.push_back(coverage);
    }
    runIndex += n;
    alphaIndex += n;
  }

  return out;
}

TEST(AlphaRunsTests, AddsSingleRun) {
  constexpr uint32_t kWidth = 6;
  AlphaRuns runs(kWidth);

  runs.add(1, 64, 2, 32, 255, 0);

  EXPECT_EQ(Expand(runs, kWidth), (std::vector<uint8_t>{0, 64, 255, 255, 32, 0}));
}

TEST(AlphaRunsTests, RespectsOffsetsAcrossSpans) {
  constexpr uint32_t kWidth = 8;
  AlphaRuns runs(kWidth);

  size_t offset = runs.add(1, 200, 1, 200, 255, 0);
  offset = runs.add(5, 180, 0, 0, 255, offset);

  EXPECT_EQ(Expand(runs, kWidth),
            (std::vector<uint8_t>{0, 200, 255, 200, 0, 180, 0, 0}));
}

}  // namespace
}  // namespace donner::backends::tiny_skia_cpp

