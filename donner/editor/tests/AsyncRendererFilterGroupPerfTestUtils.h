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

void RunFilterGroupSubtreeDragPerfScenario(FilterGroupSubtreeDragPerfResult* result);

}  // namespace donner::editor
