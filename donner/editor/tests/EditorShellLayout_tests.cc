#include "donner/editor/EditorShellLayout.h"

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

TEST(EditorShellLayoutTest, UsesDesktopChromeForUnconstrainedMouseViewport) {
  const EditorAdaptiveUiLayout layout = ComputeEditorAdaptiveUiLayout({
      .windowWidth = 1440.0f,
      .windowHeight = 900.0f,
      .preferTouch = false,
  });

  EXPECT_EQ(layout.mode, EditorUiMode::Desktop);
  EXPECT_FLOAT_EQ(layout.topBarHeight, 0.0f);
  EXPECT_FLOAT_EQ(layout.toolButtonSize, 32.0f);
  EXPECT_TRUE(layout.showPaintControls);
  EXPECT_TRUE(layout.showTextFormatBar);
  EXPECT_TRUE(layout.showCanvasScrollbars);
}

TEST(EditorShellLayoutTest, UsesRightTouchSheetForLandscapeViewport) {
  const EditorAdaptiveUiLayout layout = ComputeEditorAdaptiveUiLayout({
      .windowWidth = 844.0f,
      .windowHeight = 390.0f,
      .preferTouch = true,
  });

  EXPECT_EQ(layout.mode, EditorUiMode::CompactTouch);
  EXPECT_EQ(layout.panelPlacement, CompactPanelPlacement::Right);
  EXPECT_FLOAT_EQ(layout.topBarHeight, 52.0f);
  EXPECT_FLOAT_EQ(layout.toolButtonSize, 44.0f);
  EXPECT_FLOAT_EQ(layout.panelX, 489.52f);
  EXPECT_FLOAT_EQ(layout.panelY, 52.0f);
  EXPECT_FLOAT_EQ(layout.panelWidth, 354.48f);
  EXPECT_FLOAT_EQ(layout.panelHeight, 338.0f);
  EXPECT_FALSE(layout.showPaintControls);
  EXPECT_FALSE(layout.showTextFormatBar);
  EXPECT_FALSE(layout.showCanvasScrollbars);
}

TEST(EditorShellLayoutTest, UsesBottomTouchSheetForPortraitViewport) {
  const EditorAdaptiveUiLayout layout = ComputeEditorAdaptiveUiLayout({
      .windowWidth = 390.0f,
      .windowHeight = 844.0f,
      .preferTouch = true,
  });

  EXPECT_EQ(layout.mode, EditorUiMode::CompactTouch);
  EXPECT_EQ(layout.panelPlacement, CompactPanelPlacement::Bottom);
  EXPECT_FLOAT_EQ(layout.panelX, 0.0f);
  EXPECT_FLOAT_EQ(layout.panelY, 489.52f);
  EXPECT_FLOAT_EQ(layout.panelWidth, 390.0f);
  EXPECT_FLOAT_EQ(layout.panelHeight, 354.48f);
}

TEST(EditorShellLayoutTest, ConstrainedMouseViewportStillUsesCompactChrome) {
  const EditorAdaptiveUiLayout layout = ComputeEditorAdaptiveUiLayout({
      .windowWidth = 700.0f,
      .windowHeight = 700.0f,
      .preferTouch = false,
  });

  EXPECT_EQ(layout.mode, EditorUiMode::CompactTouch);
}

RightSidebarLayoutInput DefaultLayoutInput() {
  return RightSidebarLayoutInput{
      .paneOriginY = 20.0f,
      .paneHeight = 1000.0f,
      .rightPaneGap = 8.0f,
      .treeViewHeightFraction = 0.33f,
      .layerPanelHeightFraction = 0.5f,
      .layerPanelSplitterThickness = 6.0f,
      .minLayerPanelHeight = 140.0f,
      .minInspectorPaneHeight = 96.0f,
  };
}

TEST(EditorShellLayoutTest, SplitsInspectorAndLayerPanelWithResizableBudget) {
  const RightSidebarLayout layout = ComputeRightSidebarLayout(DefaultLayoutInput());

  EXPECT_FLOAT_EQ(layout.treePaneHeight, 330.0f);
  EXPECT_FLOAT_EQ(layout.lowerPaneHeight, 656.0f);
  EXPECT_FLOAT_EQ(layout.inspectorPaneY, 358.0f);
  EXPECT_FLOAT_EQ(layout.inspectorPaneHeight, 328.0f);
  EXPECT_FLOAT_EQ(layout.layerPanelSplitterY, 686.0f);
  EXPECT_FLOAT_EQ(layout.layerPanelPaneY, 692.0f);
  EXPECT_FLOAT_EQ(layout.layerPanelHeight, 328.0f);
  EXPECT_FLOAT_EQ(layout.layerPanelHeightFraction, 0.5f);
}

TEST(EditorShellLayoutTest, MainLayoutUsesSourcePaneWidthWhenVisible) {
  const EditorMainPaneLayout layout = ComputeEditorMainPaneLayout({
      .windowWidth = 1600.0f,
      .sourcePaneVisible = true,
      .sourcePaneWidth = 560.0f,
      .minSourcePaneWidth = 240.0f,
      .maxSourcePaneWidth = 900.0f,
      .sourcePaneRailWidth = 32.0f,
      .rightPaneWidth = 420.0f,
      .minRightPaneWidth = 220.0f,
      .maxRightPaneWidth = 900.0f,
      .minRenderPaneWidth = 220.0f,
  });

  EXPECT_FLOAT_EQ(layout.sourcePaneWidth, 560.0f);
  EXPECT_FLOAT_EQ(layout.renderPaneX, 560.0f);
  EXPECT_FLOAT_EQ(layout.renderPaneWidth, 620.0f);
  EXPECT_FLOAT_EQ(layout.rightPaneX, 1180.0f);
  EXPECT_FLOAT_EQ(layout.rightPaneWidth, 420.0f);
}

TEST(EditorShellLayoutTest, MainLayoutGivesSourceWidthBackToRenderPaneWhenHidden) {
  const EditorMainPaneLayout layout = ComputeEditorMainPaneLayout({
      .windowWidth = 1600.0f,
      .sourcePaneVisible = false,
      .sourcePaneWidth = 560.0f,
      .minSourcePaneWidth = 240.0f,
      .maxSourcePaneWidth = 900.0f,
      .sourcePaneRailWidth = 32.0f,
      .rightPaneWidth = 420.0f,
      .minRightPaneWidth = 220.0f,
      .maxRightPaneWidth = 900.0f,
      .minRenderPaneWidth = 220.0f,
  });

  EXPECT_FLOAT_EQ(layout.sourcePaneWidth, 0.0f);
  EXPECT_FLOAT_EQ(layout.sourcePaneRailWidth, 32.0f);
  EXPECT_FLOAT_EQ(layout.renderPaneX, 32.0f);
  EXPECT_FLOAT_EQ(layout.renderPaneWidth, 1148.0f);
  EXPECT_FLOAT_EQ(layout.rightPaneX, 1180.0f);
  EXPECT_FLOAT_EQ(layout.rightPaneWidth, 420.0f);
}

TEST(EditorShellLayoutTest, MainLayoutGivesFullWidthToCompactCanvas) {
  const EditorMainPaneLayout layout = ComputeEditorMainPaneLayout({
      .windowWidth = 390.0f,
      .sourcePaneVisible = false,
      .sourcePaneWidth = 560.0f,
      .minSourcePaneWidth = 240.0f,
      .maxSourcePaneWidth = 900.0f,
      .sourcePaneRailWidth = 0.0f,
      .rightPaneWidth = 420.0f,
      .rightPaneVisible = false,
      .minRightPaneWidth = 220.0f,
      .maxRightPaneWidth = 900.0f,
      .minRenderPaneWidth = 220.0f,
  });

  EXPECT_FLOAT_EQ(layout.sourcePaneWidth, 0.0f);
  EXPECT_FLOAT_EQ(layout.sourcePaneRailWidth, 0.0f);
  EXPECT_FLOAT_EQ(layout.renderPaneX, 0.0f);
  EXPECT_FLOAT_EQ(layout.renderPaneWidth, 390.0f);
  EXPECT_FLOAT_EQ(layout.rightPaneWidth, 0.0f);
}

TEST(EditorShellLayoutTest, MainLayoutClampsSourcePaneWidthWhenVisible) {
  const EditorMainPaneLayout layout = ComputeEditorMainPaneLayout({
      .windowWidth = 1600.0f,
      .sourcePaneVisible = true,
      .sourcePaneWidth = 80.0f,
      .minSourcePaneWidth = 240.0f,
      .maxSourcePaneWidth = 900.0f,
      .rightPaneWidth = 420.0f,
      .minRightPaneWidth = 220.0f,
      .maxRightPaneWidth = 900.0f,
      .minRenderPaneWidth = 220.0f,
  });

  EXPECT_FLOAT_EQ(layout.sourcePaneWidth, 240.0f);
  EXPECT_FLOAT_EQ(layout.renderPaneX, 240.0f);
  EXPECT_FLOAT_EQ(layout.renderPaneWidth, 940.0f);
  EXPECT_FLOAT_EQ(layout.rightPaneX, 1180.0f);
  EXPECT_FLOAT_EQ(layout.rightPaneWidth, 420.0f);
}

TEST(EditorShellLayoutTest, PreservesInspectorMinimumWhenLayerPanelIsExpanded) {
  RightSidebarLayoutInput input = DefaultLayoutInput();
  input.layerPanelHeightFraction = 1.0f;

  const RightSidebarLayout layout = ComputeRightSidebarLayout(input);

  EXPECT_FLOAT_EQ(layout.inspectorPaneHeight, 96.0f);
  EXPECT_FLOAT_EQ(layout.layerPanelHeight, 560.0f);
  EXPECT_FLOAT_EQ(layout.maxLayerPanelHeight, 560.0f);
}

TEST(EditorShellLayoutTest, DetachedLayerPanelLetsInspectorUseLowerPane) {
  RightSidebarLayoutInput input = DefaultLayoutInput();
  input.layerPanelDetached = true;

  const RightSidebarLayout layout = ComputeRightSidebarLayout(input);

  EXPECT_FLOAT_EQ(layout.inspectorPaneHeight, 662.0f);
  EXPECT_FLOAT_EQ(layout.layerPanelHeight, 0.0f);
  EXPECT_FLOAT_EQ(layout.layerPanelSplitterY, 1020.0f);
}

TEST(EditorShellLayoutTest, DraggingSplitterUpExpandsLayerPanel) {
  const float nextFraction = ResizeLayerPanelHeightFraction(0.5f, 600.0f, 120.0f, 500.0f, -150.0f);

  EXPECT_FLOAT_EQ(nextFraction, 0.75f);
}

TEST(EditorShellLayoutTest, DraggingSplitterDownPreservesLayerMinimum) {
  const float nextFraction = ResizeLayerPanelHeightFraction(0.5f, 600.0f, 120.0f, 500.0f, 500.0f);

  EXPECT_FLOAT_EQ(nextFraction, 0.2f);
}


// The render pane geometry of a settled desktop frame: a 1280x699 dock host below the menu bar,
// split into a 947-wide canvas column and a right sidebar column, with the docked pane window
// filling the central node and its content region inset by ImGui's window padding.
RenderPaneLatchInput SettledDesktopFrame() {
  const LayoutRect host{.x = 32.0f, .y = 21.0f, .width = 1248.0f, .height = 699.0f};
  const LayoutRect centralNode{.x = 32.0f, .y = 21.0f, .width = 947.0f, .height = 699.0f};
  return RenderPaneLatchInput{
      .paneWindow = centralNode,
      .dockCentralNode = centralNode,
      .dockHost = host,
      .previousDockHost = host,
      .paneContentWidth = centralNode.width - 16.0f,
      .paneContentHeight = centralNode.height - 16.0f,
      .previousPaneContentWidth = -1.0f,
      .previousPaneContentHeight = -1.0f,
      .sidebarColumnIncluded = true,
  };
}

TEST(EditorShellLayoutTest, LatchesSettledDockedRenderPaneOnItsFirstFrame) {
  // No content-region history at all: the geometry alone proves the pane is settled, so a document
  // loaded into an already-running editor never spends a frame at the placeholder fit.
  EXPECT_TRUE(RenderPaneViewportLatchReady(SettledDesktopFrame()));
}

TEST(EditorShellLayoutTest, RejectsRenderPaneBeforeTheSidebarColumnIsSplitOff) {
  // Cold start: the freshly docked pane still spans the whole host because the right column has
  // not been split off yet. It fits inside the central node (they are the same rect), which is
  // exactly why a fits-within test accepts it, and it must still be rejected.
  RenderPaneLatchInput input = SettledDesktopFrame();
  input.dockCentralNode.width = input.dockHost.width;
  input.paneWindow = input.dockCentralNode;
  input.paneContentWidth = input.dockCentralNode.width - 16.0f;

  EXPECT_FALSE(RenderPaneViewportLatchReady(input));
}

TEST(EditorShellLayoutTest, RejectsRenderPaneShorterThanItsDockHost) {
  // The pre-settle rectangle that clips the document and presents the placeholder viewport is
  // *smaller* than the settled one, so it passes any "fits inside the central node" test. It
  // cannot pass this one: the shell computed the host height itself this frame.
  RenderPaneLatchInput input = SettledDesktopFrame();
  input.dockCentralNode.height = 446.0f;
  input.paneWindow = input.dockCentralNode;
  input.paneContentHeight = input.dockCentralNode.height - 16.0f;

  EXPECT_FALSE(RenderPaneViewportLatchReady(input));
}

TEST(EditorShellLayoutTest, RejectsRenderPaneWindowThatHasNotAdoptedTheCentralNode) {
  // ImGui has resized the node but the docked window still reports the previous rect. Rejected
  // whether that stale rect is larger or smaller than the node.
  RenderPaneLatchInput smaller = SettledDesktopFrame();
  smaller.paneWindow.width -= 120.0f;
  smaller.paneContentWidth = smaller.paneWindow.width - 16.0f;
  EXPECT_FALSE(RenderPaneViewportLatchReady(smaller));

  RenderPaneLatchInput larger = SettledDesktopFrame();
  larger.paneWindow.width += 120.0f;
  larger.paneContentWidth = larger.paneWindow.width - 16.0f;
  EXPECT_FALSE(RenderPaneViewportLatchReady(larger));
}

TEST(EditorShellLayoutTest, RejectsRenderPaneWhileTheDockHostIsStillMoving) {
  // Nothing downstream of a host that moved this frame is settled, no matter how self-consistent
  // ImGui's own report is.
  RenderPaneLatchInput input = SettledDesktopFrame();
  input.previousDockHost.height += 66.0f;

  EXPECT_FALSE(RenderPaneViewportLatchReady(input));
}

TEST(EditorShellLayoutTest, RejectsRepeatedContentRegionWhileTheDockHostIsStillMoving) {
  // The two-frame content-region fallback would otherwise pair a pre-change frame with a
  // post-change one and latch a rectangle that is about to be replaced.
  RenderPaneLatchInput input = SettledDesktopFrame();
  input.dockCentralNode = LayoutRect{};
  input.paneWindow = LayoutRect{};
  input.previousPaneContentWidth = input.paneContentWidth;
  input.previousPaneContentHeight = input.paneContentHeight;
  input.previousDockHost.width += 240.0f;

  EXPECT_FALSE(RenderPaneViewportLatchReady(input));
}

TEST(EditorShellLayoutTest, FallsBackToTwoStableFramesForAnUnrecognizedDockTree) {
  // A customized dock tree restored from the persisted layout need not put the canvas in a plain
  // column split; the pane must still be able to latch, just not on its first frame.
  RenderPaneLatchInput input = SettledDesktopFrame();
  input.dockCentralNode.y += 40.0f;
  input.paneWindow = input.dockCentralNode;
  EXPECT_FALSE(RenderPaneViewportLatchReady(input));

  input.previousPaneContentWidth = input.paneContentWidth;
  input.previousPaneContentHeight = input.paneContentHeight;
  EXPECT_TRUE(RenderPaneViewportLatchReady(input));
}

TEST(EditorShellLayoutTest, LatchesCompactCanvasNodeThatSpansItsHost) {
  // The compact-touch profile docks the canvas into the root node, so there is no column to split
  // off and the node legitimately spans the host.
  RenderPaneLatchInput input = SettledDesktopFrame();
  input.sidebarColumnIncluded = false;
  input.dockCentralNode.width = input.dockHost.width;
  input.paneWindow = input.dockCentralNode;
  input.paneContentWidth = input.dockCentralNode.width - 16.0f;

  EXPECT_TRUE(RenderPaneViewportLatchReady(input));
}

TEST(EditorShellLayoutTest, RejectsRenderPaneBeforeTheHostHasAnySize) {
  RenderPaneLatchInput input = SettledDesktopFrame();
  input.dockHost = LayoutRect{};
  input.previousDockHost = LayoutRect{};

  EXPECT_FALSE(RenderPaneViewportLatchReady(input));
}

}  // namespace
}  // namespace donner::editor
