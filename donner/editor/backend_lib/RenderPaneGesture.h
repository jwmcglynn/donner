#pragma once
/// @file
///
/// Headless classification for render-pane scroll gestures. `main.cc`
/// feeds raw GLFW scroll events into this helper so tests can cover
/// pan vs. zoom routing without depending on ImGui or GLFW.

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

/// Raw scroll event captured from the windowing layer.
struct RenderPaneScrollEvent {
  /// Scroll delta in GLFW / ImGui wheel units.
  Vector2d scrollDelta = Vector2d::Zero();
  /// Cursor position in the render pane's screen-pixel space.
  Vector2d cursorScreen = Vector2d::Zero();
  /// True when Cmd/Ctrl was held for this event.
  bool zoomModifierHeld = false;
};

/// Per-frame state needed to route a raw scroll event.
struct RenderPaneGestureContext {
  /// Render-pane bounds in screen pixels.
  Box2d paneRect;
  /// True while the explicit mouse-drag pan gesture is active.
  bool mouseDragPanActive = false;
};

/// The viewport mutation a scroll gesture should apply.
enum class RenderPaneGestureActionType {
  Pan,
  Zoom,
};

/// Output of `ClassifyRenderPaneScrollGesture`.
struct RenderPaneGestureAction {
  /// Which viewport helper to call.
  RenderPaneGestureActionType type = RenderPaneGestureActionType::Pan;
  /// Screen-pixel delta for `ViewportState::panBy`.
  Vector2d panScreenDelta = Vector2d::Zero();
  /// Multiplicative zoom factor to apply to `ViewportState::zoom`.
  double zoomFactor = 1.0;
  /// Focal screen point for `ViewportState::zoomAround`.
  Vector2d focalScreen = Vector2d::Zero();

  bool operator==(const RenderPaneGestureAction& other) const = default;
};

/// Route a raw scroll event to either pan or zoom.
///
/// Bare scroll inside the pane pans. Cmd/Ctrl-modified vertical scroll
/// zooms around the cursor using the existing wheel-zoom step model.
/// Events outside the pane, zero-delta events, and wheel input while a
/// mouse-drag pan is active are ignored.
///
/// @param event Raw scroll event from GLFW.
/// @param context Per-frame pane bounds and mouse-pan state.
/// @param wheelZoomStep Multiplicative zoom step per +1.0 wheel unit.
/// @param panPixelsPerScrollUnit Screen pixels to pan per +1.0 scroll unit.
/// @return The viewport mutation to apply, if any.
[[nodiscard]] std::optional<RenderPaneGestureAction> ClassifyRenderPaneScrollGesture(
    const RenderPaneScrollEvent& event, const RenderPaneGestureContext& context,
    double wheelZoomStep, double panPixelsPerScrollUnit);

/// Apply a classified gesture action to the viewport.
///
/// @param viewport Viewport state to mutate.
/// @param action Gesture action previously returned by
///   `ClassifyRenderPaneScrollGesture`.
void ApplyRenderPaneGesture(ViewportState& viewport, const RenderPaneGestureAction& action);

}  // namespace donner::editor
