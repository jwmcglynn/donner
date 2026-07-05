#include "donner/editor/FrameMissTelemetry.h"

#include <string>

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

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

  EXPECT_TRUE(BuildFrameMissTelemetryJson(input).empty());
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

  EXPECT_NE(json.find(R"("event":"frame_budget_miss")"), std::string::npos);
  EXPECT_NE(json.find(R"("frame":42)"), std::string::npos);
  EXPECT_NE(json.find(R"("miss":"missed_60hz")"), std::string::npos);
  EXPECT_NE(json.find(R"("known_ui_ms":10)"), std::string::npos);
  EXPECT_NE(json.find(R"("known_worker_ms":9)"), std::string::npos);
  EXPECT_NE(json.find(R"("other_ui_ms":12)"), std::string::npos);
  EXPECT_NE(json.find(R"({"name":"other","ms":12})"), std::string::npos);
  EXPECT_NE(json.find(R"({"name":"backend","ms":11})"), std::string::npos);
  EXPECT_NE(json.find(R"({"name":"rnd-cache","ms":9})"), std::string::npos);
  EXPECT_NE(json.find(R"("has_live_drag_preview":true)"), std::string::npos);
  EXPECT_NE(json.find(R"("cached_tiles":12)"), std::string::npos);
  EXPECT_NE(json.find(R"("document_canvas_commits":2)"), std::string::npos);
  EXPECT_NE(json.find(R"("total_tracked_bytes":67108864)"), std::string::npos);
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

  EXPECT_NE(json.find(R"("known_ui_ms":30)"), std::string::npos);
  EXPECT_NE(json.find(R"("other_ui_ms":10)"), std::string::npos);
  EXPECT_NE(json.find(R"({"name":"render-pane","ms":12})"), std::string::npos);
  EXPECT_NE(json.find(R"({"name":"host-present","ms":6})"), std::string::npos);
  EXPECT_NE(json.find(R"({"name":"host-direct","ms":3})"), std::string::npos);
  EXPECT_NE(json.find(R"({"name":"ui-misc","ms":2})"), std::string::npos);
  EXPECT_NE(json.find(R"("previous_end_frame_ms":10)"), std::string::npos);
  EXPECT_NE(json.find(R"("checkerboard_ms":8)"), std::string::npos);
  EXPECT_NE(json.find(R"("checkerboard_draws":1)"), std::string::npos);
  EXPECT_EQ(json.find(R"({"name":"overlay-capture","ms":20})"), std::string::npos)
      << "legacy nested renderer details must not double-count top-level UI frame time";
  EXPECT_EQ(json.find(R"({"name":"checkerboard","ms":8})"), std::string::npos)
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

  const std::size_t capturePos = json.find(R"({"name":"overlay-capture","ms":2})");
  const std::size_t drawPos = json.find(R"({"name":"overlay-draw","ms":2})");
  ASSERT_NE(capturePos, std::string::npos);
  ASSERT_NE(drawPos, std::string::npos);
  EXPECT_LT(capturePos, drawPos);

  EXPECT_NE(json.find(R"("payload_bytes":128)"), std::string::npos);
  EXPECT_NE(json.find(R"("selected_elements":3)"), std::string::npos);
  EXPECT_NE(json.find(R"("source_hover_elements":2)"), std::string::npos);
  EXPECT_NE(json.find(R"("paths":4)"), std::string::npos);
  EXPECT_NE(json.find(R"("hover_paths":1)"), std::string::npos);
  EXPECT_NE(json.find(R"("aabbs":5)"), std::string::npos);
  EXPECT_NE(json.find(R"("hover_aabbs":6)"), std::string::npos);
  EXPECT_NE(json.find(R"("handles":7)"), std::string::npos);
  EXPECT_NE(json.find(R"("has_marquee":true)"), std::string::npos);
  EXPECT_NE(json.find(R"("selection_bounds_only":true)"), std::string::npos);
  EXPECT_NE(json.find(R"("has_live_drag_preview":false)"), std::string::npos);
  EXPECT_NE(json.find(R"("has_represented_drag_preview":true)"), std::string::npos);
  EXPECT_NE(json.find(R"("live_drag_translation_doc":[1.5,-2.5])"), std::string::npos);
  EXPECT_NE(json.find(R"("represented_drag_translation_doc":[3,4])"), std::string::npos);
  EXPECT_NE(json.find(R"("canvas_size":[320,240])"), std::string::npos);
}

}  // namespace
}  // namespace donner::editor
