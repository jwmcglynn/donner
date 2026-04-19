#include "donner/editor/ViewportInteractionController.h"

#include <algorithm>

namespace donner::editor {

void FrameHistory::push(float ms) {
  deltaMs[writeIndex] = ms;
  // Reset the backend slot to zero — `setLatestBackendMs` will fill it
  // in when a render result lands during the same UI frame.
  backendMs[writeIndex] = 0.0f;
  writeIndex = (writeIndex + 1) % kFrameHistoryCapacity;
  if (samples < kFrameHistoryCapacity) {
    ++samples;
  }
}

void FrameHistory::setLatestBackendMs(float ms) {
  if (samples == 0) {
    return;
  }
  const std::size_t latestIdx = (writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  backendMs[latestIdx] = ms;
  if (ms > 0.0f) {
    lastBackendMs = ms;
  }
}

float FrameHistory::latest() const {
  if (samples == 0) {
    return 0.0f;
  }
  const std::size_t idx = (writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  return deltaMs[idx];
}

float FrameHistory::max() const {
  float maximum = 0.0f;
  for (std::size_t i = 0; i < samples; ++i) {
    maximum = std::max(maximum, deltaMs[i]);
  }
  return maximum;
}

void ViewportInteractionController::updatePaneLayout(const Vector2d& paneOrigin,
                                                     const Vector2d& paneSize,
                                                     const std::optional<Box2d>& documentViewBox) {
  viewport_.paneOrigin = paneOrigin;
  viewport_.paneSize = paneSize;
  if (documentViewBox.has_value()) {
    viewport_.documentViewBox = *documentViewBox;
  }
}

void ViewportInteractionController::updateDevicePixelRatio(double devicePixelRatio) {
  viewport_.devicePixelRatio = devicePixelRatio;
}

void ViewportInteractionController::resetToActualSize() {
  viewport_.resetTo100Percent();
}

void ViewportInteractionController::applyZoom(double factor, const Vector2d& focalScreen) {
  viewport_.zoomAround(viewport_.zoom * factor, focalScreen);
}

void ViewportInteractionController::updatePanState(bool paneHovered, bool spaceHeld,
                                                   bool middleDown, bool leftDown,
                                                   const ImVec2& mousePosition) {
  if (paneHovered && (spaceHeld || middleDown) && leftDown && !panning_) {
    panning_ = true;
    lastPanMouse_ = mousePosition;
  } else if (middleDown && !panning_) {
    panning_ = true;
    lastPanMouse_ = mousePosition;
  }

  if (!panning_) {
    return;
  }

  viewport_.panBy(Vector2d(mousePosition.x - lastPanMouse_.x, mousePosition.y - lastPanMouse_.y));
  lastPanMouse_ = mousePosition;
  if (!leftDown && !middleDown) {
    panning_ = false;
  }
}

void ViewportInteractionController::consumeScrollEvents(std::vector<RenderPaneScrollEvent>& events,
                                                        const Box2d& paneRect,
                                                        bool modalCapturingInput,
                                                        double wheelZoomStep,
                                                        double panPixelsPerScrollUnit) {
  if (!modalCapturingInput) {
    const RenderPaneGestureContext gestureContext{
        .paneRect = paneRect,
        .mouseDragPanActive = panning_,
    };
    for (const auto& event : events) {
      const auto action = ClassifyRenderPaneScrollGesture(event, gestureContext, wheelZoomStep,
                                                          panPixelsPerScrollUnit);
      if (!action.has_value()) {
        continue;
      }
      ApplyRenderPaneGesture(viewport_, *action);
    }
  }

  events.clear();
}

void ViewportInteractionController::bufferPendingClick(const Vector2d& documentPoint,
                                                       MouseModifiers modifiers) {
  pendingClick_ = PendingClick{
      .documentPoint = documentPoint,
      .modifiers = modifiers,
  };
}

}  // namespace donner::editor
