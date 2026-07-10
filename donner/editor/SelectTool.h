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
/// `EditorCommand::SetTransform` - never directly.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/SelectionTransformHandles.h"
#include "donner/editor/Tool.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

class SelectTool final : public Tool {
public:
  /// Preview state for an in-progress drag, consumed by the async renderer.
  struct ActiveDragPreview {
    /// Entity being dragged.
    Entity entity = entt::null;
    /// Additional selected entities moving with \ref entity in the same gesture.
    std::vector<Entity> extraEntities;
    /// Current drag translation in document coordinates.
    Vector2d translation = Vector2d::Zero();
    /// Current affine transform from the cached document placement to
    /// the active preview placement.
    Transform2d documentFromCachedDocument = Transform2d();
    /// Monotonic id for one mouse-down/move/up drag gesture.
    std::uint64_t dragGeneration = 0;
  };

  /// Active transform chrome state for selection bounds presentation.
  struct ActiveTransformBoundsPreview {
    /// Selection AABB captured when the transform gesture started.
    Box2d startBoundsDoc;
    /// Current transform from gesture-start document space to active
    /// document space.
    Transform2d documentFromStartDocument = Transform2d();
  };

  /// Active selection gesture kind.
  enum class ActiveGestureKind {
    Move,
    Resize,
    Rotate,
  };

  /// Active selection gesture state for editor UI chrome.
  struct ActiveGesturePreview {
    /// Gesture kind currently being performed.
    ActiveGestureKind kind = ActiveGestureKind::Move;
    /// Transform handle corner that started the gesture.
    SelectionTransformCorner corner = SelectionTransformCorner::TopLeft;
    /// Selection bounds captured at gesture start.
    Box2d startBoundsDoc;
    /// Current transform from gesture-start document space to active document space.
    Transform2d documentFromStartDocument = Transform2d();
    /// Current drag delta in document coordinates.
    Vector2d currentDocumentDelta = Vector2d::Zero();
    /// Whether the gesture has moved past the drag threshold.
    bool hasMoved = false;
  };

  /// Payload needed to write a completed drag back into the source pane.
  /// For multi-element drags this is the primary; additional writeback
  /// entries are latched in `extras`.
  struct CompletedDragWriteback {
    AttributeWritebackTarget target;
    Transform2d transform;

    /// The element's `transform=` bytes as authored at drag start, used by the
    /// source writeback to preserve the author's function syntax instead of
    /// canonicalizing to `matrix()`. `std::nullopt` when the element had no
    /// transform attribute before the drag.
    std::optional<RcString> sourceTransformAttributeValue;

    /// Additional writeback entries for extra elements in a multi-element
    /// drag. One per non-primary element that had a capturable writeback
    /// target. Empty for single-element drags.
    std::vector<CompletedDragWriteback> extras;
  };

  void onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                   MouseModifiers modifiers) override;
  void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) override;
  /// Mouse-move variant that samples live modifiers during resize gestures.
  void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld,
                   MouseModifiers modifiers);
  void onMouseUp(EditorApp& editor, const Vector2d& documentPoint) override;

  /// Snapshot-safe re-drag start (design doc 0033 §M8). When the user
  /// clicks inside the bounds of the single currently-selected element,
  /// start a drag of that element WITHOUT calling `EditorApp::hitTest`
  /// - so the call is safe to run even while the async-renderer worker
  /// is mid-render (hitTest would race the worker's
  /// `prepareDocumentForRendering`).
  ///
  /// `selectionBoundsDoc` and `occludingBoundsDoc` are **caller-supplied**
  /// AABB lists in document space. EditorShell passes the pre-snapshotted
  /// bounds from `SelectionBoundsCache` - that cache is refreshed on idle
  /// frames, so reading it during a busy render is race-free (no live
  /// `SnapshotSelectionWorldBounds` call inside this function). `onMouseDown`
  /// passes freshly-computed live selection bounds and no occlusion hints since
  /// its caller has already gated on `!isBusy()`.
  ///
  /// Returns true if a drag was started; false if the caller must fall
  /// back to the full `onMouseDown` path (multi-select, shift-click,
  /// click outside the selection's snapshotted bounds, click inside
  /// later-painted cached bounds, empty bounds span, etc.).
  ///
  /// `onMouseDown` itself calls this first to avoid duplicating logic
  /// - so plain clicks on the selection always take the no-hit-test
  /// path regardless of busy state. The split exists so EditorShell
  /// can run it BEFORE checking `isBusy()` for the click handler.
  [[nodiscard]] bool tryStartRedragOnSelected(EditorApp& editor, const Vector2d& documentPoint,
                                              MouseModifiers modifiers,
                                              std::span<const Box2d> selectionBoundsDoc,
                                              std::span<const Box2d> occludingBoundsDoc = {});

  /// Returns true when live hit-testing at @p documentPoint hits the current selectable selection
  /// or one of its renderable descendants.
  ///
  /// This reads the live document through \ref EditorApp::hitTest, so callers must only use it when
  /// the async renderer is idle. EditorShell uses this to keep clicks on already-selected elements
  /// on the immediate drag path instead of converting a held press into a marquee. Locked selected
  /// elements return false because they cannot start an immediate canvas drag.
  ///
  /// @param editor Editor state used for hit-testing and the current selection.
  /// @param documentPoint Pointer location in SVG document coordinates.
  /// @return true when the point hits an unlocked selected element or selected descendant.
  [[nodiscard]] bool clickHitsCurrentSelection(EditorApp& editor,
                                               const Vector2d& documentPoint) const;

  /// Returns true when live hit-testing at @p documentPoint hits an element that can be selected
  /// immediately by a canvas press.
  ///
  /// Locked layers and descendants of locked layers return false: they may still flash rejection on
  /// a click, but they should not block the shell's delayed marquee path.
  ///
  /// @param editor Editor state used for hit-testing.
  /// @param documentPoint Pointer location in SVG document coordinates.
  /// @return true when the point hits an unlocked geometry element.
  [[nodiscard]] bool clickHitsImmediatelySelectableElement(EditorApp& editor,
                                                           const Vector2d& documentPoint) const;

  /// Whether a drag is currently in progress (button is held after a
  /// successful hit-test on mouse-down).
  [[nodiscard]] bool isDragging() const { return dragState_.has_value(); }

  /// Whether a marquee selection is currently in progress (button is
  /// held after a `onMouseDown` that hit empty space).
  [[nodiscard]] bool isMarqueeing() const { return marqueeState_.has_value(); }

  /// Start a marquee drag from `documentPoint` without performing a hit-test.
  void beginMarquee(EditorApp& editor, const Vector2d& documentPoint, bool additive);

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

  /// Returns the current selection gesture preview, if a drag is in progress.
  [[nodiscard]] std::optional<ActiveGesturePreview> activeGesturePreview() const;

  /// Returns bounds chrome derived from the gesture's immutable start bounds
  /// and current transform. Available for every moved drag so the overlay does
  /// not need to walk live geometry while the render worker is active.
  [[nodiscard]] std::optional<ActiveTransformBoundsPreview> activeTransformBoundsPreview() const;

  /// A transient "this element is locked, you can't select it" flash on the
  /// rejected element's outline. `intensity` fades from 1 → 0 over the flash
  /// duration.
  struct LockedRejectionFlash {
    svg::SVGElement element;  ///< The element whose selection was rejected.
    float intensity = 0.0f;   ///< Fade intensity in (0, 1].
  };

  /// The active locked-selection rejection flash, or `std::nullopt`. The overlay
  /// renderer draws this as a red outline on the rejected (locked) element.
  [[nodiscard]] std::optional<LockedRejectionFlash> lockedRejectionFlash() const;

  /// Advance the locked-rejection flash fade by @p deltaSeconds, clearing it
  /// once fully faded. Called once per frame by the editor shell.
  void tickLockedRejectionFlash(float deltaSeconds);

private:
  /// Per-element bookkeeping for one participant in a drag. Carries the
  /// start transform (for computing `startTransform * translate(delta)`
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
    /// registry itself - reads here racing with a concurrent background
    /// render would hit `entt::fast_mod` assertions when the worker is
    /// resizing a dense-map bucket array.
    std::optional<RcString> sourceTransformAttributeValue;
  };

  struct DragState {
    enum class GestureKind {
      Move,
      Resize,
      Rotate,
    };

    /// Primary drag participant - the element that was under the cursor on
    /// mouse-down. Always populated. The compositor-preview fast path
    /// (when a single-element drag is composited) runs against this one.
    PerElementDrag primary;

    /// Additional elements that move in lockstep with the primary. Empty
    /// for a single-element drag. Populated when mouse-down hits an
    /// already-selected element and the current selection has more than
    /// one entry - classic "grab an item in the current selection, move
    /// them all" design-tool behavior.
    std::vector<PerElementDrag> extras;

    GestureKind gestureKind = GestureKind::Move;
    SelectionTransformCorner corner = SelectionTransformCorner::TopLeft;
    Vector2d startDocumentPoint;
    Box2d startBoundsDoc;
    Vector2d centerDocumentPoint = Vector2d::Zero();
    double startAngleRadians = 0.0;
    /// Generation copied into \ref ActiveDragPreview for this gesture.
    std::uint64_t generation = 0;
    /// Current drag delta in document coordinates, used for compositor preview.
    Vector2d currentDocumentDelta = Vector2d::Zero();
    /// Current affine transform from the gesture-start document geometry
    /// to the active preview geometry.
    Transform2d currentDocumentFromStartDocument = Transform2d();
    /// Whether any `onMouseMove` has fired since `onMouseDown`. A
    /// click-without-drag shouldn't leave an undo entry behind.
    bool hasMoved = false;
  };

  /// Active marquee drag. Records the start point (the document
  /// position of the `onMouseDown` that hit empty space), the
  /// current point (updated on every `onMouseMove`), and whether
  /// Shift was held - additive marquee appends to the existing
  /// selection instead of replacing it.
  struct MarqueeState {
    Vector2d startDocumentPoint;
    Vector2d currentDocumentPoint;
    bool additive = false;
  };

  /// Begin a locked-selection rejection flash on @p element.
  void requestLockedRejectionFlash(const svg::SVGElement& element);

  /// Duration of the locked-selection rejection flash.
  static constexpr float kLockedRejectionFlashSeconds = 0.5f;

  std::optional<DragState> dragState_;
  std::optional<MarqueeState> marqueeState_;
  std::optional<CompletedDragWriteback> completedDragWriteback_;
  std::uint64_t nextDragGeneration_ = 1;
  /// Element whose selection was rejected for being locked, flashed red.
  std::optional<svg::SVGElement> lockedFlashElement_;
  /// Remaining flash time in seconds; counts down to 0 then clears.
  float lockedFlashRemainingSeconds_ = 0.0f;
};

}  // namespace donner::editor
