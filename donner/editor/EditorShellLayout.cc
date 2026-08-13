#include "donner/editor/EditorShellLayout.h"

#include <algorithm>
#include <cmath>

namespace donner::editor {

namespace {

constexpr float kCompactWidthThreshold = 760.0f;
constexpr float kCompactHeightThreshold = 520.0f;
constexpr float kCompactTopBarHeight = 52.0f;
constexpr float kCompactToolButtonSize = 44.0f;
constexpr float kCompactPanelFraction = 0.42f;
constexpr float kCompactPanelMinWidth = 280.0f;
constexpr float kCompactPanelMaxWidth = 380.0f;
constexpr float kCompactPanelMinHeight = 220.0f;
constexpr float kCompactPanelMaxHeight = 360.0f;

}  // namespace

EditorAdaptiveUiLayout ComputeEditorAdaptiveUiLayout(const EditorAdaptiveUiInput& input) {
  const float windowWidth = std::max(0.0f, input.windowWidth);
  const float windowHeight = std::max(0.0f, input.windowHeight);
  const bool constrained =
      windowWidth < kCompactWidthThreshold || windowHeight < kCompactHeightThreshold;

  EditorAdaptiveUiLayout layout;
  if (!input.preferTouch && !constrained) {
    return layout;
  }

  layout.mode = EditorUiMode::CompactTouch;
  layout.topBarHeight = std::min(kCompactTopBarHeight, windowHeight);
  layout.toolButtonSize = kCompactToolButtonSize;
  layout.showPaintControls = false;
  layout.showTextFormatBar = false;
  layout.showCanvasScrollbars = false;

  if (windowWidth > windowHeight) {
    layout.panelPlacement = CompactPanelPlacement::Right;
    const float availableHeight = std::max(0.0f, windowHeight - layout.topBarHeight);
    const float requestedWidth = windowWidth * kCompactPanelFraction;
    layout.panelWidth = std::min(
        windowWidth, std::clamp(requestedWidth, kCompactPanelMinWidth, kCompactPanelMaxWidth));
    layout.panelHeight = availableHeight;
    layout.panelX = windowWidth - layout.panelWidth;
    layout.panelY = layout.topBarHeight;
  } else {
    layout.panelPlacement = CompactPanelPlacement::Bottom;
    const float availableHeight = std::max(0.0f, windowHeight - layout.topBarHeight);
    const float requestedHeight = windowHeight * kCompactPanelFraction;
    layout.panelWidth = windowWidth;
    layout.panelHeight =
        std::min(availableHeight,
                 std::clamp(requestedHeight, kCompactPanelMinHeight, kCompactPanelMaxHeight));
    layout.panelX = 0.0f;
    layout.panelY = windowHeight - layout.panelHeight;
  }
  return layout;
}

EditorMainPaneLayout ComputeEditorMainPaneLayout(const EditorMainPaneLayoutInput& input) {
  const float windowWidth = std::max(0.0f, input.windowWidth);
  const float minSourcePaneWidth = std::max(0.0f, input.minSourcePaneWidth);
  const float maxSourcePaneWidth = std::max(minSourcePaneWidth, input.maxSourcePaneWidth);
  const float minRightPaneWidth =
      input.rightPaneVisible ? std::max(0.0f, input.minRightPaneWidth) : 0.0f;
  const float minRenderPaneWidth = std::max(0.0f, input.minRenderPaneWidth);
  const float maxRightPaneWidth =
      input.rightPaneVisible ? std::max(minRightPaneWidth, input.maxRightPaneWidth) : 0.0f;
  const float sourcePaneRailWidth =
      input.sourcePaneVisible ? 0.0f : std::clamp(input.sourcePaneRailWidth, 0.0f, windowWidth);
  const float sourcePaneUpperBound = std::max(
      0.0f, std::min(maxSourcePaneWidth, windowWidth - minRightPaneWidth - minRenderPaneWidth));
  const float sourcePaneLowerBound = std::min(minSourcePaneWidth, sourcePaneUpperBound);
  const float sourcePaneWidth =
      input.sourcePaneVisible
          ? std::clamp(input.sourcePaneWidth, sourcePaneLowerBound, sourcePaneUpperBound)
          : 0.0f;
  const float leftChromeWidth = sourcePaneWidth + sourcePaneRailWidth;
  const float rightPaneUpperBound =
      std::max(minRightPaneWidth,
               std::min(maxRightPaneWidth, windowWidth - leftChromeWidth - minRenderPaneWidth));

  EditorMainPaneLayout layout;
  layout.sourcePaneWidth = sourcePaneWidth;
  layout.sourcePaneRailWidth = sourcePaneRailWidth;
  layout.rightPaneWidth =
      input.rightPaneVisible
          ? std::clamp(input.rightPaneWidth, minRightPaneWidth, rightPaneUpperBound)
          : 0.0f;
  layout.renderPaneX = leftChromeWidth;
  layout.renderPaneWidth = std::max(0.0f, windowWidth - leftChromeWidth - layout.rightPaneWidth);
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

namespace {

/// Layout rects are integral ImGui pixel coordinates; a pixel of slack absorbs float rounding
/// without admitting a genuinely different rectangle.
constexpr float kLayoutSettleEpsilonPx = 1.0f;

bool NearlyEqual(float a, float b) {
  return std::abs(a - b) <= kLayoutSettleEpsilonPx;
}

bool SameRect(const LayoutRect& a, const LayoutRect& b) {
  return NearlyEqual(a.x, b.x) && NearlyEqual(a.y, b.y) && NearlyEqual(a.width, b.width) &&
         NearlyEqual(a.height, b.height);
}

/// True when the central node is this frame's dock host split into a canvas column, exactly as
/// the shell laid the host out. Every comparison is against geometry the shell computed itself.
bool CentralNodeMatchesDockHost(const RenderPaneLatchInput& input) {
  if (!NearlyEqual(input.dockCentralNode.x, input.dockHost.x) ||
      !NearlyEqual(input.dockCentralNode.y, input.dockHost.y) ||
      !NearlyEqual(input.dockCentralNode.height, input.dockHost.height) ||
      input.dockCentralNode.width > input.dockHost.width + kLayoutSettleEpsilonPx) {
    return false;
  }

  return input.sidebarColumnIncluded
             ? input.dockCentralNode.width < input.dockHost.width - kLayoutSettleEpsilonPx
             : NearlyEqual(input.dockCentralNode.width, input.dockHost.width);
}

}  // namespace

bool RenderPaneViewportLatchReady(const RenderPaneLatchInput& input) {
  if (input.dockHost.width <= 0.0f || input.dockHost.height <= 0.0f ||
      input.paneContentWidth <= 0.0f || input.paneContentHeight <= 0.0f) {
    return false;
  }

  // A host that moved this frame invalidates everything downstream of it, including the
  // two-frame content-region history, which would otherwise pair a pre-change frame with a
  // post-change one.
  if (!SameRect(input.dockHost, input.previousDockHost)) {
    return false;
  }

  const bool dockGeometrySettled = input.dockCentralNode.width > 0.0f &&
                                   input.dockCentralNode.height > 0.0f &&
                                   SameRect(input.paneWindow, input.dockCentralNode) &&
                                   CentralNodeMatchesDockHost(input);
  if (dockGeometrySettled) {
    return true;
  }

  // Fallback for dock trees the policy above does not describe: the same content region on two
  // consecutive frames, which is only reached with a stationary host.
  return NearlyEqual(input.paneContentWidth, input.previousPaneContentWidth) &&
         NearlyEqual(input.paneContentHeight, input.previousPaneContentHeight);
}

}  // namespace donner::editor
