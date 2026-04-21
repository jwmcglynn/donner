#include "donner/editor/tests/AsyncRendererFilterGroupPerfTestUtils.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

// Same filter-group drag repro as the correctness target, but the runner-speed-
// dependent wall-clock budgets live in the nightly-only `perf` target.
TEST(AsyncRendererPerfWallclockTest, FilterGroupSubtreeDragStaysUnderNightlyWallclockBudget) {
  FilterGroupSubtreeDragPerfResult result;
  RunFilterGroupSubtreeDragPerfScenario(&result);

  EXPECT_LT(result.avgDragFrameMs, 100.0) << "average filter-group drag frame far above even the "
                                             "widened CI runner budget — something is doing a "
                                             "full-document re-render per frame";
  EXPECT_LT(result.maxDragFrameMs, 200.0) << "max filter-group drag frame exceeded the widened CI "
                                             "budget — individual frame is doing full-prepare work";
}

}  // namespace
}  // namespace donner::editor
