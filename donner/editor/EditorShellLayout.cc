#include "donner/editor/EditorShellLayout.h"

#include <algorithm>

namespace donner::editor {

EditorMainPaneLayout ComputeEditorMainPaneLayout(const EditorMainPaneLayoutInput& input) {
  const float windowWidth = std::max(0.0f, input.windowWidth);
  const float minSourcePaneWidth = std::max(0.0f, input.minSourcePaneWidth);
  const float maxSourcePaneWidth = std::max(minSourcePaneWidth, input.maxSourcePaneWidth);
  const float minRightPaneWidth = std::max(0.0f, input.minRightPaneWidth);
  const float minRenderPaneWidth = std::max(0.0f, input.minRenderPaneWidth);
  const float maxRightPaneWidth = std::max(minRightPaneWidth, input.maxRightPaneWidth);
  const float sourcePaneUpperBound = std::max(
      0.0f, std::min(maxSourcePaneWidth, windowWidth - minRightPaneWidth - minRenderPaneWidth));
  const float sourcePaneLowerBound = std::min(minSourcePaneWidth, sourcePaneUpperBound);
  const float sourcePaneWidth =
      input.sourcePaneVisible
          ? std::clamp(input.sourcePaneWidth, sourcePaneLowerBound, sourcePaneUpperBound)
          : 0.0f;
  const float rightPaneUpperBound =
      std::max(minRightPaneWidth,
               std::min(maxRightPaneWidth, windowWidth - sourcePaneWidth - minRenderPaneWidth));

  EditorMainPaneLayout layout;
  layout.sourcePaneWidth = sourcePaneWidth;
  layout.rightPaneWidth = std::clamp(input.rightPaneWidth, minRightPaneWidth, rightPaneUpperBound);
  layout.renderPaneX = sourcePaneWidth;
  layout.renderPaneWidth = std::max(0.0f, windowWidth - sourcePaneWidth - layout.rightPaneWidth);
  layout.rightPaneX = windowWidth - layout.rightPaneWidth;
  return layout;
}

RightSidebarLayout ComputeRightSidebarLayout(const RightSidebarLayoutInput& input) {
  const float paneOriginY = input.paneOriginY;
  const float paneHeight = std::max(0.0f, input.paneHeight);
  const float rightPaneGap = std::max(0.0f, input.rightPaneGap);
  const float splitterThickness =
      input.layerPanelDetached ? 0.0f : std::max(0.0f, input.layerPanelSplitterThickness);
  const float treeFraction = std::clamp(input.treeViewHeightFraction, 0.0f, 1.0f);
  const float requestedLayerFraction = std::clamp(input.layerPanelHeightFraction, 0.0f, 1.0f);

  RightSidebarLayout layout;
  layout.treePaneHeight = paneHeight * treeFraction;
  layout.lowerPaneHeight =
      std::max(0.0f, paneHeight - layout.treePaneHeight - rightPaneGap - splitterThickness);

  if (input.layerPanelDetached) {
    layout.inspectorPaneHeight = layout.lowerPaneHeight;
    layout.inspectorPaneY = paneOriginY + layout.treePaneHeight + rightPaneGap;
    layout.layerPanelSplitterY = layout.inspectorPaneY + layout.inspectorPaneHeight;
    layout.layerPanelPaneY = layout.layerPanelSplitterY;
    layout.layerPanelHeightFraction = requestedLayerFraction;
    return layout;
  }

  layout.minLayerPanelHeight =
      std::min(std::max(0.0f, input.minLayerPanelHeight), layout.lowerPaneHeight);
  const float remainingAfterLayerMin =
      std::max(0.0f, layout.lowerPaneHeight - layout.minLayerPanelHeight);
  const float minInspectorPaneHeight =
      std::min(std::max(0.0f, input.minInspectorPaneHeight), remainingAfterLayerMin);
  layout.maxLayerPanelHeight =
      std::max(layout.minLayerPanelHeight, layout.lowerPaneHeight - minInspectorPaneHeight);

  const float requestedLayerHeight = layout.lowerPaneHeight * requestedLayerFraction;
  layout.layerPanelHeight =
      std::clamp(requestedLayerHeight, layout.minLayerPanelHeight, layout.maxLayerPanelHeight);
  layout.inspectorPaneHeight = std::max(0.0f, layout.lowerPaneHeight - layout.layerPanelHeight);
  layout.inspectorPaneY = paneOriginY + layout.treePaneHeight + rightPaneGap;
  layout.layerPanelSplitterY = layout.inspectorPaneY + layout.inspectorPaneHeight;
  layout.layerPanelPaneY = layout.layerPanelSplitterY + splitterThickness;
  layout.layerPanelHeightFraction = layout.lowerPaneHeight > 0.0f
                                        ? layout.layerPanelHeight / layout.lowerPaneHeight
                                        : requestedLayerFraction;
  return layout;
}

float ResizeLayerPanelHeightFraction(float currentFraction, float lowerPaneHeight,
                                     float minLayerPanelHeight, float maxLayerPanelHeight,
                                     float splitterDeltaY) {
  const float clampedLowerPaneHeight = std::max(0.0f, lowerPaneHeight);
  if (clampedLowerPaneHeight == 0.0f) {
    return std::clamp(currentFraction, 0.0f, 1.0f);
  }

  const float minHeight = std::min(std::max(0.0f, minLayerPanelHeight), clampedLowerPaneHeight);
  const float maxHeight =
      std::max(minHeight, std::min(std::max(0.0f, maxLayerPanelHeight), clampedLowerPaneHeight));
  const float currentHeight = std::clamp(currentFraction, 0.0f, 1.0f) * clampedLowerPaneHeight;
  const float nextHeight = std::clamp(currentHeight - splitterDeltaY, minHeight, maxHeight);
  return nextHeight / clampedLowerPaneHeight;
}

}  // namespace donner::editor
