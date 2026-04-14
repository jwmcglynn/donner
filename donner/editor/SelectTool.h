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
#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/Tool.h"
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
  struct CompletedDragWriteback {
    AttributeWritebackTarget target;
    Transform2d transform;
  };

  void onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                   MouseModifiers modifiers) override;
  void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) override;
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

  /// Returns the current drag preview, if a drag is in progress.
  [[nodiscard]] std::optional<ActiveDragPreview> activeDragPreview() const;

private:
  struct DragState {
    svg::SVGElement element;
    Vector2d startDocumentPoint;
    Transform2d startTransform;
    /// The most recent preview transform for this drag. Tracked so
    /// `onMouseUp` can queue one final commit and record the undo
    /// entry without reading the element back from the document.
    Transform2d currentTransform;
    /// Current drag delta in document coordinates, used for compositor preview.
    Vector2d currentDocumentDelta = Vector2d::Zero();
    /// Stable locator used for the later canvas→text writeback.
    std::optional<AttributeWritebackTarget> writebackTarget;
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

}  // namespace donner::editor
