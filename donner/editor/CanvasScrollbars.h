#pragma once
/// @file
///
/// Geometry for the render pane's emulated canvas scrollbars. The canvas pane
/// never window-scrolls (an ImGui window scrollbar would move the in-pane
/// overlay chrome — toolbar, perf HUD — instead of the document), so the pane
/// draws its own scrollbars that represent the document extent relative to
/// the viewport and pan the canvas when dragged.

#include "donner/editor/ViewportState.h"

namespace donner::editor {

/// One scrollbar axis of the render pane's emulated canvas scrollbars.
struct CanvasScrollbarAxis {
  /// True when the document extends past the pane on this axis (the bar is
  /// drawn only when there is something to scroll to).
  bool visible = false;
  /// Rail start position along the axis, in logical screen coordinates.
  double railStart = 0.0;
  /// Rail length along the axis, in logical screen coordinates.
  double railLength = 0.0;
  /// Thumb start position along the axis, in logical screen coordinates.
  double thumbStart = 0.0;
  /// Thumb length along the axis, in logical screen coordinates.
  double thumbLength = 0.0;
  /// Screen-space content shift per pixel of thumb drag: dragging the thumb
  /// by +1 px pans the content by -contentPerThumbPx px on this axis.
  double contentPerThumbPx = 1.0;
};

/// Both axes of the render pane's emulated canvas scrollbars.
struct CanvasScrollbars {
  CanvasScrollbarAxis horizontal;  //!< Along the pane bottom edge.
  CanvasScrollbarAxis vertical;    //!< Along the pane right edge.
};

/// Minimum thumb length in logical pixels so the thumb stays grabbable at
/// extreme zoom.
inline constexpr double kCanvasScrollbarMinThumbPx = 24.0;

/// Rail thickness in logical pixels (drawn along the pane bottom/right edges).
inline constexpr double kCanvasScrollbarRailPx = 10.0;

/// True when @p screenPoint lies on a visible scrollbar rail for @p viewport.
/// Canvas tools must ignore pointer input over the rails so scrollbar drags
/// never become canvas clicks or element drags.
[[nodiscard]] bool CanvasScrollbarsContain(const ViewportState& viewport,
                                           const Vector2d& screenPoint);

/// Compute the emulated scrollbar geometry for @p viewport. The scrollable
/// content extent on each axis is the union of the document's screen-space
/// box and the pane, matching the usual scroll-region contract (scrolling
/// can always bring the viewport back to the document).
[[nodiscard]] CanvasScrollbars ComputeCanvasScrollbars(const ViewportState& viewport);

}  // namespace donner::editor
