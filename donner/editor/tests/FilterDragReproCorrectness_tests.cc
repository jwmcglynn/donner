/// @file
///
/// CPU-invariant portion of the `filter_drag_repro` replay test.
/// Runs on the default PR gate (`bazel test //...`) because every
/// assertion here is independent of the host runner's wall-clock
/// speed - fast-path counters, the selection-change invariant, and
/// the "something was selected at all" invariant all derive from
/// compositor and editor state, not from timing.
///
/// Paired with `FilterDragReproWallclock_tests.cc`, which runs the
/// same scenario but asserts the runner-speed-dependent drag-frame
/// wall-clock budgets and is tagged `manual` + `perf` so it only
/// runs in the nightly `perf` lane.

#include "donner/editor/tests/FilterDragReproTestUtils.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::editor::filter_drag_repro {
namespace {

using testing::AllOf;
using testing::Field;
using testing::Gt;
using testing::Le;

MATCHER(SelectionChangedAcrossDrags, "") {
  if (!arg.firstSelectionExists) {
    *result_listener << "first drag ended without a latched selection";
    return false;
  }
  if (!arg.secondSelectionExists) {
    *result_listener << "second mouse-down never produced a selection";
    return false;
  }
  if (!arg.selectionChangedAcrossDrags) {
    *result_listener << "selection stayed on " << arg.firstSelectionId;
    return false;
  }
  return true;
}

MATCHER(SingleTargetDragUsedFastPath, "") {
  if (arg.fastPathFrames == 0) {
    *result_listener << "fastPathFrames is 0";
    return false;
  }
  if (arg.slowPathFramesWithDirty > 1) {
    *result_listener << "slowPathFramesWithDirty is " << arg.slowPathFramesWithDirty;
    return false;
  }
  return true;
}

TEST(FilterDragReproCorrectnessTest, ReplayReSelectsAndHitsFastPathWhenEligible) {
  FilterDragReproResult r;
  RunFilterDragReproScenario(&r, FilterDragReproReplayMode::CompactCorrectness);
  if (r.skipped) {
    GTEST_SKIP() << "Required data files not available in runfiles";
  }

  // Selection invariant: after the first mouse-up, ONE element must be
  // selected (the first drag's target). The second mouse-down in the
  // recording lands on a different location; it must hit-test, replace
  // the selection, and produce a visible drag preview - the user's
  // "I can't select any other elements" complaint is exactly the
  // failure mode where the first drag's selection sticks because the
  // new mouse-down was dropped (async renderer busy / first drag
  // layer never demoted).
  EXPECT_THAT(r, SelectionChangedAcrossDrags());

  // With sibling-expansion enabled, a click on a compositing group may
  // intentionally turn into a multi-element drag (halo + bright core +
  // highlights). Those drags are no longer eligible for the single-
  // layer compositor preview, so the old "every drag must hit the fast
  // path" invariant is too strict. The CPU-invariant property we still
  // need is narrower and more accurate:
  //   * any drag that remains single-target must hit the translation-
  //     only fast path;
  //   * sibling-expanded composite drags may fall back to the mutation
  //     path without being considered a regression.
  if (r.firstSelectionSize == 1) {
    EXPECT_THAT(r.firstDragCounters, SingleTargetDragUsedFastPath());
  }

  EXPECT_THAT(
      r.secondDragCounters,
      AllOf(Field("fastPathFrames", &DragCounterStats::fastPathFrames, Gt(0u)),
            Field("slowPathFramesWithDirty", &DragCounterStats::slowPathFramesWithDirty, Le(1u))));
}

}  // namespace
}  // namespace donner::editor::filter_drag_repro
