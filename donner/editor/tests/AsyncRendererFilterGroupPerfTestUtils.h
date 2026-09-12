#pragma once
/// @file

#include <cstdint>

namespace donner::editor {

struct FilterGroupSubtreeDragPerfResult {
  double avgDragFrameMs = 0.0;
  double maxDragFrameMs = 0.0;
  int dragFrames = 0;
  uint64_t fastPathFrames = 0;
  uint64_t slowPathFramesWithDirty = 0;
};

/// Replay the filter-group subtree drag and populate `result`.
///
/// Uses `ASSERT_*` internally, including on the splash the target declares in `data` when it
/// is not in the runfiles tree. Callers must therefore check `HasFatalFailure()` before
/// reading `result`.
void RunFilterGroupSubtreeDragPerfScenario(FilterGroupSubtreeDragPerfResult* result);

}  // namespace donner::editor
