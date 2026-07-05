#include "donner/editor/FrameMissTelemetry.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

namespace donner::editor {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

TEST(FrameMissTelemetryTest, ClassifiesBudgets) {
  EXPECT_EQ(ClassifyFrameBudgetMiss(8.0), FrameBudgetMiss::WithinBudget);
  EXPECT_EQ(ClassifyFrameBudgetMiss(10.0), FrameBudgetMiss::Missed120Hz);
  EXPECT_EQ(ClassifyFrameBudgetMiss(20.0), FrameBudgetMiss::Missed60Hz);
}

TEST(FrameMissTelemetryTest, NamesBudgetMissClassifications) {
  EXPECT_STREQ(FrameBudgetMissName(FrameBudgetMiss::WithinBudget), "within_budget");
  EXPECT_STREQ(FrameBudgetMissName(FrameBudgetMiss::Missed120Hz), "missed_120hz");
  EXPECT_STREQ(FrameBudgetMissName(FrameBudgetMiss::Missed60Hz), "missed_60hz");
  EXPECT_STREQ(FrameBudgetMissName(static_cast<FrameBudgetMiss>(255)), "unknown");
}

TEST(FrameMissTelemetryTest, DoesNotSerializeFramesWithinBudget) {
  FrameMissTelemetryInput input;
  input.frameIndex = 4;
  input.frameMs = 8.0;

  EXPECT_THAT(BuildFrameMissTelemetryJson(input), IsEmpty());
}

TEST(FrameMissTelemetryTest, SerializesMissWithContributorBreakdown) {
  FrameMissTelemetryInput input;
  input.frameIndex = 42;
  input.frameMs = 22.0;
  input.backendMs = 11.0;
  input.frameCost.overlay.drawMs = 4.0;
  input.frameCost.overlay.snapshotMs = 1.0;
  input.frameCost.overlay.payloadBytes = 4096;
  input.frameCost.overlay.hasLiveDragPreview = true;
  input.frameCost.compositedRender.cachedMs = 9.0;
  input.frameCost.compositedRender.cachedTileCount = 12;
  input.frameCost.compositedUpload.uploadMs = 2.0;
  input.frameCost.compositedUpload.payloadBytes = 8192;
  input.frameCost.sourceRopes.drawMs = 3.0;
  input.frameCost.documentCanvasCommitCount = 2;
  input.resources.totalTrackedBytes = 64u * 1024u * 1024u;
  input.resources.peakTrackedBytes = 96u * 1024u * 1024u;
  input.resources.wgpuLifetimeTextureCreates = 17;

  const std::string json = BuildFrameMissTelemetryJson(input);

  EXPECT_THAT(json, HasSubstr(R"("event":"frame_budget_miss")"));
  EXPECT_THAT(json, HasSubstr(R"("frame":42)"));
  EXPECT_THAT(json, HasSubstr(R"("miss":"missed_60hz")"));
  EXPECT_THAT(json, HasSubstr(R"("known_ui_ms":10)"));
  EXPECT_THAT(json, HasSubstr(R"("known_worker_ms":9)"));
  EXPECT_THAT(json, HasSubstr(R"("other_ui_ms":12)"));
  EXPECT_THAT(json, HasSubstr(R"({"name":"other","ms":12})"));
  EXPECT_THAT(json, HasSubstr(R"({"name":"backend","ms":11})"));
  EXPECT_THAT(json, HasSubstr(R"({"name":"rnd-cache","ms":9})"));
  EXPECT_THAT(json, HasSubstr(R"("has_live_drag_preview":true)"));
  EXPECT_THAT(json, HasSubstr(R"("cached_tiles":12)"));
  EXPECT_THAT(json, HasSubstr(R"("document_canvas_commits":2)"));
  EXPECT_THAT(json, HasSubstr(R"("total_tracked_bytes":67108864)"));
  EXPECT_EQ(json.back(), '\n');
}

TEST(FrameMissTelemetryTest, TopLevelFrameCostsReplaceLegacyOther) {
  FrameMissTelemetryInput input;
  input.frameIndex = 7;
  input.frameMs = 40.0;
  input.frameCost.hostFrame.beginFrameMs = 1.0;
  input.frameCost.hostFrame.previousEndFrameMs = 10.0;
  input.frameCost.hostFrame.previousSurfaceAcquireMs = 2.0;
  input.frameCost.hostFrame.previousDirectMs = 3.0;
  input.frameCost.hostFrame.previousPresentMs = 4.0;
  input.frameCost.mainFrame.renderPaneMs = 12.0;
  input.frameCost.mainFrame.sidebarsMs = 5.0;
  input.frameCost.mainFrame.documentSyncMs = 2.0;
  input.frameCost.overlay.captureMs = 20.0;
  input.frameCost.directPresentation.checkerboardMs = 8.0;
  input.frameCost.directPresentation.checkerboardDrawCount = 1;

  const std::string json = BuildFrameMissTelemetryJson(input);

  EXPECT_THAT(json, HasSubstr(R"("known_ui_ms":30)"));
  EXPECT_THAT(json, HasSubstr(R"("other_ui_ms":10)"));
  EXPECT_THAT(json, HasSubstr(R"({"name":"render-pane","ms":12})"));
  EXPECT_THAT(json, HasSubstr(R"({"name":"host-present","ms":6})"));
  EXPECT_THAT(json, HasSubstr(R"({"name":"host-direct","ms":3})"));
  EXPECT_THAT(json, HasSubstr(R"({"name":"ui-misc","ms":2})"));
  EXPECT_THAT(json, HasSubstr(R"("previous_end_frame_ms":10)"));
  EXPECT_THAT(json, HasSubstr(R"("checkerboard_ms":8)"));
  EXPECT_THAT(json, HasSubstr(R"("checkerboard_draws":1)"));
  EXPECT_THAT(json, Not(HasSubstr(R"({"name":"overlay-capture","ms":20})")))
      << "legacy nested renderer details must not double-count top-level UI frame time";
  EXPECT_THAT(json, Not(HasSubstr(R"({"name":"checkerboard","ms":8})")))
      << "nested direct-presentation details must not double-count host underlay time";
}

TEST(FrameMissTelemetryTest, SerializesOverlayStateAndOrdersTiedContributorsByName) {
  FrameMissTelemetryInput input;
  input.frameIndex = 51;
  input.frameMs = 25.0;
  input.frameCost.overlay.captureMs = 2.0;
  input.frameCost.overlay.drawMs = 2.0;
  input.frameCost.overlay.uploadMs = 1.0;
  input.frameCost.overlay.payloadBytes = 128;
  input.frameCost.overlay.selectedElementCount = 3;
  input.frameCost.overlay.sourceHoverElementCount = 2;
  input.frameCost.overlay.pathCount = 4;
  input.frameCost.overlay.hoverPathCount = 1;
  input.frameCost.overlay.aabbCount = 5;
  input.frameCost.overlay.hoverAabbCount = 6;
  input.frameCost.overlay.handleCount = 7;
  input.frameCost.overlay.hasMarquee = true;
  input.frameCost.overlay.selectionBoundsOnly = true;
  input.frameCost.overlay.hasRepresentedDragPreview = true;
  input.frameCost.overlay.liveDragTranslationDoc = Vector2d(1.5, -2.5);
  input.frameCost.overlay.representedDragTranslationDoc = Vector2d(3.0, 4.0);
  input.frameCost.overlay.canvasSize = Vector2i(320, 240);

  const std::string json = BuildFrameMissTelemetryJson(input);

  EXPECT_THAT(json, HasSubstr(R"({"name":"overlay-capture","ms":2})"));
  EXPECT_THAT(json, HasSubstr(R"({"name":"overlay-draw","ms":2})"));
  const std::size_t capturePos = json.find(R"({"name":"overlay-capture","ms":2})");
  const std::size_t drawPos = json.find(R"({"name":"overlay-draw","ms":2})");
  ASSERT_NE(capturePos, std::string::npos);
  ASSERT_NE(drawPos, std::string::npos);
  EXPECT_LT(capturePos, drawPos);

  EXPECT_THAT(json, HasSubstr(R"("payload_bytes":128)"));
  EXPECT_THAT(json, HasSubstr(R"("selected_elements":3)"));
  EXPECT_THAT(json, HasSubstr(R"("source_hover_elements":2)"));
  EXPECT_THAT(json, HasSubstr(R"("paths":4)"));
  EXPECT_THAT(json, HasSubstr(R"("hover_paths":1)"));
  EXPECT_THAT(json, HasSubstr(R"("aabbs":5)"));
  EXPECT_THAT(json, HasSubstr(R"("hover_aabbs":6)"));
  EXPECT_THAT(json, HasSubstr(R"("handles":7)"));
  EXPECT_THAT(json, HasSubstr(R"("has_marquee":true)"));
  EXPECT_THAT(json, HasSubstr(R"("selection_bounds_only":true)"));
  EXPECT_THAT(json, HasSubstr(R"("has_live_drag_preview":false)"));
  EXPECT_THAT(json, HasSubstr(R"("has_represented_drag_preview":true)"));
  EXPECT_THAT(json, HasSubstr(R"("live_drag_translation_doc":[1.5,-2.5])"));
  EXPECT_THAT(json, HasSubstr(R"("represented_drag_translation_doc":[3,4])"));
  EXPECT_THAT(json, HasSubstr(R"("canvas_size":[320,240])"));
}

}  // namespace
}  // namespace donner::editor
