#pragma once
/// @file
///
/// `SelectTool` is the editor's first and (in this milestone) only tool.
/// It dispatches three different gestures off `onMouseDown`:
///
///   - **Click on an element** → replace the selection with that
///     element and start a drag (move on subsequent `onMouseMove`).
///   - **Shift+click on an element** → toggle that element in the
///     current selection (no drag).
///   - **Click on empty space** → start a marquee drag. Subsequent
///     `onMouseMove` events grow the marquee rect; `onMouseUp`
///     resolves the rect to every geometry element it intersects and
///     replaces (or appends-to, if Shift was held) the selection.
///
/// All DOM mutations flow through `EditorApp::applyMutation()` as
/// `EditorCommand::SetTransform` — never directly.

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/Tool.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

class SelectTool final : public Tool {
public:
  void onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                   MouseModifiers modifiers) override;
  void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) override;
  void onMouseUp(EditorApp& editor, const Vector2d& documentPoint) override;

  /// Whether a drag is currently in progress (button is held after a
  /// successful hit-test on mouse-down).
  [[nodiscard]] bool isDragging() const { return dragState_.has_value(); }

  /// Whether a marquee selection is currently in progress (button is
  /// held after a `onMouseDown` that hit empty space).
  [[nodiscard]] bool isMarqueeing() const { return marqueeState_.has_value(); }

  /// The current marquee rectangle in document coordinates, or
  /// `std::nullopt` if no marquee drag is active. Returned even on
  /// the very first frame of a marquee (rect collapses to a single
  /// point). Used by the overlay renderer to draw the marquee chrome.
  [[nodiscard]] std::optional<Box2d> marqueeRect() const;

  /// Returns true **exactly once** after a drag that actually moved
  /// completes (i.e. `onMouseUp` was called and `hasMoved` was true).
  /// The main loop polls this after `flushFrame()` to know when to
  /// build a canvas→text writeback patch for the `transform` attribute.
  /// Calling this function clears the flag.
  [[nodiscard]] bool consumeDragCompleted() {
    if (dragCompleted_) {
      dragCompleted_ = false;
      return true;
    }
    return false;
  }

private:
  struct DragState {
    svg::SVGElement element;
    Vector2d startDocumentPoint;
    Transform2d startTransform;
    /// The most recent transform SelectTool has pushed through the
    /// command queue during this drag. Tracked so `onMouseUp` can
    /// record an undo entry with the final drag state without having
    /// to read the element back (the queued commands may not have
    /// been flushed yet).
    Transform2d currentTransform;
    /// Whether any `onMouseMove` has fired since `onMouseDown`. A
    /// click-without-drag shouldn't leave an undo entry behind.
    bool hasMoved = false;
  };

  /// Active marquee drag. Records the start point (the document
  /// position of the `onMouseDown` that hit empty space), the
  /// current point (updated on every `onMouseMove`), and whether
  /// Shift was held — additive marquee appends to the existing
  /// selection instead of replacing it.
  struct MarqueeState {
    Vector2d startDocumentPoint;
    Vector2d currentDocumentPoint;
    bool additive = false;
  };

  std::optional<DragState> dragState_;
  std::optional<MarqueeState> marqueeState_;
  bool dragCompleted_ = false;
};

}  // namespace donner::editor
