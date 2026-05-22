#include "donner/editor/EditorShellLayout.h"

#include <algorithm>

namespace donner::editor {

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
