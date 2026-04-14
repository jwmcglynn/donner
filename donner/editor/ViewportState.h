#pragma once
/// @file
///
/// `ViewportState` is the single source of truth for the editor render
/// pane's screen↔document coordinate mapping. See
/// `docs/design_docs/0025-editor_ux.md` for the full design rationale;
/// this header is the public API.
///
/// Conceptually a `ViewportState` is a value the main loop snapshots
/// once at the top of each frame and then hands to every reader (async
/// render dispatch, click math, overlay re-render, image draw,
/// inspector text). Every reader consults the same value, so the four
/// pieces never disagree about where the document is on screen.
///
/// Coordinate spaces:
///   - **document** / **world**: SVG `viewBox` coordinates.
///   - **screen**: ImGui screen-pixel coordinates in *logical* pixels,
///     `(0, 0)` at the top-left of the OS window. On Retina, a mouse
///     position of `(100, 100)` is the same logical pixel regardless
///     of `devicePixelRatio`.
///   - **device pixel**: physical pixels in the rasterized bitmap.
///     Equal to `screen * devicePixelRatio`. Only the SVG renderer's
///     bitmap output and the GL texture upload care about this.
///
/// Zoom semantics: `zoom == 1.0` means **100%** — one SVG `viewBox`
/// unit takes exactly one screen pixel. This is the initial state
/// when the editor opens and the target of `Cmd+0` / View → Reset
/// Zoom. The user can pan freely if the document doesn't fit the
/// window.

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"

namespace donner::editor {

/// All viewport state for a single frame of the render pane. Plain
/// data, side-effect-free, copyable, ~80 bytes.
struct ViewportState {
  /// Minimum allowed zoom (10%).
  static constexpr double kMinZoom = 0.1;
  /// Maximum allowed zoom (3200%).
  static constexpr double kMaxZoom = 32.0;
  /// Hard cap on the rasterized canvas dimension on either axis. A
  /// 32x zoom on a 4K display would otherwise rasterize a multi-GB
  /// bitmap; this clamp keeps us in a safe place at the cost of
  /// transient pixelation when the user zooms very far in.
  static constexpr int kMaxCanvasDim = 8192;

  // ---------------------------------------------------------------------------
  // Inputs (set once per frame from user state).
  // ---------------------------------------------------------------------------

  /// Top-left of the render pane's content region in screen pixels.
  Vector2d paneOrigin = Vector2d::Zero();
  /// Size of the render pane's content region in screen pixels.
  Vector2d paneSize = Vector2d::Zero();
  /// SVG document viewBox in document coordinates.
  Box2d documentViewBox;
  /// OS device pixel ratio (1.0 on standard displays, 2.0 on Retina).
  /// Read via `glfwGetWindowContentScale`.
  double devicePixelRatio = 1.0;

  /// Zoom factor in *screen pixels per document unit*. `1.0` means
  /// "100%": one SVG `viewBox` unit takes exactly one screen pixel,
  /// regardless of how many physical device pixels that is.
  double zoom = 1.0;
  /// Document point that should appear at `panScreenPoint`. Together
  /// with `panScreenPoint` and `zoom`, this fully determines the
  /// screen↔document map.
  Vector2d panDocPoint = Vector2d::Zero();
  /// Screen point at which `panDocPoint` is anchored.
  Vector2d panScreenPoint = Vector2d::Zero();

  // ---------------------------------------------------------------------------
  // Coordinate transforms.
  // ---------------------------------------------------------------------------

  /// Screen pixels per document unit at the current zoom. With the
  /// "zoom is in screen-pixels-per-doc-unit" convention this is just
  /// `zoom`; the accessor exists so callers don't reach for the raw
  /// field and the definition can grow if "100%" ever stops meaning
  /// "1:1".
  [[nodiscard]] double pixelsPerDocUnit() const { return zoom; }

  /// Device pixels per document unit. Used by `desiredCanvasSize` and
  /// the renderer's `RenderViewport.devicePixelRatio`.
  [[nodiscard]] double devicePixelsPerDocUnit() const {
    return pixelsPerDocUnit() * devicePixelRatio;
  }

  /// Map a document point to a screen-pixel point.
  [[nodiscard]] Vector2d documentToScreen(const Vector2d& docPoint) const;
  /// Map a screen-pixel point to a document point.
  [[nodiscard]] Vector2d screenToDocument(const Vector2d& screenPoint) const;

  /// Map a document-space box to its screen-pixel AABB.
  [[nodiscard]] Box2d documentToScreen(const Box2d& docBox) const;
  /// Map a screen-pixel box to its document-space AABB.
  [[nodiscard]] Box2d screenToDocument(const Box2d& screenBox) const;

  /// On-screen rectangle of the displayed document image, in screen
  /// pixels. Equal to `documentToScreen(documentViewBox)`. Used by
  /// the `AddImage` call so the image always lands at the right place
  /// regardless of texture resolution.
  [[nodiscard]] Box2d imageScreenRect() const { return documentToScreen(documentViewBox); }

  /// Canvas size (in device pixels) the SVG renderer should produce
  /// for the current zoom and DPR. Equal to
  /// `documentViewBox.size() * zoom * devicePixelRatio`, clamped to
  /// `[1, kMaxCanvasDim]` per axis.
  [[nodiscard]] Vector2i desiredCanvasSize() const;

  // ---------------------------------------------------------------------------
  // Mutating helpers.
  // ---------------------------------------------------------------------------

  /// Set the zoom factor while holding `focalScreen` fixed: after
  /// this call, the document point that was previously under
  /// `focalScreen` is still under `focalScreen`. Use for Cmd+wheel,
  /// Cmd+Plus / Cmd+Minus, menu-driven zoom, and pinch-to-zoom.
  ///
  /// `newZoom` is clamped to `[kMinZoom, kMaxZoom]`.
  void zoomAround(double newZoom, const Vector2d& focalScreen);

  /// Pan by `screenDelta` screen pixels. After this call, every
  /// document point that was previously visible is at its previous
  /// screen position + `screenDelta`.
  void panBy(const Vector2d& screenDelta);

  /// Reset to 100%, with the document viewBox center anchored at the
  /// pane center. After this call, `pixelsPerDocUnit() == 1.0` and
  /// `documentToScreen(documentViewBoxCenter()) == paneCenter`. This
  /// is the initial state when the editor opens, and the target of
  /// Cmd+0 / View → Reset Zoom / View → Actual Size.
  void resetTo100Percent();

  // ---------------------------------------------------------------------------
  // Convenience getters.
  // ---------------------------------------------------------------------------

  /// Center of the render pane in screen pixels.
  [[nodiscard]] Vector2d paneCenter() const { return paneOrigin + paneSize * 0.5; }
  /// Center of the document viewBox in document coordinates.
  [[nodiscard]] Vector2d documentViewBoxCenter() const {
    return (documentViewBox.topLeft + documentViewBox.bottomRight) * 0.5;
  }
};

}  // namespace donner::editor
