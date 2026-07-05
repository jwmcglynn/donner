#include "donner/editor/ViewportInteractionController.h"

#include <gtest/gtest.h>

namespace donner::editor {
namespace {

TEST(ViewportInteractionControllerTest, FrameHistoryTracksLatestAndMax) {
  ViewportInteractionController controller;

  controller.noteFrameDelta(10.0f);
  controller.noteFrameDelta(24.0f);
  controller.noteFrameDelta(12.0f);

  EXPECT_FLOAT_EQ(controller.frameHistory().latest(), 12.0f);
  EXPECT_FLOAT_EQ(controller.frameHistory().max(), 24.0f);
}

TEST(ViewportInteractionControllerTest, EmptyFrameHistoryAccessorsAndSettersAreNoOps) {
  ViewportInteractionController controller;
  FrameCostBreakdown cost;
  cost.mainFrame.layoutMs = 5.0;

  controller.frameHistory().setLatestBackendMs(8.0f);
  controller.frameHistory().setLatestFrameCost(cost);
  controller.frameHistory().setLatestMemorySample(FrameMemorySample{.totalTrackedBytes = 64u});

  EXPECT_FLOAT_EQ(controller.frameHistory().latest(), 0.0f);
  EXPECT_FLOAT_EQ(controller.frameHistory().latestBackend(), 0.0f);
  EXPECT_FLOAT_EQ(controller.frameHistory().max(), 0.0f);
  EXPECT_FLOAT_EQ(controller.frameHistory().lastBackendMs, 0.0f);
  EXPECT_EQ(controller.frameHistory().latestNonZeroMemorySample().totalTrackedBytes, 0u);
}

TEST(ViewportInteractionControllerTest, BackendSamplesResetPerFrameButKeepLastNonZeroLatency) {
  ViewportInteractionController controller;

  controller.noteFrameDelta(16.0f);
  controller.frameHistory().setLatestBackendMs(0.0f);
  EXPECT_FLOAT_EQ(controller.frameHistory().latestBackend(), 0.0f);
  EXPECT_FLOAT_EQ(controller.frameHistory().lastBackendMs, 0.0f);

  controller.frameHistory().setLatestBackendMs(7.5f);
  EXPECT_FLOAT_EQ(controller.frameHistory().latestBackend(), 7.5f);
  EXPECT_FLOAT_EQ(controller.frameHistory().lastBackendMs, 7.5f);

  controller.noteFrameDelta(17.0f);
  EXPECT_FLOAT_EQ(controller.frameHistory().latestBackend(), 0.0f);
  EXPECT_FLOAT_EQ(controller.frameHistory().lastBackendMs, 7.5f);
}

TEST(ViewportInteractionControllerTest, FrameHistoryTracksProfilerCostForLatestFrame) {
  ViewportInteractionController controller;

  controller.noteFrameDelta(16.0f);
  FrameCostBreakdown cost;
  cost.overlay.captureMs = 1.25;
  cost.overlay.drawMs = 2.5;
  cost.overlay.snapshotMs = 0.75;
  cost.overlay.uploadMs = 0.5;
  cost.compositedUpload.uploadMs = 1.0;
  cost.compositedRender.immediateMs = 1.75;
  cost.compositedRender.cachedMs = 2.25;
  cost.sourceRopes.layoutMs = 0.25;
  cost.sourceRopes.updateMs = 0.125;
  cost.sourceRopes.drawMs = 0.375;
  controller.frameHistory().setLatestFrameCost(cost);

  const std::size_t latestIdx =
      (controller.frameHistory().writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  const FrameProfilerSample& profiler = controller.frameHistory().profiler[latestIdx];
  EXPECT_FLOAT_EQ(profiler.overlayCaptureMs, 1.25f);
  EXPECT_FLOAT_EQ(profiler.overlayDrawMs, 2.5f);
  EXPECT_FLOAT_EQ(profiler.overlaySnapshotMs, 0.75f);
  EXPECT_FLOAT_EQ(profiler.overlayUploadMs, 0.5f);
  EXPECT_FLOAT_EQ(profiler.compositedUploadMs, 1.0f);
  EXPECT_FLOAT_EQ(profiler.compositedRenderImmediateMs, 1.75f);
  EXPECT_FLOAT_EQ(profiler.compositedRenderCachedMs, 2.25f);
  EXPECT_FLOAT_EQ(profiler.sourceRopeLayoutMs, 0.25f);
  EXPECT_FLOAT_EQ(profiler.sourceRopeUpdateMs, 0.125f);
  EXPECT_FLOAT_EQ(profiler.sourceRopeDrawMs, 0.375f);
  EXPECT_FLOAT_EQ(profiler.totalProfiledMs(), 6.75f);
}

TEST(ViewportInteractionControllerTest, TopLevelFrameCostOverridesNestedRendererDetails) {
  ViewportInteractionController controller;

  controller.noteFrameDelta(16.0f);
  FrameCostBreakdown cost;
  cost.hostFrame.beginFrameMs = 1.0;
  cost.hostFrame.previousEndFrameMs = 6.0;
  cost.mainFrame.renderPaneMs = 4.0;
  cost.mainFrame.sidebarsMs = 2.0;
  cost.overlay.captureMs = 100.0;
  cost.compositedRender.cachedMs = 50.0;
  controller.frameHistory().setLatestFrameCost(cost);

  const std::size_t latestIdx =
      (controller.frameHistory().writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  const FrameProfilerSample& profiler = controller.frameHistory().profiler[latestIdx];
  EXPECT_FLOAT_EQ(profiler.hostBeginFrameMs, 1.0f);
  EXPECT_FLOAT_EQ(profiler.hostPreviousEndFrameMs, 6.0f);
  EXPECT_FLOAT_EQ(profiler.mainRenderPaneMs, 4.0f);
  EXPECT_FLOAT_EQ(profiler.mainSidebarsMs, 2.0f);
  EXPECT_FLOAT_EQ(profiler.overlayCaptureMs, 100.0f);
  EXPECT_FLOAT_EQ(profiler.compositedRenderCachedMs, 50.0f);
  EXPECT_FLOAT_EQ(profiler.totalProfiledMs(), 13.0f);
}

TEST(ViewportInteractionControllerTest, NewFrameClearsProfilerCostSlot) {
  ViewportInteractionController controller;

  controller.noteFrameDelta(16.0f);
  FrameCostBreakdown cost;
  cost.overlay.captureMs = 1.0;
  controller.frameHistory().setLatestFrameCost(cost);
  controller.noteFrameDelta(17.0f);

  const std::size_t latestIdx =
      (controller.frameHistory().writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  EXPECT_FLOAT_EQ(controller.frameHistory().profiler[latestIdx].totalProfiledMs(), 0.0f);
}

TEST(ViewportInteractionControllerTest, FrameHistoryTracksPresentationMemoryForLatestFrame) {
  ViewportInteractionController controller;

  controller.noteFrameDelta(16.0f);
  const FrameMemorySample sample{
      .overlayBytes = 1u,
      .activeTileBytes = 2u,
      .overviewTileBytes = 3u,
      .retiredBytes = 4u,
      .totalTrackedBytes = 10u,
      .peakTrackedBytes = 12u,
      .wgpuLifetimeTextureCreates = 20u,
      .wgpuLifetimeBufferCreates = 30u,
  };
  controller.frameHistory().setLatestMemorySample(sample);

  const std::size_t latestIdx =
      (controller.frameHistory().writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  EXPECT_EQ(controller.frameHistory().memory[latestIdx].overlayBytes, 1u);
  EXPECT_EQ(controller.frameHistory().memory[latestIdx].activeTileBytes, 2u);
  EXPECT_EQ(controller.frameHistory().memory[latestIdx].overviewTileBytes, 3u);
  EXPECT_EQ(controller.frameHistory().memory[latestIdx].retiredBytes, 4u);
  EXPECT_EQ(controller.frameHistory().memory[latestIdx].totalTrackedBytes, 10u);
  EXPECT_EQ(controller.frameHistory().memory[latestIdx].peakTrackedBytes, 12u);
  EXPECT_EQ(controller.frameHistory().memory[latestIdx].wgpuLifetimeTextureCreates, 20u);
  EXPECT_EQ(controller.frameHistory().memory[latestIdx].wgpuLifetimeBufferCreates, 30u);
}

TEST(ViewportInteractionControllerTest, NewFrameClearsPresentationMemorySlot) {
  ViewportInteractionController controller;

  controller.noteFrameDelta(16.0f);
  controller.frameHistory().setLatestMemorySample(FrameMemorySample{
      .totalTrackedBytes = 10u,
      .peakTrackedBytes = 12u,
  });
  controller.noteFrameDelta(17.0f);

  const std::size_t latestIdx =
      (controller.frameHistory().writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  EXPECT_EQ(controller.frameHistory().memory[latestIdx].totalTrackedBytes, 0u);
  EXPECT_EQ(controller.frameHistory().memory[latestIdx].peakTrackedBytes, 0u);
}

TEST(ViewportInteractionControllerTest, LatestNonZeroMemorySampleSkipsFreshlyClearedSlot) {
  ViewportInteractionController controller;

  controller.noteFrameDelta(16.0f);
  controller.frameHistory().setLatestMemorySample(FrameMemorySample{
      .totalTrackedBytes = 10u,
      .peakTrackedBytes = 12u,
  });
  controller.noteFrameDelta(17.0f);

  const FrameMemorySample latest = controller.frameHistory().latestNonZeroMemorySample();

  EXPECT_EQ(latest.totalTrackedBytes, 10u);
  EXPECT_EQ(latest.peakTrackedBytes, 12u);
}

TEST(ViewportInteractionControllerTest, ApplyZoomUsesViewportCenterMath) {
  ViewportInteractionController controller;
  controller.viewport().paneOrigin = Vector2d::Zero();
  controller.viewport().paneSize = Vector2d(200.0, 200.0);
  controller.viewport().documentViewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0);
  EXPECT_TRUE(controller.resetToActualSize());

  const Vector2d focal(100.0, 100.0);
  const Vector2d documentBefore = controller.viewport().screenToDocument(focal);
  EXPECT_TRUE(controller.applyZoom(2.0, focal));
  const Vector2d documentAfter = controller.viewport().screenToDocument(focal);

  EXPECT_NEAR(documentBefore.x, documentAfter.x, 1e-9);
  EXPECT_NEAR(documentBefore.y, documentAfter.y, 1e-9);
}

TEST(ViewportInteractionControllerTest, PaneLayoutAndDevicePixelRatioUpdateState) {
  ViewportInteractionController controller;
  const Box2d initialDocumentViewBox = controller.viewport().documentViewBox;

  controller.updatePaneLayout(Vector2d(4.0, 5.0), Vector2d(300.0, 200.0), std::nullopt);
  EXPECT_EQ(controller.viewport().paneOrigin, Vector2d(4.0, 5.0));
  EXPECT_EQ(controller.viewport().paneSize, Vector2d(300.0, 200.0));
  EXPECT_EQ(controller.viewport().documentViewBox, initialDocumentViewBox);

  controller.updatePaneLayout(Vector2d(6.0, 7.0), Vector2d(400.0, 250.0),
                              Box2d::FromXYWH(10.0, 20.0, 30.0, 40.0));
  EXPECT_EQ(controller.viewport().paneOrigin, Vector2d(6.0, 7.0));
  EXPECT_EQ(controller.viewport().paneSize, Vector2d(400.0, 250.0));
  EXPECT_EQ(controller.viewport().documentViewBox, Box2d::FromXYWH(10.0, 20.0, 30.0, 40.0));

  controller.updateDevicePixelRatio(2.5);
  EXPECT_DOUBLE_EQ(controller.viewport().devicePixelRatio, 2.5);
}

TEST(ViewportInteractionControllerTest, MousePanReportsViewportChange) {
  ViewportInteractionController controller;
  controller.viewport().paneOrigin = Vector2d::Zero();
  controller.viewport().paneSize = Vector2d(200.0, 200.0);
  controller.viewport().documentViewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0);
  EXPECT_TRUE(controller.resetToActualSize());

  EXPECT_FALSE(controller.updatePanState(/*paneHovered=*/true, /*spaceHeld=*/true,
                                         /*middleDown=*/false, /*leftDown=*/true,
                                         ImVec2(10.0f, 10.0f)));
  EXPECT_TRUE(controller.updatePanState(/*paneHovered=*/true, /*spaceHeld=*/true,
                                        /*middleDown=*/false, /*leftDown=*/true,
                                        ImVec2(18.0f, 6.0f)));

  EXPECT_NEAR(controller.viewport().panScreenPoint.x, 108.0, 1e-9);
  EXPECT_NEAR(controller.viewport().panScreenPoint.y, 96.0, 1e-9);
}

TEST(ViewportInteractionControllerTest, MiddleMousePanStartsWithoutHoverOrSpaceAndStopsOnRelease) {
  ViewportInteractionController controller;
  controller.viewport().paneOrigin = Vector2d::Zero();
  controller.viewport().paneSize = Vector2d(200.0, 200.0);
  controller.viewport().documentViewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0);
  EXPECT_TRUE(controller.resetToActualSize());

  EXPECT_FALSE(controller.updatePanState(/*paneHovered=*/false, /*spaceHeld=*/false,
                                         /*middleDown=*/true, /*leftDown=*/false,
                                         ImVec2(25.0f, 25.0f)));
  EXPECT_TRUE(controller.panning());

  EXPECT_TRUE(controller.updatePanState(/*paneHovered=*/false, /*spaceHeld=*/false,
                                        /*middleDown=*/true, /*leftDown=*/false,
                                        ImVec2(30.0f, 35.0f)));
  EXPECT_TRUE(controller.panning());

  EXPECT_FALSE(controller.updatePanState(/*paneHovered=*/false, /*spaceHeld=*/false,
                                         /*middleDown=*/false, /*leftDown=*/false,
                                         ImVec2(30.0f, 35.0f)));
  EXPECT_FALSE(controller.panning());
}

TEST(ViewportInteractionControllerTest, PanCursorShowsForSpacePanModeOrActivePan) {
  EXPECT_TRUE(ShouldShowRenderPanePanCursor(/*canvasHovered=*/true, /*spaceHeld=*/true,
                                            /*panning=*/false));
  EXPECT_TRUE(ShouldShowRenderPanePanCursor(/*canvasHovered=*/false, /*spaceHeld=*/false,
                                            /*panning=*/true));
  EXPECT_FALSE(ShouldShowRenderPanePanCursor(/*canvasHovered=*/false, /*spaceHeld=*/true,
                                             /*panning=*/false));
  EXPECT_FALSE(ShouldShowRenderPanePanCursor(/*canvasHovered=*/true, /*spaceHeld=*/false,
                                             /*panning=*/false));
}

TEST(ViewportInteractionControllerTest, ConsumeScrollEventsAppliesPanAndZoom) {
  ViewportInteractionController controller;
  controller.viewport().paneOrigin = Vector2d::Zero();
  controller.viewport().paneSize = Vector2d(200.0, 200.0);
  controller.viewport().documentViewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0);
  EXPECT_TRUE(controller.resetToActualSize());

  std::vector<RenderPaneScrollEvent> panEvents = {
      RenderPaneScrollEvent{
          .scrollDelta = Vector2d(0.0, 2.0),
          .cursorScreen = Vector2d(100.0, 100.0),
          .zoomModifierHeld = false,
      },
  };

  const ScrollConsumptionResult panResult =
      controller.consumeScrollEvents(panEvents, Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0),
                                     /*modalCapturingInput=*/false, /*wheelZoomStep=*/1.1,
                                     /*panPixelsPerScrollUnit=*/10.0);
  EXPECT_TRUE(panResult.viewportChanged);
  EXPECT_FALSE(panResult.zoomChanged);

  EXPECT_TRUE(panEvents.empty());
  EXPECT_NEAR(controller.viewport().panScreenPoint.y, 120.0, 1e-9);

  std::vector<RenderPaneScrollEvent> zoomEvents = {
      RenderPaneScrollEvent{
          .scrollDelta = Vector2d(0.0, 1.0),
          .cursorScreen = Vector2d(100.0, 100.0),
          .zoomModifierHeld = true,
      },
  };

  const ScrollConsumptionResult zoomResult =
      controller.consumeScrollEvents(zoomEvents, Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0),
                                     /*modalCapturingInput=*/false, /*wheelZoomStep=*/1.1,
                                     /*panPixelsPerScrollUnit=*/10.0);
  EXPECT_TRUE(zoomResult.viewportChanged);
  EXPECT_TRUE(zoomResult.zoomChanged);

  EXPECT_TRUE(zoomEvents.empty());
  EXPECT_NEAR(controller.viewport().zoom, 1.1, 1e-9);
}

TEST(ViewportInteractionControllerTest, ConsumeScrollEventsReportsNoChangeWhenModalCapturesInput) {
  ViewportInteractionController controller;
  controller.viewport().paneOrigin = Vector2d::Zero();
  controller.viewport().paneSize = Vector2d(200.0, 200.0);
  controller.viewport().documentViewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0);
  EXPECT_TRUE(controller.resetToActualSize());

  std::vector<RenderPaneScrollEvent> panEvents = {
      RenderPaneScrollEvent{
          .scrollDelta = Vector2d(0.0, 2.0),
          .cursorScreen = Vector2d(100.0, 100.0),
          .zoomModifierHeld = false,
      },
  };

  const ScrollConsumptionResult result =
      controller.consumeScrollEvents(panEvents, Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0),
                                     /*modalCapturingInput=*/true, /*wheelZoomStep=*/1.1,
                                     /*panPixelsPerScrollUnit=*/10.0);
  EXPECT_FALSE(result.viewportChanged);
  EXPECT_FALSE(result.zoomChanged);

  EXPECT_TRUE(panEvents.empty());
  EXPECT_NEAR(controller.viewport().panScreenPoint.y, 100.0, 1e-9);
}

TEST(ViewportInteractionControllerTest, ConsumeScrollEventsClearsIgnoredEventsWhilePanning) {
  ViewportInteractionController controller;
  controller.viewport().paneOrigin = Vector2d::Zero();
  controller.viewport().paneSize = Vector2d(200.0, 200.0);
  controller.viewport().documentViewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0);
  EXPECT_TRUE(controller.resetToActualSize());
  ASSERT_FALSE(controller.updatePanState(/*paneHovered=*/false, /*spaceHeld=*/false,
                                         /*middleDown=*/true, /*leftDown=*/false,
                                         ImVec2(10.0f, 10.0f)));
  ASSERT_TRUE(controller.panning());

  std::vector<RenderPaneScrollEvent> events = {
      RenderPaneScrollEvent{
          .scrollDelta = Vector2d(0.0, 2.0),
          .cursorScreen = Vector2d(100.0, 100.0),
          .zoomModifierHeld = true,
      },
  };

  const ScrollConsumptionResult result =
      controller.consumeScrollEvents(events, Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0),
                                     /*modalCapturingInput=*/false, /*wheelZoomStep=*/1.1,
                                     /*panPixelsPerScrollUnit=*/10.0);

  EXPECT_FALSE(result.viewportChanged);
  EXPECT_FALSE(result.zoomChanged);
  EXPECT_TRUE(events.empty());
}

TEST(ViewportInteractionControllerTest, PendingClickBuffersAndClears) {
  ViewportInteractionController controller;
  MouseModifiers modifiers;
  modifiers.shift = true;

  controller.bufferPendingClick(Vector2d(12.0, 34.0), modifiers);
  ASSERT_TRUE(controller.pendingClick().has_value());
  EXPECT_DOUBLE_EQ(controller.pendingClick()->documentPoint.x, 12.0);
  EXPECT_DOUBLE_EQ(controller.pendingClick()->documentPoint.y, 34.0);
  EXPECT_TRUE(controller.pendingClick()->modifiers.shift);

  controller.clearPendingClick();
  EXPECT_FALSE(controller.pendingClick().has_value());
}

TEST(ViewportStateTest, ZeroZoomScreenToDocumentFallsBackToAnchor) {
  ViewportState viewport;
  viewport.zoom = 0.0;
  viewport.panDocPoint = Vector2d(12.0, 34.0);
  viewport.panScreenPoint = Vector2d(100.0, 200.0);

  EXPECT_EQ(viewport.screenToDocument(Vector2d(300.0, 400.0)), Vector2d(12.0, 34.0));

  const Box2d documentBox = viewport.screenToDocument(Box2d::FromXYWH(1.0, 2.0, 3.0, 4.0));
  EXPECT_EQ(documentBox.topLeft, Vector2d(12.0, 34.0));
  EXPECT_EQ(documentBox.bottomRight, Vector2d(12.0, 34.0));
}

TEST(ViewportStateTest, RasterViewportClampsDegenerateDocumentAxis) {
  ViewportState viewport;
  viewport.documentViewBox = Box2d::FromXYWH(10.0, 20.0, 0.0, 50.0);
  viewport.zoom = 2.0;
  viewport.devicePixelRatio = 2.0;
  viewport.paneSize = Vector2d(100.0, 100.0);

  const EditorRasterViewport raster = viewport.rasterViewport();

  EXPECT_EQ(raster.semanticCanvasSizePx, Vector2i(1, 200));
  EXPECT_EQ(raster.outputSizePx, Vector2i(1, 200));
  EXPECT_FALSE(raster.viewportBounded);
}

TEST(ViewportStateTest, RasterViewportDoesNotViewportBoundWithoutUsablePane) {
  struct Case {
    Vector2d paneSize;
    double devicePixelRatio;
  };

  constexpr Case kCases[] = {
      {Vector2d(0.0, 100.0), 1.0},
      {Vector2d(100.0, 0.0), 1.0},
      {Vector2d(100.0, 100.0), 0.0},
  };

  for (const Case& testCase : kCases) {
    ViewportState viewport;
    viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 1000.0, 1000.0);
    viewport.zoom = 20.0;
    viewport.devicePixelRatio = testCase.devicePixelRatio;
    viewport.paneSize = testCase.paneSize;

    const EditorRasterViewport raster = viewport.rasterViewport();

    EXPECT_FALSE(raster.viewportBounded);
  }
}

TEST(ViewportStateTest, RasterViewportBoundsTallDocumentTargets) {
  ViewportState viewport;
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 100.0, 1000.0);
  viewport.zoom = 10.0;
  viewport.devicePixelRatio = 1.0;
  viewport.paneOrigin = Vector2d(10.0, 20.0);
  viewport.paneSize = Vector2d(200.0, 120.0);
  viewport.resetTo100Percent();
  viewport.zoomAround(10.0, viewport.paneCenter());

  const EditorRasterViewport raster = viewport.rasterViewport();

  EXPECT_TRUE(raster.viewportBounded);
  EXPECT_EQ(raster.outputSizePx, Vector2i(200 + 2 * ViewportState::kHighZoomRasterMarginScreenPx,
                                          120 + 2 * ViewportState::kHighZoomRasterMarginScreenPx));
}

TEST(ViewportStateTest, SelectedPrewarmExpandsViewportBoundedRaster) {
  ViewportState viewport;
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 2000.0, 2000.0);
  viewport.zoom = 12.0;
  viewport.devicePixelRatio = 1.0;
  viewport.paneOrigin = Vector2d(50.0, 60.0);
  viewport.paneSize = Vector2d(100.0, 80.0);
  viewport.resetTo100Percent();
  viewport.zoomAround(12.0, viewport.paneCenter());

  const EditorRasterViewport base = viewport.rasterViewport();
  const EditorRasterViewport prewarm = viewport.selectedPrewarmRasterViewport();

  ASSERT_TRUE(base.viewportBounded);
  EXPECT_TRUE(prewarm.viewportBounded);
  EXPECT_GT(prewarm.outputSizePx.x, base.outputSizePx.x);
  EXPECT_GT(prewarm.outputSizePx.y, base.outputSizePx.y);
}

TEST(ViewportStateTest, SelectedPrewarmKeepsMaxSizedViewportBoundedRaster) {
  ViewportState viewport;
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 100000.0, 100000.0);
  viewport.zoom = 1.0;
  viewport.devicePixelRatio = 1.0;
  viewport.paneSize = Vector2d(10000.0, 10000.0);

  const EditorRasterViewport base = viewport.rasterViewport();
  const EditorRasterViewport prewarm = viewport.selectedPrewarmRasterViewport();

  ASSERT_TRUE(base.viewportBounded);
  EXPECT_EQ(base.outputSizePx,
            Vector2i(ViewportState::kMaxCanvasDim, ViewportState::kMaxCanvasDim));
  EXPECT_EQ(prewarm.outputSizePx, base.outputSizePx);
  EXPECT_EQ(prewarm.documentRect, base.documentRect);
}

TEST(ViewportStateTest, SelectedPrewarmKeepsViewportBoundedRasterWhenScaleIsNonPositive) {
  ViewportState viewport;
  viewport.documentViewBox = Box2d(Vector2d(1000.0, 1000.0), Vector2d(0.0, 0.0));
  viewport.zoom = -10.0;
  viewport.devicePixelRatio = 1.0;
  viewport.paneSize = Vector2d(100.0, 100.0);

  const EditorRasterViewport base = viewport.rasterViewport();
  const EditorRasterViewport prewarm = viewport.selectedPrewarmRasterViewport();

  ASSERT_TRUE(base.viewportBounded);
  EXPECT_EQ(prewarm.outputSizePx, base.outputSizePx);
  EXPECT_EQ(prewarm.documentRect, base.documentRect);
}

TEST(ViewportStateTest, OverviewInfillReturnsBaseForDegenerateInputs) {
  ViewportState zeroDocument;
  zeroDocument.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 0.0, 0.0);
  zeroDocument.zoom = 4.0;
  zeroDocument.devicePixelRatio = 2.0;
  EXPECT_EQ(zeroDocument.overviewInfillRasterViewport().outputSizePx,
            zeroDocument.rasterViewport().outputSizePx);

  ViewportState zeroDpr;
  zeroDpr.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0);
  zeroDpr.zoom = 4.0;
  zeroDpr.devicePixelRatio = 0.0;
  EXPECT_EQ(zeroDpr.overviewInfillRasterViewport().outputSizePx,
            zeroDpr.rasterViewport().outputSizePx);
}

TEST(ViewportStateTest, OverviewInfillUsesFullDocumentAtCappedScale) {
  ViewportState viewport;
  viewport.documentViewBox = Box2d::FromXYWH(10.0, 20.0, 5000.0, 1000.0);
  viewport.zoom = 20.0;
  viewport.devicePixelRatio = 2.0;
  viewport.paneSize = Vector2d(200.0, 200.0);

  const EditorRasterViewport overview = viewport.overviewInfillRasterViewport();

  EXPECT_FALSE(overview.viewportBounded);
  EXPECT_EQ(overview.documentRect, viewport.documentViewBox);
  EXPECT_EQ(overview.outputSizePx, Vector2i(ViewportState::kOverviewInfillMaxCanvasDim, 307));
}

}  // namespace
}  // namespace donner::editor
