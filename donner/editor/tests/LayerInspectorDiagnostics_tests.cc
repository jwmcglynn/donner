#include "donner/editor/LayerInspectorDiagnostics.h"

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

TEST(LayerInspectorDiagnosticsTest, ClassifiesCurrentCanvas) {
  const Vector2i canvas(320, 240);

  EXPECT_EQ(ClassifyCanvasFreshness(canvas, canvas, canvas), CanvasFreshness::Current);
  EXPECT_EQ(CanvasFreshnessStatusSuffix(CanvasFreshness::Current), "");
}

TEST(LayerInspectorDiagnosticsTest, CommitStallTakesPrecedence) {
  EXPECT_EQ(ClassifyCanvasFreshness(Vector2i(640, 480), Vector2i(320, 240), Vector2i(160, 120)),
            CanvasFreshness::CommitStalled);
  EXPECT_EQ(CanvasFreshnessStatusSuffix(CanvasFreshness::CommitStalled),
            "  \u2190 commit stalled vs desired");
}

TEST(LayerInspectorDiagnosticsTest, DetectsCompositorBehindDocumentCanvas) {
  EXPECT_EQ(ClassifyCanvasFreshness(Vector2i(640, 480), Vector2i(640, 480), Vector2i(320, 240)),
            CanvasFreshness::CompositorBehind);
  EXPECT_EQ(CanvasFreshnessStatusSuffix(CanvasFreshness::CompositorBehind),
            "  \u2190 compositor not yet re-rasterized");
}

}  // namespace
}  // namespace donner::editor
