#include "donner/editor/backend_lib/RenderPaneGesture.h"

#include <cmath>

namespace donner::editor {

namespace {

constexpr double kEpsilonScroll = 1e-9;

[[nodiscard]] bool HasNonZeroScroll(const Vector2d& delta) {
  return std::abs(delta.x) > kEpsilonScroll || std::abs(delta.y) > kEpsilonScroll;
}

}  // namespace

std::optional<RenderPaneGestureAction> ClassifyRenderPaneScrollGesture(
    const RenderPaneScrollEvent& event, const RenderPaneGestureContext& context,
    double wheelZoomStep, double panPixelsPerScrollUnit) {
  if (context.mouseDragPanActive || !context.paneRect.contains(event.cursorScreen) ||
      !HasNonZeroScroll(event.scrollDelta)) {
    return std::nullopt;
  }

  if (event.zoomModifierHeld) {
    if (std::abs(event.scrollDelta.y) <= kEpsilonScroll) {
      return std::nullopt;
    }

    RenderPaneGestureAction action;
    action.type = RenderPaneGestureActionType::Zoom;
    action.zoomFactor = std::pow(wheelZoomStep, event.scrollDelta.y);
    action.focalScreen = event.cursorScreen;
    return action;
  }

  RenderPaneGestureAction action;
  action.type = RenderPaneGestureActionType::Pan;
  action.panScreenDelta = event.scrollDelta * panPixelsPerScrollUnit;
  return action;
}

void ApplyRenderPaneGesture(ViewportState& viewport, const RenderPaneGestureAction& action) {
  if (action.type == RenderPaneGestureActionType::Zoom) {
    viewport.zoomAround(viewport.zoom * action.zoomFactor, action.focalScreen);
    return;
  }

  viewport.panBy(action.panScreenDelta);
}

}  // namespace donner::editor
