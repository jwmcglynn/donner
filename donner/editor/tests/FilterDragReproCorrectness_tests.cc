/// @file
///
/// CPU-invariant portion of the `filter_drag_repro` replay test.
/// Runs on the default PR gate (`bazel test //...`) because every
/// assertion here is independent of the host runner's wall-clock
/// speed — fast-path counters, the selection-change invariant, and
/// the "something was selected at all" invariant all derive from
/// compositor and editor state, not from timing.
///
/// Paired with `FilterDragReproWallclock_tests.cc`, which runs the
/// same scenario but asserts the runner-speed-dependent drag-frame
/// wall-clock budgets and is tagged `manual` + `perf` so it only
/// runs in the nightly `perf` lane.

#include "donner/editor/tests/FilterDragReproTestUtils.h"
#include "gtest/gtest.h"

namespace donner::editor::filter_drag_repro {
namespace {

TEST(FilterDragReproCorrectnessTest, ReplayHitsFastPathAndReSelects) {
  FilterDragReproResult r;
  RunFilterDragReproScenario(&r);
  if (r.skipped) {
    GTEST_SKIP() << "Required data files not available in runfiles";
  }

  // Selection invariant: after the first mouse-up, ONE element must be
  // selected (the first drag's target). The second mouse-down in the
  // recording lands on a different location; it must hit-test, replace
  // the selection, and produce a visible drag preview — the user's
  // "I can't select any other elements" complaint is exactly the
  // failure mode where the first drag's selection sticks because the
  // new mouse-down was dropped (async renderer busy / first drag
  // layer never demoted).
  EXPECT_TRUE(r.firstSelectionExists)
      << "first drag ended without a latched selection — hit-test missed or gesture aborted";
  EXPECT_TRUE(r.secondSelectionExists)
      << "second mouse-down never produced a selection — user's 'can't select anything else' "
         "complaint exactly";
  EXPECT_TRUE(r.selectionChangedAcrossDrags)
      << "second drag's selection did not differ from the first — second mouse-down was ignored "
         "(likely dropped because async renderer stayed busy through the entire repro window)";

  // Fast-path counter regression gate. This is the CPU-speed-invariant
  // signal that the compositor is riding the translation-only fast path
  // during the drag rather than re-running the heavy
  // `prepareDocumentForRendering` pipeline every frame. Catches the
  // "really laggy" regression in the metric that isn't runner-
  // dependent. The exact counts depend on frame count in the
  // recording, so we assert the qualitative shape: at least some
  // fast-path frames, at most one slow-path-with-dirty frame
  // (the prewarm).
  EXPECT_GT(r.fastPathFrames, 0u)
      << "no frames took the translation-only fast path — compositor-group elevation / "
         "skipMainComposeDuringSplit path is dead or mis-gated";
  EXPECT_LE(r.slowPathFramesWithDirty, 1u)
      << "more than the pre-warm frame slipped into the slow path — drag is spending wall-clock "
         "re-preparing the document per frame";
}

}  // namespace
}  // namespace donner::editor::filter_drag_repro
