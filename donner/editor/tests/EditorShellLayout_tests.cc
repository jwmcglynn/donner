#include "donner/editor/EditorShellLayout.h"

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

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
