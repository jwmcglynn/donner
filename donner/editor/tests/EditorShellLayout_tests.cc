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

}  // namespace
}  // namespace donner::editor
