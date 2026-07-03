#include "donner/editor/ViewportInteractionController.h"

#include <algorithm>

namespace donner::editor {

namespace {

bool ViewportMappingChanged(const ViewportState& before, const ViewportState& after) {
  return before.zoom != after.zoom || before.panDocPoint != after.panDocPoint ||
         before.panScreenPoint != after.panScreenPoint;
}

float MainFrameProfiledMs(const FrameProfilerSample& sample) {
  return sample.mainPreparationMs + sample.mainRenderPollMs + sample.mainDocumentFlushMs +
         sample.mainOverlayRefreshMs + sample.mainDocumentSyncMs + sample.mainLayoutMs +
         sample.mainShortcutsMs + sample.mainMenusDialogsMs + sample.mainSourcePaneMs +
         sample.mainRenderPaneMs + sample.mainSidebarsMs + sample.mainSplittersMs +
         sample.mainEndRenderRequestMs;
}

float HostFrameProfiledMs(const FrameProfilerSample& sample) {
  return sample.hostBeginFrameMs + sample.hostPreviousEndFrameMs;
}

float LegacyUiProfiledMs(const FrameProfilerSample& sample) {
  return sample.overlayCaptureMs + sample.overlayDrawMs + sample.overlaySnapshotMs +
         sample.overlayUploadMs + sample.compositedUploadMs + sample.sourceRopeLayoutMs +
         sample.sourceRopeUpdateMs + sample.sourceRopeDrawMs;
}

}  // namespace

float FrameProfilerSample::totalProfiledMs() const {
  const float topLevelMs = HostFrameProfiledMs(*this) + MainFrameProfiledMs(*this);
  if (topLevelMs > 0.0f) {
    return topLevelMs;
  }

  return LegacyUiProfiledMs(*this);
}

void FrameHistory::push(float ms) {
  deltaMs[writeIndex] = ms;
  // Reset the worker slot to zero — `setLatestBackendMs` will fill it in when
  // a render result lands during the same UI frame.
  backendMs[writeIndex] = 0.0f;
  profiler[writeIndex] = FrameProfilerSample{};
  memory[writeIndex] = FrameMemorySample{};
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

void FrameHistory::setLatestFrameCost(const FrameCostBreakdown& cost) {
  if (samples == 0) {
    return;
  }

  const std::size_t latestIdx = (writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  profiler[latestIdx] = FrameProfilerSample{
      .hostBeginFrameMs = static_cast<float>(cost.hostFrame.beginFrameMs),
      .hostPreviousEndFrameMs = static_cast<float>(cost.hostFrame.previousEndFrameMs),
      .hostPreviousImguiRenderMs = static_cast<float>(cost.hostFrame.previousImguiRenderMs),
      .hostPreviousSurfaceAcquireMs = static_cast<float>(cost.hostFrame.previousSurfaceAcquireMs),
      .hostPreviousUnderlayMs = static_cast<float>(cost.hostFrame.previousUnderlayMs),
      .hostPreviousImguiDrawMs = static_cast<float>(cost.hostFrame.previousImguiDrawMs),
      .hostPreviousDirectMs = static_cast<float>(cost.hostFrame.previousDirectMs),
      .hostPreviousReadbackMs = static_cast<float>(cost.hostFrame.previousReadbackMs),
      .hostPreviousPresentMs = static_cast<float>(cost.hostFrame.previousPresentMs),
      .mainPreparationMs = static_cast<float>(cost.mainFrame.preparationMs),
      .mainRenderPollMs = static_cast<float>(cost.mainFrame.renderPollMs),
      .mainDocumentFlushMs = static_cast<float>(cost.mainFrame.documentFlushMs),
      .mainOverlayRefreshMs = static_cast<float>(cost.mainFrame.overlayRefreshMs),
      .mainDocumentSyncMs = static_cast<float>(cost.mainFrame.documentSyncMs),
      .mainLayoutMs = static_cast<float>(cost.mainFrame.layoutMs),
      .mainShortcutsMs = static_cast<float>(cost.mainFrame.shortcutsMs),
      .mainMenusDialogsMs = static_cast<float>(cost.mainFrame.menusDialogsMs),
      .mainSourcePaneMs = static_cast<float>(cost.mainFrame.sourcePaneMs),
      .mainRenderPaneMs = static_cast<float>(cost.mainFrame.renderPaneMs),
      .mainSidebarsMs = static_cast<float>(cost.mainFrame.sidebarsMs),
      .mainSplittersMs = static_cast<float>(cost.mainFrame.splittersMs),
      .mainEndRenderRequestMs = static_cast<float>(cost.mainFrame.endRenderRequestMs),
      .overlayCaptureMs = static_cast<float>(cost.overlay.captureMs),
      .overlayDrawMs = static_cast<float>(cost.overlay.drawMs),
      .overlaySnapshotMs = static_cast<float>(cost.overlay.snapshotMs),
      .overlayUploadMs = static_cast<float>(cost.overlay.uploadMs),
      .compositedUploadMs = static_cast<float>(cost.compositedUpload.uploadMs),
      .compositedRenderImmediateMs = static_cast<float>(cost.compositedRender.immediateMs),
      .compositedRenderCachedMs = static_cast<float>(cost.compositedRender.cachedMs),
      .sourceRopeLayoutMs = static_cast<float>(cost.sourceRopes.layoutMs),
      .sourceRopeUpdateMs = static_cast<float>(cost.sourceRopes.updateMs),
      .sourceRopeDrawMs = static_cast<float>(cost.sourceRopes.drawMs),
  };
}

void FrameHistory::setLatestMemorySample(const FrameMemorySample& sample) {
  if (samples == 0) {
    return;
  }

  const std::size_t latestIdx = (writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  memory[latestIdx] = sample;
}

FrameMemorySample FrameHistory::latestNonZeroMemorySample() const {
  for (std::size_t i = 0; i < samples; ++i) {
    const std::size_t idx = (writeIndex + kFrameHistoryCapacity - 1 - i) % kFrameHistoryCapacity;
    const FrameMemorySample& sample = memory[idx];
    if (sample.totalTrackedBytes > 0u || sample.peakTrackedBytes > 0u) {
      return sample;
    }
  }

  return FrameMemorySample{};
}

float FrameHistory::latest() const {
  if (samples == 0) {
    return 0.0f;
  }
  const std::size_t idx = (writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  return deltaMs[idx];
}

float FrameHistory::latestBackend() const {
  if (samples == 0) {
    return 0.0f;
  }
  const std::size_t idx = (writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  return backendMs[idx];
}

float FrameHistory::max() const {
  float maximum = 0.0f;
  for (std::size_t i = 0; i < samples; ++i) {
    maximum = std::max(maximum, deltaMs[i]);
  }
  return maximum;
}

bool ShouldShowRenderPanePanCursor(bool canvasHovered, bool spaceHeld, bool panning) {
  return panning || (canvasHovered && spaceHeld);
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

bool ViewportInteractionController::resetToActualSize() {
  const ViewportState before = viewport_;
  viewport_.resetTo100Percent();
  return ViewportMappingChanged(before, viewport_);
}

bool ViewportInteractionController::applyZoom(double factor, const Vector2d& focalScreen) {
  const ViewportState before = viewport_;
  viewport_.zoomAround(viewport_.zoom * factor, focalScreen);
  return ViewportMappingChanged(before, viewport_);
}

bool ViewportInteractionController::updatePanState(bool paneHovered, bool spaceHeld,
                                                   bool middleDown, bool leftDown,
                                                   const ImVec2& mousePosition) {
  const ViewportState before = viewport_;
  if (paneHovered && (spaceHeld || middleDown) && leftDown && !panning_) {
    panning_ = true;
    lastPanMouse_ = mousePosition;
  } else if (middleDown && !panning_) {
    panning_ = true;
    lastPanMouse_ = mousePosition;
  }

  if (!panning_) {
    return false;
  }

  viewport_.panBy(Vector2d(mousePosition.x - lastPanMouse_.x, mousePosition.y - lastPanMouse_.y));
  lastPanMouse_ = mousePosition;
  if (!leftDown && !middleDown) {
    panning_ = false;
  }

  return ViewportMappingChanged(before, viewport_);
}

ScrollConsumptionResult ViewportInteractionController::consumeScrollEvents(
    std::vector<RenderPaneScrollEvent>& events, const Box2d& paneRect, bool modalCapturingInput,
    double wheelZoomStep, double panPixelsPerScrollUnit) {
  ScrollConsumptionResult result;
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
      const ViewportState before = viewport_;
      ApplyRenderPaneGesture(viewport_, *action);
      const bool viewportChanged = ViewportMappingChanged(before, viewport_);
      result.viewportChanged = result.viewportChanged || viewportChanged;
      result.zoomChanged = result.zoomChanged ||
                           (viewportChanged && action->type == RenderPaneGestureActionType::Zoom);
    }
  }

  events.clear();
  return result;
}

void ViewportInteractionController::bufferPendingClick(const Vector2d& documentPoint,
                                                       MouseModifiers modifiers) {
  pendingClick_ = PendingClick{
      .documentPoint = documentPoint,
      .modifiers = modifiers,
  };
}

}  // namespace donner::editor
