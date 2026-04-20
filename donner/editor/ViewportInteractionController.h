#pragma once
/// @file

#include <array>
#include <optional>
#include <vector>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/backend_lib/RenderPaneGesture.h"
#include "donner/editor/backend_lib/Tool.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

constexpr std::size_t kFrameHistoryCapacity = 120;

struct FrameHistory {
  /// ImGui frame delta per UI-thread frame — populated from
  /// `ImGui::GetIO().DeltaTime` by `noteFrameDelta`.
  std::array<float, kFrameHistoryCapacity> deltaMs{};
  /// Async-renderer worker time (ms) per UI frame, aligned 1:1 with the
  /// matching `deltaMs[]` slot. Holds `0.0f` for frames where no render
  /// result landed (the worker was still busy or nothing was requested);
  /// consumers skip zero entries so the graph doesn't drop to zero
  /// between drags.
  std::array<float, kFrameHistoryCapacity> backendMs{};
  std::size_t writeIndex = 0;
  std::size_t samples = 0;
  /// Most recent non-zero backend sample, so latched-backend-latency
  /// readers (the numeric readout, sticky-line rendering) have something
  /// to show between render-result landings.
  float lastBackendMs = 0.0f;

  /// Append a new frame sample. The matching `backendMs[]` slot is
  /// reset to 0 ("no backend result this frame"); `setLatestBackendMs`
  /// fills it in if a render result lands during the same UI frame.
  void push(float ms);
  /// Record a backend (async-renderer worker) timing for the most
  /// recently pushed frame. Called by `RenderCoordinator::pollRenderResult`
  /// when a new `RenderResult` arrives. No-op if no frame has been pushed
  /// yet. Also updates `lastBackendMs` so UI elements that want to show
  /// the latest measured backend latency have a persistent value.
  void setLatestBackendMs(float ms);
  [[nodiscard]] float latest() const;
  [[nodiscard]] float max() const;
};

struct PendingClick {
  Vector2d documentPoint;
  MouseModifiers modifiers;
};

/// Owns viewport/input state for the render pane: zoom/pan, pending click dispatch, and frame
/// timing history.
class ViewportInteractionController {
public:
  [[nodiscard]] ViewportState& viewport() { return viewport_; }
  [[nodiscard]] const ViewportState& viewport() const { return viewport_; }

  [[nodiscard]] FrameHistory& frameHistory() { return frameHistory_; }
  [[nodiscard]] const FrameHistory& frameHistory() const { return frameHistory_; }

  void noteFrameDelta(float deltaMs) { frameHistory_.push(deltaMs); }

  void updatePaneLayout(const Vector2d& paneOrigin, const Vector2d& paneSize,
                        const std::optional<Box2d>& documentViewBox);
  void updateDevicePixelRatio(double devicePixelRatio);
  void resetToActualSize();
  void applyZoom(double factor, const Vector2d& focalScreen);

  void updatePanState(bool paneHovered, bool spaceHeld, bool middleDown, bool leftDown,
                      const ImVec2& mousePosition);
  [[nodiscard]] bool panning() const { return panning_; }

  void consumeScrollEvents(std::vector<RenderPaneScrollEvent>& events, const Box2d& paneRect,
                           bool modalCapturingInput, double wheelZoomStep,
                           double panPixelsPerScrollUnit);

  void bufferPendingClick(const Vector2d& documentPoint, MouseModifiers modifiers);
  [[nodiscard]] const std::optional<PendingClick>& pendingClick() const { return pendingClick_; }
  void clearPendingClick() { pendingClick_.reset(); }

private:
  ViewportState viewport_;
  FrameHistory frameHistory_;
  bool panning_ = false;
  ImVec2 lastPanMouse_ = ImVec2(0.0f, 0.0f);
  std::optional<PendingClick> pendingClick_;
};

}  // namespace donner::editor
