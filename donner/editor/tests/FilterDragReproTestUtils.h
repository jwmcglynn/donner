#pragma once
/// @file
///
/// Shared replay harness for the `FilterDragRepro_*` tests. The harness
/// exercises the full editor stack (EditorApp + SelectTool +
/// RenderCoordinator + AsyncRenderer) against the checked-in
/// `filter_drag_repro.rnr` recording against `donner_splash.svg`, and
/// returns aggregated per-gesture stats.
///
/// The scenario is split across two paired test targets produced by
/// `donner_perf_cc_test`:
///   - `filter_drag_repro_tests_correctness` — CPU-invariant
///     assertions (fast-path counters, selection invariants). Runs on
///     the default PR gate via `bazel test //...`.
///   - `filter_drag_repro_tests_wallclock` — wall-clock budget
///     assertions. Tagged `manual` + `perf` so it runs in the nightly
///     `perf` lane, not on the PR gate where shared GitHub CI runners
///     make per-frame wall-clock measurements flaky.

#include <cstdint>
#include <string>

#include "donner/base/EcsRegistry_fwd.h"

namespace donner::editor::filter_drag_repro {

struct DragStats {
  int frameCount = 0;
  double avgWorkerMs = 0.0;
  double maxWorkerMs = 0.0;
};

/// Aggregated result of replaying `filter_drag_repro.rnr` through the
/// real editor stack. `skipped == true` means required data files were
/// missing in runfiles and the caller should `GTEST_SKIP()` — the other
/// fields are meaningless in that case.
struct FilterDragReproResult {
  bool skipped = false;

  // Per-gesture drag stats, computed over the frames strictly between
  // the mouse-down and mouse-up of each gesture (so cold mouse-down /
  // mouse-up frames are excluded).
  DragStats firstDrag;
  DragStats secondDrag;

  // Selection invariants — the entity selected after each mouse-up.
  donner::Entity firstSelection{};
  donner::Entity secondSelection{};

  // Diagnostic identifiers mirrored from the live editor, surfaced so
  // failing-test output tells the reader which element the user hit
  // and which filter ancestor was in scope.
  std::string firstSelectionId;
  std::string firstSelectionFilterAncestorId;
  std::string secondSelectionId;
  std::string secondSelectionFilterAncestorId;

  // CPU-speed-invariant compositor counters — the primary regression
  // signal that the translation-only fast path is active.
  uint64_t fastPathFrames = 0;
  uint64_t slowPathFramesWithDirty = 0;
  uint64_t noDirtyFrames = 0;
};

/// Replay the checked-in recording and populate `out`. Also streams the
/// standard diagnostic log lines (per-gesture avg/max, histograms, first
/// 5 frames, fast-path counters, selection ids) to `std::cerr` so both
/// paired tests produce the same operator-facing log output.
///
/// Uses `ASSERT_*` internally; if an assertion trips, `out->skipped`
/// stays false and the caller's test is halted by the assertion flow.
/// On genuine "files missing in runfiles", sets `out->skipped = true`
/// and returns without asserting — the caller issues `GTEST_SKIP()`.
void RunFilterDragReproScenario(FilterDragReproResult* out);

}  // namespace donner::editor::filter_drag_repro
