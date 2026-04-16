#pragma once
/// @file

#include <array>
#include <optional>

#include "donner/editor/RenderPaneGesture.h"
#include "donner/editor/Tool.h"
#include "donner/editor/ViewportState.h"
#include "imgui.h"

namespace donner::editor {

constexpr std::size_t kFrameHistoryCapacity = 120;

struct FrameHistory {
  std::array<float, kFrameHistoryCapacity> deltaMs{};
  std::size_t writeIndex = 0;
  std::size_t samples = 0;

  void push(float ms);
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
