#include "donner/editor/tests/AsyncRendererFilterGroupPerfTestUtils.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

// Regression: dragging a `<g filter=...>` subtree (the editor's flow when the
// user picks the filter group from the tree view and drags) must go through the
// compositor's translation-only fast path. The counter assertions are CPU-speed
// invariant, so they stay on the default PR gate.
TEST(AsyncRendererPerfCorrectnessTest, FilterGroupSubtreeDragHitsTranslationFastPathCounters) {
  FilterGroupSubtreeDragPerfResult result;
  RunFilterGroupSubtreeDragPerfScenario(&result);

  EXPECT_GE(result.fastPathFrames, static_cast<uint64_t>(result.dragFrames))
      << "filter-group subtree drag is not hitting the translation-only fast "
         "path — compositor is falling through to prepareDocumentForRendering "
         "every frame";
  EXPECT_LE(result.slowPathFramesWithDirty, 1u)
      << "more than the pre-warm frame slipped into the slow path";
}

}  // namespace
}  // namespace donner::editor
