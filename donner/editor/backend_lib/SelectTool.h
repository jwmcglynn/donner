#pragma once
/// @file
///
/// `SelectTool` is the editor's first and (in this milestone) only tool.
/// It dispatches four different gestures off `onMouseDown`:
///
///   - **Click on an element** → replace the selection with that
///     element and start a drag (move on subsequent `onMouseMove`).
///   - **Drag a selection handle** → scale the selected element from
///     the opposite handle/edge. Holding Shift during the drag constrains
///     aspect ratio.
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
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/backend_lib/AttributeWriteback.h"
#include "donner/editor/backend_lib/Tool.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

class SelectTool final : public Tool {
public:
  /// Preview state for an in-progress drag, consumed by the async renderer.
  struct ActiveDragPreview {
    Entity entity = entt::null;
    Vector2d translation = Vector2d::Zero();
  };

  /// Payload needed to write a completed drag back into the source pane.
  /// For multi-element drags this is the primary; additional writeback
  /// entries are latched in `extras`.
  struct CompletedDragWriteback {
    AttributeWritebackTarget target;
    Transform2d transform;

    /// Additional writeback entries for extra elements in a multi-element
    /// drag. One per non-primary element that had a capturable writeback
    /// target. Empty for single-element drags.
    std::vector<CompletedDragWriteback> extras;
  };

  void onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                   MouseModifiers modifiers) override;
  void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) override;
  /// Mouse move with current modifier state. Used by the editor backend so Shift can dynamically
  /// constrain an in-progress resize.
  void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld,
                   MouseModifiers modifiers);
  void onMouseUp(EditorApp& editor, const Vector2d& documentPoint) override;

  /// Enable the experimental compositor-backed drag preview path.
  void setCompositedDragPreviewEnabled(bool enabled) { compositedDragPreviewEnabled_ = enabled; }

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

  /// Returns the most recent completed drag writeback request, if any.
  /// The payload is latched until consumed so the main loop can retry
  /// writeback across busy frames without depending on the current selection.
  [[nodiscard]] std::optional<CompletedDragWriteback> consumeCompletedDragWriteback() {
    auto result = completedDragWriteback_;
    completedDragWriteback_.reset();
    return result;
  }

  /// Returns the current drag preview, if a drag has crossed the movement threshold.
  [[nodiscard]] std::optional<ActiveDragPreview> activeDragPreview() const;

private:
  enum class DragKind {
    Move,
    Resize,
  };

  struct ResizeDrag {
    Vector2d anchorDocumentPoint;
    bool scaleX = false;
    bool scaleY = false;
  };

  /// Per-element bookkeeping for one participant in a drag. Carries the
  /// start transform (for computing `startTransform * Translate(delta)`
  /// on each mouse move), the current preview transform, and the stable
  /// locator for later canvas→text writeback.
  struct PerElementDrag {
    svg::SVGElement element;
    Transform2d startTransform;
    Transform2d currentTransform;
    std::optional<AttributeWritebackTarget> writebackTarget;
    /// Original `transform` attribute value captured at drag start. Used
    /// for `UndoSnapshot::sourceTransformAttributeValue` on release.
    /// Captured eagerly in `onMouseDown` (which runs only when the async
    /// renderer is idle) so `onMouseUp` never needs to touch the entt
    /// registry itself — reads here racing with a concurrent background
    /// render would hit `entt::fast_mod` assertions when the worker is
    /// resizing a dense-map bucket array.
    std::optional<RcString> sourceTransformAttributeValue;
  };

  struct DragState {
    /// Primary drag participant — the element that was under the cursor on
    /// mouse-down. Always populated. The compositor-preview fast path
    /// (when a single-element drag is composited) runs against this one.
    PerElementDrag primary;

    DragKind kind = DragKind::Move;

    /// Additional elements that move in lockstep with the primary. Empty
    /// for a single-element drag. Populated when mouse-down hits an
    /// already-selected element and the current selection has more than
    /// one entry — classic "grab an item in the current selection, move
    /// them all" design-tool behavior.
    std::vector<PerElementDrag> extras;

    Vector2d startDocumentPoint;
    ResizeDrag resize;
    /// Current drag delta in document coordinates, used for compositor preview.
    Vector2d currentDocumentDelta = Vector2d::Zero();
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
  std::optional<CompletedDragWriteback> completedDragWriteback_;
  bool compositedDragPreviewEnabled_ = false;
};

/// Render-mode toggles are safe whenever there is no in-progress drag or marquee gesture.
[[nodiscard]] inline bool CanToggleCompositedRendering(const SelectTool& tool) {
  return !tool.isDragging() && !tool.isMarqueeing();
}

}  // namespace donner::editor
