/// @file
///
/// Wall-clock budget assertions for the `filter_drag_repro` replay.
/// These numbers are runner-speed-dependent - the same scenario runs
/// at ~2 ms/frame on dev hardware and 40-50 ms/frame avg (with
/// single-outlier excursions past 200 ms) on shared GitHub CI
/// runners. Put them in the `_wallclock` target (tagged `manual` +
/// `perf`) so they run in the nightly `perf` lane instead of on every
/// PR.
///
/// The CPU-invariant portion - fast-path counters and selection
/// invariants - lives in `FilterDragReproCorrectness_tests.cc` and
/// does run on the PR gate.

#include "donner/editor/tests/FilterDragReproTestUtils.h"
#include "gtest/gtest.h"

namespace donner::editor::filter_drag_repro {
namespace {

TEST(FilterDragReproWallclockTest, DragFramesStayUnderNightlyWallclockBudget) {
  FilterDragReproResult r;
  RunFilterDragReproScenario(&r);
  if (r.skipped) {
    GTEST_SKIP() << "Required data files not available in runfiles";
  }

  // Budgets tuned for the nightly lane where we can assume roughly
  // dev-machine-class runners. The numbers here are intentionally
  // tighter than the transitional thresholds that the PR-gated test
  // used - this is where we actually want to catch regressions.
  //
  // The original user report was ~250 ms / frame avg during a drag on
  // a `<g filter>` subtree. The primary gate is `avg`, not `max`:
  // sustained laggy behavior is what the user perceives. The `max`
  // gate catches single catastrophic frames but has to leave headroom
  // for cold-frame + runner-scheduling noise.
  constexpr double kDragWorkerAvgBudgetMs = 80.0;
  constexpr double kDragWorkerMaxBudgetMs = 250.0;

  // Sibling-expanded composite drags intentionally use the DOM-mutation
  // path today, so only single-target drags are expected to satisfy the
  // old fast-path wall-clock budget.
  if (r.firstSelectionSize == 1) {
    EXPECT_LT(r.firstDrag.avgWorkerMs, kDragWorkerAvgBudgetMs)
        << "single-target first drag exceeds the worker-ms budget";
    EXPECT_LT(r.firstDrag.maxWorkerMs, kDragWorkerMaxBudgetMs)
        << "single-target first drag exceeded the worst-frame budget";
  }
  EXPECT_LT(r.secondDrag.avgWorkerMs, kDragWorkerAvgBudgetMs)
      << "second drag: avg worker ms exceeds budget";
  EXPECT_LT(r.secondDrag.maxWorkerMs, kDragWorkerMaxBudgetMs)
      << "second drag: worst-frame worker ms exceeds budget";
}

}  // namespace
}  // namespace donner::editor::filter_drag_repro
